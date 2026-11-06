#include "TuiApp.h"
#include <QCoreApplication>
#include <QDateTime>
#include <sys/ioctl.h>
#include <unistd.h>

namespace gofi {

namespace {
// ANSI Escape Helpers
const QString ESC_CLEAR_HOME = QStringLiteral("\033[H");
const QString ESC_CLEAR_ALL  = QStringLiteral("\033[2J\033[H");
const QString ESC_HIDE_CUR   = QStringLiteral("\033[?25l");
const QString ESC_SHOW_CUR   = QStringLiteral("\033[?25h");
const QString ESC_ALT_ON     = QStringLiteral("\033[?1049h");
const QString ESC_ALT_OFF    = QStringLiteral("\033[?1049l");
const QString ESC_RESET      = QStringLiteral("\033[0m");
const QString ESC_BOLD       = QStringLiteral("\033[1m");
const QString ESC_DIM        = QStringLiteral("\033[2m");
const QString ESC_STRIKE     = QStringLiteral("\033[9m");

const QString FG_CYAN        = QStringLiteral("\033[38;5;81m");
const QString FG_GREEN       = QStringLiteral("\033[38;5;114m");
const QString FG_YELLOW      = QStringLiteral("\033[38;5;221m");
const QString FG_ORANGE      = QStringLiteral("\033[38;5;209m");
const QString FG_RED         = QStringLiteral("\033[38;5;203m");
const QString FG_PURPLE      = QStringLiteral("\033[38;5;177m");
const QString FG_GRAY        = QStringLiteral("\033[38;5;244m");
const QString FG_WHITE       = QStringLiteral("\033[38;5;255m");

const QString BG_HEADER      = QStringLiteral("\033[48;5;236m");
const QString BG_SELECTED    = QStringLiteral("\033[48;5;238m");
const QString BG_ACTIVE_TAB  = QStringLiteral("\033[48;5;31m\033[38;5;255m\033[1m");
const QString BG_MODE_NORM   = QStringLiteral("\033[48;5;31m\033[38;5;255m\033[1m");
const QString BG_MODE_INS    = QStringLiteral("\033[48;5;28m\033[38;5;255m\033[1m");
const QString BG_MODE_CMD    = QStringLiteral("\033[48;5;130m\033[38;5;255m\033[1m");
QString statusLabel(const QString& status) {
    if (status == QStringLiteral("in_progress")) return QStringLiteral("In Progress");
    if (status == QStringLiteral("paused")) return QStringLiteral("Paused");
    if (status == QStringLiteral("done")) return QStringLiteral("Done");
    if (status == QStringLiteral("canceled")) return QStringLiteral("Canceled / Rejected");
    return QStringLiteral("To-Do");
}
} // namespace

TuiApp::TuiApp(AppController* controller, QObject* parent)
    : QObject(parent)
    , m_app(controller)
{
    connect(m_app, &AppController::currentListChanged, this, [this]() {
        m_selectedRow = 0;
        m_scrollOffset = 0;
        m_detailsScroll = 0;
        render();
    });
    connect(m_app, &AppController::activeTodoRowChanged, this, &TuiApp::render);
    connect(m_app->timer(), &TaskTimer::timerUpdated, this, &TuiApp::render);
    connect(m_app->timer(), &TaskTimer::runningChanged, this, &TuiApp::render);
    connect(m_app->timer(), &TaskTimer::breakActiveChanged, this, &TuiApp::render);
    connect(m_app->issueTracker(), &IssueTrackerService::syncingChanged, this, &TuiApp::render);
    connect(m_app->issueTracker(), &IssueTrackerService::statusMessageChanged, this, [this]() {
        m_statusMessage = m_app->issueTracker()->lastStatusMessage();
        render();
    });
    connect(m_app->todoProxyModel(), &QAbstractItemModel::modelReset, this, &TuiApp::render);
    connect(m_app->todoProxyModel(), &QAbstractItemModel::rowsInserted, this, &TuiApp::render);
    connect(m_app->todoProxyModel(), &QAbstractItemModel::rowsRemoved, this, &TuiApp::render);
    connect(m_app->todoProxyModel(), &QAbstractItemModel::dataChanged, this, &TuiApp::render);
    connect(m_app->doneProxyModel(), &QAbstractItemModel::modelReset, this, &TuiApp::render);
    connect(m_app->doneProxyModel(), &QAbstractItemModel::rowsInserted, this, &TuiApp::render);
    connect(m_app->doneProxyModel(), &QAbstractItemModel::rowsRemoved, this, &TuiApp::render);
    connect(m_app->doneProxyModel(), &QAbstractItemModel::dataChanged, this, &TuiApp::render);

    connect(&m_resizeTimer, &QTimer::timeout, this, [this]() {
        const int oldR = m_rows;
        const int oldC = m_cols;
        updateTerminalSize();
        if (m_rows != oldR || m_cols != oldC) {
            render();
        }
    });
}

TuiApp::~TuiApp() {
    disableRawMode();
}

bool TuiApp::start() {
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
        return false;
    }
    enableRawMode();
    updateTerminalSize();
    switchViewTab(0);

    m_stdinNotifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(m_stdinNotifier, &QSocketNotifier::activated, this, &TuiApp::onStdinActivated);
    m_resizeTimer.start(250);

    m_statusMessage = tr("Vim keys: j/k move, Space toggle done, s status, o add, i edit, t timer, r sync, L lists, / search, : cmd, ? help");
    render();
    return true;
}

void TuiApp::enableRawMode() {
    if (m_rawModeActive) return;
    if (::tcgetattr(STDIN_FILENO, &m_origTermios) == 0) {
        struct termios raw = m_origTermios;
        raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
        raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        m_rawModeActive = true;
    }
    const QByteArray initSeq = (ESC_ALT_ON + ESC_HIDE_CUR + ESC_CLEAR_ALL).toUtf8();
    (void)::write(STDOUT_FILENO, initSeq.constData(), static_cast<size_t>(initSeq.size()));
}

void TuiApp::disableRawMode() {
    if (!m_rawModeActive) return;
    const QByteArray exitSeq = (ESC_RESET + ESC_SHOW_CUR + ESC_ALT_OFF).toUtf8();
    (void)::write(STDOUT_FILENO, exitSeq.constData(), static_cast<size_t>(exitSeq.size()));
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &m_origTermios);
    m_rawModeActive = false;
}

void TuiApp::updateTerminalSize() {
    struct winsize ws {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        m_rows = qMax(14, static_cast<int>(ws.ws_row));
        m_cols = qMax(50, static_cast<int>(ws.ws_col));
    }
}

bool TuiApp::isClosedTab() const {
    return m_viewTab >= 4;
}

TaskFilterProxyModel* TuiApp::currentProxy() const {
    return isClosedTab() ? m_app->doneProxyModel() : m_app->todoProxyModel();
}

void TuiApp::switchViewTab(int tabIndex) {
    m_viewTab = qBound(0, tabIndex, 5);
    if (m_viewTab == 0) {
        m_app->todoProxyModel()->setStatusFilter(QString());
    } else if (m_viewTab == 1) {
        m_app->todoProxyModel()->setStatusFilter(QStringLiteral("todo"));
    } else if (m_viewTab == 2) {
        m_app->todoProxyModel()->setStatusFilter(QStringLiteral("in_progress"));
    } else if (m_viewTab == 3) {
        m_app->todoProxyModel()->setStatusFilter(QStringLiteral("paused"));
    } else if (m_viewTab == 4) {
        m_app->doneProxyModel()->setStatusFilter(QStringLiteral("done"));
    } else if (m_viewTab == 5) {
        m_app->doneProxyModel()->setStatusFilter(QStringLiteral("canceled"));
    }
    m_detailsScroll = 0;
    clampSelection();
}

void TuiApp::clampSelection() {
    const int count = currentProxy()->rowCount();
    if (count <= 0) {
        m_selectedRow = 0;
        m_scrollOffset = 0;
        return;
    }
    m_selectedRow = qBound(0, m_selectedRow, count - 1);
}

void TuiApp::onStdinActivated() {
    char buf[128];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) return;

    const QByteArray raw(buf, static_cast<int>(n));
    // Dispatch escape sequences or individual UTF-8 characters
    if (raw.startsWith('\033')) {
        handleKeySequence(raw);
    } else {
        const QString utf8Str = QString::fromUtf8(raw);
        for (int i = 0; i < utf8Str.size(); ++i) {
            const QString ch = utf8Str.mid(i, 1);
            handleKeySequence(ch.toUtf8());
        }
    }
    render();
}

void TuiApp::handleKeySequence(const QByteArray& seq) {
    // Ctrl+C quits cleanly from any mode
    if (seq == QByteArray("\x03")) {
        QCoreApplication::quit();
        return;
    }

    switch (m_mode) {
    case Mode::Normal:
        handleNormalKey(seq);
        break;
    case Mode::InsertTask:
    case Mode::EditTask:
    case Mode::EditDetails:
    case Mode::Search:
    case Mode::Command:
        handleInputLineKey(seq);
        break;
    case Mode::StatusPicker:
        handleStatusPickerKey(seq);
        break;
    case Mode::ListPicker:
        handleListPickerKey(seq);
        break;
    case Mode::Help:
        handleHelpKey(seq);
        break;
    }
}

void TuiApp::handleNormalKey(const QByteArray& seq) {
    const int count = currentProxy()->rowCount();

    // Handle 2-key Vim combos (gg, gt, gT, dd, cw)
    if (m_pendingKey != 0) {
        const char prev = m_pendingKey;
        m_pendingKey = 0;
        if (prev == 'g' && seq == "g") {
            m_selectedRow = 0;
            m_detailsScroll = 0;
            return;
        }
        if (prev == 'g' && seq == "t") {
            switchViewTab((m_viewTab + 1) % 6);
            return;
        }
        if (prev == 'g' && seq == "T") {
            switchViewTab((m_viewTab + 5) % 6);
            return;
        }
        if (prev == 'd' && seq == "d") {
            if (count > 0) {
                m_app->removeTask(isClosedTab(), m_selectedRow);
                clampSelection();
                m_statusMessage = tr("Task deleted.");
            }
            return;
        }
        if (prev == 'c' && (seq == "w" || seq == "c")) {
            if (count > 0) {
                const QModelIndex idx = currentProxy()->index(m_selectedRow, 0);
                m_inputBuffer = currentProxy()->data(idx, TaskStoreModel::DescriptionRole).toString();
                m_inputCursor = static_cast<int>(m_inputBuffer.size());
                m_mode = Mode::EditTask;
            }
            return;
        }
    }

    if (seq == "g" || seq == "d" || seq == "c") {
        m_pendingKey = seq.at(0);
        return;
    }

    // Navigation: j / Down
    if (seq == "j" || seq == "\033[B") {
        if (count > 0) {
            m_selectedRow = qMin(count - 1, m_selectedRow + 1);
            m_detailsScroll = 0;
        }
        return;
    }
    // Navigation: k / Up
    if (seq == "k" || seq == "\033[A") {
        if (count > 0) {
            m_selectedRow = qMax(0, m_selectedRow - 1);
            m_detailsScroll = 0;
        }
        return;
    }
    // Jump to bottom: G
    if (seq == "G") {
        if (count > 0) {
            m_selectedRow = count - 1;
            m_detailsScroll = 0;
        }
        return;
    }
    // Half-page down: Ctrl+D
    if (seq == "\x04") {
        if (count > 0) m_selectedRow = qMin(count - 1, m_selectedRow + 8);
        return;
    }
    // Half-page up: Ctrl+U
    if (seq == "\x15") {
        if (count > 0) m_selectedRow = qMax(0, m_selectedRow - 8);
        return;
    }
    // Scroll details pane down/up: Ctrl+E / Ctrl+Y or PgDn / PgUp
    if (seq == "\x05" || seq == "\033[6~") {
        ++m_detailsScroll;
        return;
    }
    if (seq == "\x19" || seq == "\033[5~") {
        m_detailsScroll = qMax(0, m_detailsScroll - 1);
        return;
    }

    // Reorder task down/up: J / K
    if (seq == "J") {
        if (count > 1 && m_selectedRow < count - 1) {
            m_app->moveTaskRow(isClosedTab(), m_selectedRow, m_selectedRow + 1);
            ++m_selectedRow;
        }
        return;
    }
    if (seq == "K") {
        if (count > 1 && m_selectedRow > 0) {
            m_app->moveTaskRow(isClosedTab(), m_selectedRow, m_selectedRow - 1);
            --m_selectedRow;
        }
        return;
    }

    // Switch status tabs: h / l / Left / Right / Tab / 0..6
    if (seq == "h" || seq == "\033[D") {
        switchViewTab((m_viewTab + 5) % 6);
        return;
    }
    if (seq == "l" || seq == "\033[C" || seq == "\t") {
        switchViewTab((m_viewTab + 1) % 6);
        return;
    }
    if (seq.size() == 1 && seq.at(0) >= '1' && seq.at(0) <= '6') {
        switchViewTab(seq.at(0) - '1');
        return;
    }
    if (seq == "0") {
        switchViewTab(0);
        return;
    }

    // Toggle task Done: Space or x
    if (seq == " " || seq == "x") {
        if (count > 0) {
            m_app->toggleTaskDone(isClosedTab(), m_selectedRow);
            clampSelection();
        }
        return;
    }

    // Open status picker: s
    if (seq == "s") {
        if (count > 0) {
            m_mode = Mode::StatusPicker;
        }
        return;
    }

    // Add new task: o or a
    if (seq == "o" || seq == "a" || seq == "A") {
        m_inputBuffer.clear();
        m_inputCursor = 0;
        m_mode = Mode::InsertTask;
        return;
    }

    // Edit selected task title: i or e
    if (seq == "i" || seq == "e") {
        if (count > 0) {
            const QModelIndex idx = currentProxy()->index(m_selectedRow, 0);
            m_inputBuffer = currentProxy()->data(idx, TaskStoreModel::DescriptionRole).toString();
            m_inputCursor = static_cast<int>(m_inputBuffer.size());
            m_mode = Mode::EditTask;
        }
        return;
    }

    // Edit selected task details: E
    if (seq == "E") {
        if (count > 0) {
            const QModelIndex idx = currentProxy()->index(m_selectedRow, 0);
            m_inputBuffer = currentProxy()->data(idx, TaskStoreModel::DetailsRole).toString().replace(QLatin1Char('\n'), QStringLiteral(" \\n "));
            m_inputCursor = static_cast<int>(m_inputBuffer.size());
            m_mode = Mode::EditDetails;
        }
        return;
    }

    // Select task for Pomodoro timer: Enter
    if (seq == "\r" || seq == "\n") {
        if (!isClosedTab() && count > 0) {
            const int srcRow = m_app->todoProxyModel()->mapToSourceRow(m_selectedRow);
            if (srcRow >= 0) {
                m_app->setActiveTodoRow(srcRow);
                m_statusMessage = tr("Selected active Pomodoro task.");
            }
        }
        return;
    }

    // Start / Pause Pomodoro timer: t or S
    if (seq == "t" || seq == "S") {
        if (!isClosedTab() && count > 0) {
            const int srcRow = m_app->todoProxyModel()->mapToSourceRow(m_selectedRow);
            if (srcRow >= 0 && m_app->activeTodoRow() != srcRow && !m_app->timer()->isRunning()) {
                m_app->setActiveTodoRow(srcRow);
            }
        }
        m_app->timer()->toggleRunning();
        return;
    }

    // Skip break / timer phase: B
    if (seq == "B") {
        m_app->timer()->skipIteration();
        return;
    }
    // Reset current timer phase: R
    if (seq == "R") {
        m_app->timer()->reset();
        return;
    }

    // Sync current list (Jira / YouTrack / SQLite): r or Ctrl+R
    if (seq == "r" || seq == "\x12") {
        m_statusMessage = tr("Syncing list '%1'...").arg(m_app->currentListName());
        m_app->syncCurrentList();
        return;
    }

    // Switch lists: L (picker) or [ / ]
    if (seq == "L") {
        m_listPickerRow = qMax(0, m_app->listManager()->indexOfId(m_app->currentListId()));
        m_mode = Mode::ListPicker;
        return;
    }
    if (seq == "[") {
        const int nLists = m_app->listManager()->rowCount();
        if (nLists > 0) {
            const int cur = qMax(0, m_app->listManager()->indexOfId(m_app->currentListId()));
            const int prev = (cur + nLists - 1) % nLists;
            m_app->selectListById(m_app->listManager()->entryAt(prev).id);
        }
        return;
    }
    if (seq == "]") {
        const int nLists = m_app->listManager()->rowCount();
        if (nLists > 0) {
            const int cur = qMax(0, m_app->listManager()->indexOfId(m_app->currentListId()));
            const int next = (cur + 1) % nLists;
            m_app->selectListById(m_app->listManager()->entryAt(next).id);
        }
        return;
    }

    // Toggle preview pane: p
    if (seq == "p") {
        m_showPreview = !m_showPreview;
        return;
    }

    // Search: /
    if (seq == "/") {
        m_inputBuffer = m_app->todoProxyModel()->filterQuery();
        m_inputCursor = static_cast<int>(m_inputBuffer.size());
        m_mode = Mode::Search;
        return;
    }

    // Clear search: Esc in Normal mode
    if (seq == "\033") {
        m_app->todoProxyModel()->setFilterQuery(QString());
        m_app->doneProxyModel()->setFilterQuery(QString());
        m_statusMessage.clear();
        clampSelection();
        return;
    }

    // Ex Command line: :
    if (seq == ":") {
        m_inputBuffer.clear();
        m_inputCursor = 0;
        m_mode = Mode::Command;
        return;
    }

    // Help overlay: ?
    if (seq == "?") {
        m_mode = Mode::Help;
        return;
    }

    // Quit shortcut: q or ZZ
    if (seq == "q") {
        QCoreApplication::quit();
    }
}

void TuiApp::handleInputLineKey(const QByteArray& seq) {
    // Esc cancels
    if (seq == "\033") {
        if (m_mode == Mode::Search) {
            m_app->todoProxyModel()->setFilterQuery(QString());
            m_app->doneProxyModel()->setFilterQuery(QString());
        }
        m_mode = Mode::Normal;
        return;
    }
    // Enter commits
    if (seq == "\r" || seq == "\n") {
        const QString text = m_inputBuffer.trimmed();
        const Mode prevMode = m_mode;
        m_mode = Mode::Normal;

        if (prevMode == Mode::InsertTask) {
            if (!text.isEmpty()) {
                m_app->addNewTask(text);
                m_statusMessage = tr("Added task: %1").arg(text);
                clampSelection();
            }
        } else if (prevMode == Mode::EditTask) {
            if (!text.isEmpty() && currentProxy()->rowCount() > 0) {
                m_app->updateTaskText(isClosedTab(), m_selectedRow, text);
                m_statusMessage = tr("Updated task.");
            }
        } else if (prevMode == Mode::EditDetails) {
            if (currentProxy()->rowCount() > 0) {
                QString restored = m_inputBuffer;
                restored.replace(QStringLiteral(" \\n "), QStringLiteral("\n"));
                m_app->updateTaskDetails(isClosedTab(), m_selectedRow, restored.trimmed());
                m_statusMessage = tr("Updated task description.");
            }
        } else if (prevMode == Mode::Search) {
            m_app->todoProxyModel()->setFilterQuery(text);
            m_app->doneProxyModel()->setFilterQuery(text);
            clampSelection();
        } else if (prevMode == Mode::Command) {
            executeExCommand(text);
        }
        return;
    }
    // Backspace
    if (seq == "\x7f" || seq == "\x08") {
        if (m_inputCursor > 0) {
            m_inputBuffer.remove(m_inputCursor - 1, 1);
            --m_inputCursor;
            if (m_mode == Mode::Search) {
                m_app->todoProxyModel()->setFilterQuery(m_inputBuffer);
                m_app->doneProxyModel()->setFilterQuery(m_inputBuffer);
                clampSelection();
            }
        }
        return;
    }
    // Left / Right arrows in input line
    if (seq == "\033[D") {
        m_inputCursor = qMax(0, m_inputCursor - 1);
        return;
    }
    if (seq == "\033[C") {
        m_inputCursor = qMin(static_cast<int>(m_inputBuffer.size()), m_inputCursor + 1);
        return;
    }
    // Ctrl+U clears input line
    if (seq == "\x15") {
        m_inputBuffer.clear();
        m_inputCursor = 0;
        if (m_mode == Mode::Search) {
            m_app->todoProxyModel()->setFilterQuery(QString());
            m_app->doneProxyModel()->setFilterQuery(QString());
        }
        return;
    }

    // Printable UTF-8 character
    if (!seq.startsWith('\033')) {
        const QString ch = QString::fromUtf8(seq);
        if (!ch.isEmpty() && ch.at(0).isPrint()) {
            m_inputBuffer.insert(m_inputCursor, ch);
            m_inputCursor += static_cast<int>(ch.size());
            if (m_mode == Mode::Search) {
                m_app->todoProxyModel()->setFilterQuery(m_inputBuffer);
                m_app->doneProxyModel()->setFilterQuery(m_inputBuffer);
                clampSelection();
            }
        }
    }
}

void TuiApp::handleStatusPickerKey(const QByteArray& seq) {
    if (seq == "\033" || seq == "q") {
        m_mode = Mode::Normal;
        return;
    }
    QString targetStatus;
    if (seq == "1" || seq == "t") targetStatus = QStringLiteral("todo");
    else if (seq == "2" || seq == "i") targetStatus = QStringLiteral("in_progress");
    else if (seq == "3" || seq == "p") targetStatus = QStringLiteral("paused");
    else if (seq == "4" || seq == "d") targetStatus = QStringLiteral("done");
    else if (seq == "5" || seq == "c") targetStatus = QStringLiteral("canceled");

    if (!targetStatus.isEmpty()) {
        m_mode = Mode::Normal;
        if (currentProxy()->rowCount() > 0) {
            m_app->changeTaskStatus(isClosedTab(), m_selectedRow, targetStatus);
            clampSelection();
            m_statusMessage = tr("Status changed to '%1'.").arg(statusLabel(targetStatus));
        }
    }
}

void TuiApp::handleListPickerKey(const QByteArray& seq) {
    const int nLists = m_app->listManager()->rowCount();
    if (seq == "\033" || seq == "q" || seq == "L") {
        m_mode = Mode::Normal;
        return;
    }
    if (seq == "j" || seq == "\033[B") {
        if (nLists > 0) m_listPickerRow = qMin(nLists - 1, m_listPickerRow + 1);
        return;
    }
    if (seq == "k" || seq == "\033[A") {
        if (nLists > 0) m_listPickerRow = qMax(0, m_listPickerRow - 1);
        return;
    }
    if (seq == "\r" || seq == "\n") {
        m_mode = Mode::Normal;
        if (m_listPickerRow >= 0 && m_listPickerRow < nLists) {
            const TodoListEntry entry = m_app->listManager()->entryAt(m_listPickerRow);
            m_app->selectListById(entry.id);
            m_statusMessage = tr("Switched to list '%1' (%2).").arg(entry.name, entry.provider);
        }
    }
}

void TuiApp::handleHelpKey(const QByteArray& seq) {
    if (seq == "\033" || seq == "q" || seq == "?" || seq == "\r" || seq == "\n") {
        m_mode = Mode::Normal;
    }
}

void TuiApp::executeExCommand(const QString& cmdLine) {
    if (cmdLine.isEmpty()) return;
    if (cmdLine == QStringLiteral("q") || cmdLine == QStringLiteral("quit") || cmdLine == QStringLiteral("qa")) {
        QCoreApplication::quit();
        return;
    }
    if (cmdLine == QStringLiteral("w") || cmdLine == QStringLiteral("sync") || cmdLine == QStringLiteral("e")) {
        m_statusMessage = tr("Syncing '%1'...").arg(m_app->currentListName());
        m_app->syncCurrentList();
        return;
    }
    if (cmdLine == QStringLiteral("wq") || cmdLine == QStringLiteral("x")) {
        QCoreApplication::quit();
        return;
    }
    if (cmdLine == QStringLiteral("sort")) {
        m_app->sortAllTasks();
        m_statusMessage = tr("Tasks sorted by priority.");
        return;
    }
    if (cmdLine == QStringLiteral("clear")) {
        m_app->clearDoneTasks();
        clampSelection();
        m_statusMessage = tr("Cleared completed tasks.");
        return;
    }
    if (cmdLine == QStringLiteral("noh") || cmdLine == QStringLiteral("nohlsearch")) {
        m_app->todoProxyModel()->setFilterQuery(QString());
        m_app->doneProxyModel()->setFilterQuery(QString());
        m_statusMessage = tr("Search filter cleared.");
        clampSelection();
        return;
    }
    if (cmdLine == QStringLiteral("list") || cmdLine == QStringLiteral("lists") || cmdLine == QStringLiteral("ls")) {
        m_listPickerRow = qMax(0, m_app->listManager()->indexOfId(m_app->currentListId()));
        m_mode = Mode::ListPicker;
        return;
    }
    if (cmdLine == QStringLiteral("help") || cmdLine == QStringLiteral("h")) {
        m_mode = Mode::Help;
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("add "))) {
        const QString taskText = cmdLine.mid(4).trimmed();
        if (!taskText.isEmpty()) {
            m_app->addNewTask(taskText);
            m_statusMessage = tr("Added task: %1").arg(taskText);
            clampSelection();
        }
        return;
    }
    m_statusMessage = tr("Unknown command: :%1 (try :help, :sync, :sort, :clear, :list, :q)").arg(cmdLine);
}

QString TuiApp::truncateVisible(const QString& str, int maxCols) {
    if (maxCols <= 0) return {};
    if (str.size() <= maxCols) return str;
    if (maxCols <= 1) return str.left(maxCols);
    return str.left(maxCols - 1) + QStringLiteral("…");
}

QString TuiApp::padRight(const QString& str, int width) {
    if (str.size() >= width) return truncateVisible(str, width);
    return str + QString(width - static_cast<int>(str.size()), QLatin1Char(' '));
}

QString TuiApp::formatSeconds(uint secs) {
    const uint h = secs / 3600U;
    const uint m = (secs / 60U) % 60U;
    const uint s = secs % 60U;
    if (h > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(h)
            .arg(m, 2, 10, QLatin1Char('0'))
            .arg(s, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

void TuiApp::render() {
    if (!m_rawModeActive) return;
    clampSelection();

    QString out;
    out.reserve(16384);
    out += ESC_CLEAR_HOME;

    // -------------------------------------------------------------------------
    // Row 1: Top Header Bar (App + Active List + Sync state)
    // -------------------------------------------------------------------------
    const QString listBadge = QStringLiteral(" 🍅 Go for It Qt [VIM TUI] │ 📋 %1 (%2) ")
        .arg(m_app->currentListName(), m_app->currentListProvider());
    const QString syncInfo = m_app->issueTracker()->isSyncing()
        ? QStringLiteral("⟳ Syncing...")
        : QStringLiteral("[L:Lists] [r:Sync] [?:Help]");
    const int headerGap = qMax(1, m_cols - static_cast<int>(listBadge.size()) - static_cast<int>(syncInfo.size()) - 1);
    out += BG_HEADER + FG_CYAN + ESC_BOLD + listBadge + ESC_RESET + BG_HEADER
        + QString(headerGap, QLatin1Char(' ')) + FG_GRAY + syncInfo + QLatin1Char(' ') + ESC_RESET + QStringLiteral("\033[K\r\n");

    // -------------------------------------------------------------------------
    // Row 2: Pomodoro Timer Banner
    // -------------------------------------------------------------------------
    TaskTimer* timer = m_app->timer();
    const QString stateBadge = timer->isRunning()
        ? (timer->isBreakActive() ? (FG_GREEN + QStringLiteral("☕ BREAK RUNNING")) : (FG_ORANGE + QStringLiteral("▶ WORK RUNNING")))
        : (timer->isBreakActive() ? (FG_GREEN + QStringLiteral("☕ BREAK PAUSED"))  : (FG_YELLOW + QStringLiteral("⏸ TIMER READY")));
    const QString clockStr = formatSeconds(timer->remainingSeconds());
    const int barWidth = 16;
    const int filled = qBound(0, static_cast<int>(timer->progress() * barWidth), barWidth);
    const QString progBar = QStringLiteral("[")
        + QString(filled, QChar(0x2588))
        + QString(barWidth - filled, QChar(0x2591))
        + QStringLiteral("]");
    const QString activeTaskTitle = timer->hasActiveTask()
        ? truncateVisible(timer->activeTaskDescription(), qMax(15, m_cols - 52))
        : tr("No active task");

    out += QStringLiteral(" ") + ESC_BOLD + stateBadge + ESC_RESET
        + QStringLiteral("  ⏱ ") + ESC_BOLD + FG_WHITE + clockStr + ESC_RESET
        + QStringLiteral(" ") + FG_CYAN + progBar + ESC_RESET
        + QStringLiteral("  🎯 ") + FG_WHITE + activeTaskTitle + ESC_RESET + QStringLiteral("\033[K\r\n");

    // -------------------------------------------------------------------------
    // Row 3: View / Status Filter Tabs (1..6)
    // -------------------------------------------------------------------------
    const QStringList tabNames = {
        QStringLiteral(" 1:All Open "),
        QStringLiteral(" 2:To-Do "),
        QStringLiteral(" 3:In Progress "),
        QStringLiteral(" 4:Paused "),
        QStringLiteral(" 5:Done "),
        QStringLiteral(" 6:Canceled ")
    };
    out += QStringLiteral(" ");
    for (int i = 0; i < tabNames.size(); ++i) {
        if (i == m_viewTab) {
            out += BG_ACTIVE_TAB + tabNames.at(i) + ESC_RESET + QStringLiteral(" ");
        } else {
            out += FG_GRAY + tabNames.at(i) + ESC_RESET + QStringLiteral(" ");
        }
    }
    const QString filterStr = currentProxy()->filterQuery();
    if (!filterStr.isEmpty()) {
        out += FG_YELLOW + QStringLiteral(" 🔍 /%1").arg(filterStr) + ESC_RESET;
    }
    out += QStringLiteral("\033[K\r\n");

    // -------------------------------------------------------------------------
    // Row 4: Horizontal Divider
    // -------------------------------------------------------------------------
    out += FG_GRAY + QString(m_cols, QChar(0x2500)) + ESC_RESET + QStringLiteral("\033[K\r\n");

    // Calculate middle viewport height (rows 5 .. m_rows - 2)
    const int bodyRows = qMax(4, m_rows - 6);
    const int previewRows = m_showPreview ? qMax(4, bodyRows / 3) : 0;
    const int listRows = qMax(2, bodyRows - (m_showPreview ? (previewRows + 1) : 0));

    // Adjust scrollOffset so m_selectedRow is visible
    if (m_selectedRow < m_scrollOffset) {
        m_scrollOffset = m_selectedRow;
    } else if (m_selectedRow >= m_scrollOffset + listRows) {
        m_scrollOffset = m_selectedRow - listRows + 1;
    }

    // -------------------------------------------------------------------------
    // Main Task List Rows
    // -------------------------------------------------------------------------
    TaskFilterProxyModel* proxy = currentProxy();
    const int totalTasks = proxy->rowCount();

    if (m_mode == Mode::Help) {
        const QStringList helpLines = {
            QStringLiteral("  VIM NAVIGATION & ACTIONS"),
            QStringLiteral("  j / k / gg / G / Ctrl+d / Ctrl+u  Move cursor down / up / top / bottom / half-page"),
            QStringLiteral("  J / K (Shift+j / Shift+k)         Move selected task down / up in order"),
            QStringLiteral("  h / l or 1..6 or gt / gT          Switch status tab (All Open, To-Do, In Progress, Paused, Done, Canceled)"),
            QStringLiteral("  Space or x                        Toggle task Done / To-Do"),
            QStringLiteral("  s                                 Open Status Picker (1:To-Do, 2:In Progress, 3:Paused, 4:Done, 5:Canceled)"),
            QStringLiteral("  o / a                             Add new task (INSERT mode)"),
            QStringLiteral("  i / e / cw                        Edit selected task title inline"),
            QStringLiteral("  E (Shift+e)                       Edit selected task full description"),
            QStringLiteral("  dd                                Delete selected task"),
            QStringLiteral("  Enter                             Set selected task as active Pomodoro task"),
            QStringLiteral("  t / S / B / R                     Start/Pause timer (t), Skip phase (B), Reset phase (R)"),
            QStringLiteral("  r / Ctrl+r                        Sync current list with Jira / YouTrack"),
            QStringLiteral("  L or [ / ]                        Open List Picker (L) or cycle previous/next list ([ / ])"),
            QStringLiteral("  p / Ctrl+e / Ctrl+y               Toggle preview pane (p), scroll description/comments (Ctrl+e/y)"),
            QStringLiteral("  / and :                           Search tasks (/) or Ex commands (:sync, :sort, :clear, :list, :noh, :q)")
        };
        for (int r = 0; r < bodyRows; ++r) {
            if (r < helpLines.size()) {
                out += (r == 0 ? (FG_CYAN + ESC_BOLD) : FG_WHITE)
                    + truncateVisible(helpLines.at(r), m_cols) + ESC_RESET + QStringLiteral("\033[K\r\n");
            } else {
                out += QStringLiteral("\033[K\r\n");
            }
        }
    } else if (m_mode == Mode::ListPicker) {
        out += FG_CYAN + ESC_BOLD + QStringLiteral("  SELECT LIST (j/k to move, Enter to select, Esc to cancel):") + ESC_RESET + QStringLiteral("\033[K\r\n");
        const int nLists = m_app->listManager()->rowCount();
        for (int r = 1; r < bodyRows; ++r) {
            const int idx = r - 1;
            if (idx < nLists) {
                const TodoListEntry e = m_app->listManager()->entryAt(idx);
                const bool isCur = (e.id == m_app->currentListId());
                const bool isSel = (idx == m_listPickerRow);
                const QString prefix = isSel ? QStringLiteral("  > ") : QStringLiteral("    ");
                const QString line = QStringLiteral("%1[%2] %3 %4")
                    .arg(prefix, e.provider, e.name, isCur ? QStringLiteral("(active)") : QString());
                out += (isSel ? (BG_SELECTED + FG_WHITE + ESC_BOLD) : FG_WHITE)
                    + padRight(line, m_cols) + ESC_RESET + QStringLiteral("\033[K\r\n");
            } else {
                out += QStringLiteral("\033[K\r\n");
            }
        }
    } else {
        for (int r = 0; r < listRows; ++r) {
            const int rowIdx = m_scrollOffset + r;
            if (rowIdx >= totalTasks) {
                if (totalTasks == 0 && r == 0) {
                    out += FG_GRAY + QStringLiteral("   (No tasks in this view — press 'o' to add a task or 'r' to sync Jira/YouTrack)")
                        + ESC_RESET + QStringLiteral("\033[K\r\n");
                } else {
                    out += FG_GRAY + QStringLiteral(" ~") + ESC_RESET + QStringLiteral("\033[K\r\n");
                }
                continue;
            }

            const QModelIndex idx = proxy->index(rowIdx, 0);
            const bool isSelected = (rowIdx == m_selectedRow);
            const int srcRow = proxy->mapToSourceRow(rowIdx);
            const bool isActivePomodoro = (!isClosedTab() && srcRow == m_app->activeTodoRow());

            const QString status = proxy->data(idx, TaskStoreModel::StatusRole).toString();
            const quint8 prio = static_cast<quint8>(proxy->data(idx, TaskStoreModel::PriorityRole).toUInt());
            const QString desc = proxy->data(idx, TaskStoreModel::DescriptionRole).toString();
            const QString details = proxy->data(idx, TaskStoreModel::DetailsRole).toString();
            const QVariantList comments = proxy->data(idx, TaskStoreModel::CommentsRole).toList();
            const uint durSec = proxy->data(idx, TaskStoreModel::DurationRole).toUInt();
            const uint timerSec = proxy->data(idx, TaskStoreModel::TimerValueRole).toUInt();

            QString statusBadge = QStringLiteral("[ ]");
            QString statusColor = FG_WHITE;
            if (status == QStringLiteral("in_progress")) {
                statusBadge = QStringLiteral("[▶]");
                statusColor = FG_CYAN;
            } else if (status == QStringLiteral("paused")) {
                statusBadge = QStringLiteral("[⏸]");
                statusColor = FG_YELLOW;
            } else if (status == QStringLiteral("done")) {
                statusBadge = QStringLiteral("[✓]");
                statusColor = FG_GREEN;
            } else if (status == QStringLiteral("canceled")) {
                statusBadge = QStringLiteral("[✗]");
                statusColor = FG_RED;
            }

            QString prioStr = QStringLiteral("   ");
            QString prioColor = FG_GRAY;
            if (prio < 26) {
                prioStr = QStringLiteral("(%1)").arg(QChar('A' + prio));
                prioColor = (prio == 0) ? FG_RED : (prio == 1 ? FG_ORANGE : FG_YELLOW);
            }

            QString metaSuffix;
            if (!details.isEmpty()) metaSuffix += QStringLiteral(" 📄");
            if (!comments.isEmpty()) metaSuffix += QStringLiteral(" 💬%1").arg(comments.size());
            if (durSec > 0 || timerSec > 0) {
                metaSuffix += QStringLiteral(" [%1/%2]").arg(formatSeconds(timerSec), formatSeconds(durSec));
            }

            const QString cursorMark = isSelected ? QStringLiteral(" > ") : (isActivePomodoro ? QStringLiteral(" • ") : QStringLiteral("   "));
            const int availDescCols = qMax(10, m_cols - 12 - static_cast<int>(metaSuffix.size()));
            const QString cleanDesc = truncateVisible(desc, availDescCols);

            if (isSelected) {
                out += BG_SELECTED + FG_CYAN + ESC_BOLD + cursorMark + statusColor + statusBadge + QLatin1Char(' ')
                    + prioColor + prioStr + QLatin1Char(' ')
                    + FG_WHITE + cleanDesc
                    + FG_PURPLE + metaSuffix
                    + ESC_RESET + QStringLiteral("\033[K\r\n");
            } else {
                out += (isActivePomodoro ? FG_CYAN : FG_GRAY) + cursorMark + statusColor + statusBadge + QLatin1Char(' ')
                    + prioColor + prioStr + QLatin1Char(' ')
                    + (isClosedTab() ? (FG_GRAY + ESC_STRIKE) : FG_WHITE) + cleanDesc + ESC_RESET
                    + FG_GRAY + metaSuffix + ESC_RESET + QStringLiteral("\033[K\r\n");
            }
        }

        // ---------------------------------------------------------------------
        // Preview Pane (Full Description + Comments of Selected Task)
        // ---------------------------------------------------------------------
        if (m_showPreview) {
            out += FG_GRAY + QStringLiteral("── Preview (p: toggle, E: edit description, Ctrl+e/Ctrl+y: scroll) ")
                + QString(qMax(0, m_cols - 67), QChar(0x2500)) + ESC_RESET + QStringLiteral("\033[K\r\n");

            QStringList previewLines;
            if (totalTasks > 0 && m_selectedRow < totalTasks) {
                const QModelIndex idx = proxy->index(m_selectedRow, 0);
                const QString desc = proxy->data(idx, TaskStoreModel::DescriptionRole).toString();
                const QString details = proxy->data(idx, TaskStoreModel::DetailsRole).toString();
                const QVariantList comments = proxy->data(idx, TaskStoreModel::CommentsRole).toList();
                const QString status = proxy->data(idx, TaskStoreModel::StatusRole).toString();

                previewLines.append(ESC_BOLD + FG_CYAN + QStringLiteral("  %1  [%2]").arg(desc, statusLabel(status)) + ESC_RESET);
                if (!details.isEmpty()) {
                    for (const QString& rawLine : details.split(QLatin1Char('\n'))) {
                        previewLines.append(FG_WHITE + QStringLiteral("  ") + truncateVisible(rawLine, m_cols - 4) + ESC_RESET);
                    }
                } else {
                    previewLines.append(FG_GRAY + QStringLiteral("  (No detailed description — press 'E' to add)") + ESC_RESET);
                }

                if (!comments.isEmpty()) {
                    previewLines.append(FG_YELLOW + ESC_BOLD + QStringLiteral("  ── Comments (%1) ──").arg(comments.size()) + ESC_RESET);
                    for (const QVariant& cv : comments) {
                        const QVariantMap cm = cv.toMap();
                        previewLines.append(FG_PURPLE + QStringLiteral("  • %1 (%2):").arg(cm.value(QStringLiteral("author")).toString(), cm.value(QStringLiteral("created")).toString()) + ESC_RESET);
                        for (const QString& cl : cm.value(QStringLiteral("body")).toString().split(QLatin1Char('\n'))) {
                            previewLines.append(FG_GRAY + QStringLiteral("    ") + truncateVisible(cl, m_cols - 6) + ESC_RESET);
                        }
                    }
                }
            } else {
                previewLines.append(FG_GRAY + QStringLiteral("  (No task selected)") + ESC_RESET);
            }

            const int maxScroll = qMax(0, static_cast<int>(previewLines.size()) - previewRows);
            m_detailsScroll = qBound(0, m_detailsScroll, maxScroll);

            for (int r = 0; r < previewRows; ++r) {
                const int pIdx = m_detailsScroll + r;
                if (pIdx < previewLines.size()) {
                    out += previewLines.at(pIdx) + QStringLiteral("\033[K\r\n");
                } else {
                    out += QStringLiteral("\033[K\r\n");
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Row m_rows - 1: Vim Statusline
    // -------------------------------------------------------------------------
    QString modeTag = BG_MODE_NORM + QStringLiteral(" NORMAL ") + ESC_RESET;
    if (m_mode == Mode::InsertTask || m_mode == Mode::EditTask || m_mode == Mode::EditDetails) {
        modeTag = BG_MODE_INS + QStringLiteral(" INSERT ") + ESC_RESET;
    } else if (m_mode == Mode::Search) {
        modeTag = BG_MODE_CMD + QStringLiteral(" SEARCH ") + ESC_RESET;
    } else if (m_mode == Mode::Command) {
        modeTag = BG_MODE_CMD + QStringLiteral(" COMMAND ") + ESC_RESET;
    } else if (m_mode == Mode::StatusPicker) {
        modeTag = BG_MODE_CMD + QStringLiteral(" STATUS ") + ESC_RESET;
    } else if (m_mode == Mode::ListPicker) {
        modeTag = BG_MODE_CMD + QStringLiteral(" LISTS ") + ESC_RESET;
    } else if (m_mode == Mode::Help) {
        modeTag = BG_MODE_CMD + QStringLiteral(" HELP ") + ESC_RESET;
    }

    const QString posTag = QStringLiteral(" %1/%2 ")
        .arg(totalTasks > 0 ? (m_selectedRow + 1) : 0)
        .arg(totalTasks);
    out += modeTag + BG_HEADER + FG_WHITE
        + padRight(QStringLiteral(" %1 │ %2").arg(m_app->currentListName(), posTag), m_cols - 9)
        + ESC_RESET + QStringLiteral("\033[K\r\n");

    // -------------------------------------------------------------------------
    // Row m_rows: Command / Input / Status Message Bar
    // -------------------------------------------------------------------------
    if (m_mode == Mode::InsertTask) {
        out += FG_GREEN + ESC_BOLD + QStringLiteral("New task: ") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
    } else if (m_mode == Mode::EditTask) {
        out += FG_YELLOW + ESC_BOLD + QStringLiteral("Edit task: ") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
    } else if (m_mode == Mode::EditDetails) {
        out += FG_YELLOW + ESC_BOLD + QStringLiteral("Edit description: ") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
    } else if (m_mode == Mode::Search) {
        out += FG_CYAN + ESC_BOLD + QStringLiteral("/") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
    } else if (m_mode == Mode::Command) {
        out += FG_ORANGE + ESC_BOLD + QStringLiteral(":") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
    } else if (m_mode == Mode::StatusPicker) {
        out += FG_YELLOW + ESC_BOLD
            + QStringLiteral("Set status: [1/t] To-Do  [2/i] In Progress  [3/p] Paused  [4/d] Done  [5/c] Canceled  [Esc] Cancel")
            + ESC_RESET + QStringLiteral("\033[K");
    } else {
        out += FG_GRAY + truncateVisible(m_statusMessage, m_cols - 1) + ESC_RESET + QStringLiteral("\033[K");
    }

    const QByteArray frameBytes = out.toUtf8();
    (void)::write(STDOUT_FILENO, frameBytes.constData(), static_cast<size_t>(frameBytes.size()));
}

} // namespace gofi
