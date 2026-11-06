#include "TuiApp.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
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
    case Mode::EditSettingValue:
    case Mode::EditListField:
        handleInputLineKey(seq);
        break;
    case Mode::StatusPicker:
        handleStatusPickerKey(seq);
        break;
    case Mode::ListPicker:
        handleListPickerKey(seq);
        break;
    case Mode::ListEdit:
        handleListEditKey(seq);
        break;
    case Mode::Settings:
        handleSettingsKey(seq);
        break;
    case Mode::MarkdownView:
        handleMarkdownViewKey(seq);
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

    // Edit selected task details in $EDITOR / nvim / vim: E
    if (seq == "E") {
        if (count > 0) {
            editTaskDetailsInVim();
        }
        return;
    }

    // Add comment to selected task in $EDITOR / nvim / vim: C
    if (seq == "C") {
        if (count > 0) {
            addTaskCommentInVim();
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

    // Open Settings: O (Shift+o)
    if (seq == "O") {
        m_settingsRow = 0;
        m_mode = Mode::Settings;
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

    // Open Full-Screen Markdown Viewer for task description & comments: v or M
    if (seq == "v" || seq == "M") {
        openTaskMarkdownView();
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
        if (m_mode == Mode::EditSettingValue) {
            m_mode = Mode::Settings;
        } else if (m_mode == Mode::EditListField) {
            m_mode = Mode::ListEdit;
        } else {
            m_mode = Mode::Normal;
        }
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
        } else if (prevMode == Mode::EditSettingValue) {
            m_mode = Mode::Settings;
            if (m_settingsRow == 0) {
                m_app->settings()->setTaskDuration(qBound(60, text.toInt() * 60, 86400));
            } else if (m_settingsRow == 1) {
                m_app->settings()->setBreakDuration(qBound(60, text.toInt() * 60, 86400));
            } else if (m_settingsRow == 2) {
                m_app->settings()->setReminderTime(qBound(0, text.toInt(), 3600));
            } else if (m_settingsRow == 7 && !text.isEmpty()) {
                if (m_app->changeDatabasePath(text, true)) {
                    m_statusMessage = tr("Database moved to: %1").arg(m_app->databasePath());
                } else {
                    m_statusMessage = tr("Failed to move database to: %1").arg(text);
                }
            } else if (m_settingsRow == 8 && !text.isEmpty()) {
                if (m_app->exportBackup(text)) {
                    m_statusMessage = tr("Backup exported to: %1").arg(text);
                } else {
                    m_statusMessage = tr("Failed to export backup to: %1").arg(text);
                }
            }
        } else if (prevMode == Mode::EditListField) {
            m_mode = Mode::ListEdit;
            if (m_listEditRow == 0) m_editingListCfg[QStringLiteral("name")] = text;
            else if (m_listEditRow == 2) m_editingListCfg[QStringLiteral("jiraUrl")] = text;
            else if (m_listEditRow == 3) m_editingListCfg[QStringLiteral("jiraUser")] = text;
            else if (m_listEditRow == 4) m_editingListCfg[QStringLiteral("apiToken")] = text;
            else if (m_listEditRow == 5) m_editingListCfg[QStringLiteral("jiraJql")] = text;
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
    if (seq == "a" || seq == "o") {
        m_editingListCfg = m_app->listManager()->getListConfig(QString());
        m_editingListCfg[QStringLiteral("name")] = QStringLiteral("New List");
        m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Jira");
        m_listEditRow = 0;
        m_mode = Mode::ListEdit;
        return;
    }
    if ((seq == "e" || seq == "i") && m_listPickerRow >= 0 && m_listPickerRow < nLists) {
        const TodoListEntry entry = m_app->listManager()->entryAt(m_listPickerRow);
        m_editingListCfg = m_app->listManager()->getListConfig(entry.id);
        m_listEditRow = 0;
        m_mode = Mode::ListEdit;
        return;
    }
    if (seq == "d" && nLists > 1 && m_listPickerRow >= 0 && m_listPickerRow < nLists) {
        const TodoListEntry entry = m_app->listManager()->entryAt(m_listPickerRow);
        m_app->listManager()->removeListById(entry.id);
        m_listPickerRow = qBound(0, m_listPickerRow, m_app->listManager()->rowCount() - 1);
        m_statusMessage = tr("Removed list '%1'.").arg(entry.name);
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

void TuiApp::handleListEditKey(const QByteArray& seq) {
    if (seq == "\033" || seq == "q") {
        m_mode = Mode::ListPicker;
        return;
    }
    if (seq == "j" || seq == "\033[B") {
        m_listEditRow = qMin(7, m_listEditRow + 1);
        return;
    }
    if (seq == "k" || seq == "\033[A") {
        m_listEditRow = qMax(0, m_listEditRow - 1);
        return;
    }
    if (seq == " " || seq == "\r" || seq == "\n" || seq == "e" || seq == "i" || seq == "h" || seq == "l") {
        if (m_listEditRow == 1) {
            // Cycle provider: SQLite -> Jira -> YouTrack
            const QString cur = m_editingListCfg.value(QStringLiteral("provider")).toString();
            if (cur == QStringLiteral("SQLite")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Jira");
            else if (cur == QStringLiteral("Jira")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("YouTrack");
            else m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("SQLite");
            return;
        }
        if (m_listEditRow == 6) {
            m_editingListCfg[QStringLiteral("autoWorklog")] = !m_editingListCfg.value(QStringLiteral("autoWorklog"), true).toBool();
            return;
        }
        if (m_listEditRow == 7) {
            // Save & Activate List
            const QVariantMap check = m_app->listManager()->validateAndCheckConflicts(m_editingListCfg);
            if (!check.value(QStringLiteral("valid")).toBool()) {
                m_statusMessage = check.value(QStringLiteral("error")).toString();
                return;
            }
            const QString savedId = m_app->listManager()->saveListConfig(m_editingListCfg, QStringLiteral("keep"));
            m_app->selectListById(savedId);
            m_mode = Mode::Normal;
            m_statusMessage = tr("Saved and activated list '%1'.").arg(m_editingListCfg.value(QStringLiteral("name")).toString());
            return;
        }
        // Text fields (0, 2, 3, 4, 5)
        if (m_listEditRow == 0) m_inputBuffer = m_editingListCfg.value(QStringLiteral("name")).toString();
        else if (m_listEditRow == 2) m_inputBuffer = m_editingListCfg.value(QStringLiteral("jiraUrl")).toString();
        else if (m_listEditRow == 3) m_inputBuffer = m_editingListCfg.value(QStringLiteral("jiraUser")).toString();
        else if (m_listEditRow == 4) m_inputBuffer = m_editingListCfg.value(QStringLiteral("apiToken")).toString();
        else if (m_listEditRow == 5) m_inputBuffer = m_editingListCfg.value(QStringLiteral("jiraJql")).toString();
        m_inputCursor = static_cast<int>(m_inputBuffer.size());
        m_mode = Mode::EditListField;
    }
}

void TuiApp::handleSettingsKey(const QByteArray& seq) {
    if (seq == "\033" || seq == "q" || seq == "O") {
        m_mode = Mode::Normal;
        return;
    }
    if (seq == "j" || seq == "\033[B") {
        m_settingsRow = qMin(8, m_settingsRow + 1);
        return;
    }
    if (seq == "k" || seq == "\033[A") {
        m_settingsRow = qMax(0, m_settingsRow - 1);
        return;
    }
    SettingsManager* s = m_app->settings();
    if (seq == "h" || seq == "-" || seq == "\033[D") {
        if (m_settingsRow == 0) s->setTaskDuration(qMax(60, s->taskDuration() - 60));
        else if (m_settingsRow == 1) s->setBreakDuration(qMax(60, s->breakDuration() - 60));
        else if (m_settingsRow == 2) s->setReminderTime(qMax(0, s->reminderTime() - 10));
        return;
    }
    if (seq == "l" || seq == "+" || seq == "=" || seq == "\033[C") {
        if (m_settingsRow == 0) s->setTaskDuration(qMin(86400, s->taskDuration() + 60));
        else if (m_settingsRow == 1) s->setBreakDuration(qMin(86400, s->breakDuration() + 60));
        else if (m_settingsRow == 2) s->setReminderTime(qMin(3600, s->reminderTime() + 10));
        return;
    }
    if (seq == " " || seq == "\r" || seq == "\n" || seq == "e" || seq == "i") {
        if (m_settingsRow == 0) {
            m_inputBuffer = QString::number(s->taskDuration() / 60);
            m_inputCursor = static_cast<int>(m_inputBuffer.size());
            m_mode = Mode::EditSettingValue;
        } else if (m_settingsRow == 1) {
            m_inputBuffer = QString::number(s->breakDuration() / 60);
            m_inputCursor = static_cast<int>(m_inputBuffer.size());
            m_mode = Mode::EditSettingValue;
        } else if (m_settingsRow == 2) {
            m_inputBuffer = QString::number(s->reminderTime());
            m_inputCursor = static_cast<int>(m_inputBuffer.size());
            m_mode = Mode::EditSettingValue;
        } else if (m_settingsRow == 3) {
            s->setMuteSounds(!s->muteSounds());
        } else if (m_settingsRow == 4) {
            s->setUseDarkTheme(!s->useDarkTheme());
        } else if (m_settingsRow == 5) {
            s->setNewTasksOnTop(!s->newTasksOnTop());
        } else if (m_settingsRow == 6) {
            s->setResetTimerOnTaskSwitch(!s->resetTimerOnTaskSwitch());
        } else if (m_settingsRow == 7) {
            m_inputBuffer = m_app->databasePath();
            m_inputCursor = static_cast<int>(m_inputBuffer.size());
            m_mode = Mode::EditSettingValue;
        } else if (m_settingsRow == 8) {
            m_inputBuffer = m_app->databasePath() + QStringLiteral(".backup.db");
            m_inputCursor = static_cast<int>(m_inputBuffer.size());
            m_mode = Mode::EditSettingValue;
        }
    }
}


void TuiApp::openMarkdownFile(const QString& filePath) {
    const QString content = m_app->readMarkdownFile(filePath);
    if (content.isEmpty()) {
        m_statusMessage = tr("Could not open Markdown file: %1").arg(filePath);
        return;
    }
    m_markdownTitle = QStringLiteral("📄 %1").arg(QFileInfo(filePath).fileName());
    m_markdownContent = content;
    m_markdownScroll = 0;
    m_mode = Mode::MarkdownView;
}


void TuiApp::editTaskDetailsInVim() {
    TaskFilterProxyModel* proxy = currentProxy();
    if (proxy->rowCount() <= 0 || m_selectedRow >= proxy->rowCount()) {
        return;
    }
    const QModelIndex idx = proxy->index(m_selectedRow, 0);
    const QString currentDetails = proxy->data(idx, TaskStoreModel::DetailsRole).toString();

    const QString tmpPath = QStringLiteral("/tmp/gofi-task-details-%1.md").arg(QCoreApplication::applicationPid());
    {
        QFile out(tmpPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            m_statusMessage = tr("Failed to create temporary Markdown file in /tmp.");
            return;
        }
        out.write(currentDetails.toUtf8());
        out.close();
    }

    QString editor = qEnvironmentVariable("VISUAL").trimmed();
    if (editor.isEmpty()) editor = qEnvironmentVariable("EDITOR").trimmed();
    if (editor.isEmpty()) {
        for (const QString& cand : {QStringLiteral("nvim"), QStringLiteral("vim"), QStringLiteral("vi"), QStringLiteral("nano")}) {
            if (!QStandardPaths::findExecutable(cand).isEmpty()) {
                editor = cand;
                break;
            }
        }
    }

    if (editor.isEmpty()) {
        // Fallback to inline prompt if no terminal editor is installed
        m_inputBuffer = currentDetails.isEmpty() ? QString() : currentDetails;
        m_inputCursor = static_cast<int>(m_inputBuffer.size());
        m_mode = Mode::EditDetails;
        return;
    }

    if (m_stdinNotifier) m_stdinNotifier->setEnabled(false);
    disableRawMode();

    const int rc = QProcess::execute(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), editor + QStringLiteral(" \"") + tmpPath + QStringLiteral("\"")});

    enableRawMode();
    updateTerminalSize();
    if (m_stdinNotifier) m_stdinNotifier->setEnabled(true);

    QFile in(tmpPath);
    if (rc == 0 && in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString updatedMd = QString::fromUtf8(in.readAll());
        in.close();
        m_app->updateTaskDetails(isClosedTab(), m_selectedRow, updatedMd);
        if (m_mode == Mode::MarkdownView) {
            openTaskMarkdownView();
        }
        m_statusMessage = tr("Markdown description saved (%1 bytes).").arg(updatedMd.toUtf8().size());
    } else {
        m_statusMessage = tr("Markdown edit canceled.");
    }
    QFile::remove(tmpPath);
}


void TuiApp::addTaskCommentInVim() {
    TaskFilterProxyModel* proxy = currentProxy();
    if (proxy->rowCount() <= 0 || m_selectedRow >= proxy->rowCount()) {
        return;
    }
    const QString tmpPath = QStringLiteral("/tmp/gofi-task-comment-%1.md").arg(QCoreApplication::applicationPid());
    {
        QFile out(tmpPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            m_statusMessage = tr("Failed to create temporary comment file in /tmp.");
            return;
        }
        out.close();
    }

    QString editor = qEnvironmentVariable("VISUAL").trimmed();
    if (editor.isEmpty()) editor = qEnvironmentVariable("EDITOR").trimmed();
    if (editor.isEmpty()) {
        for (const QString& cand : {QStringLiteral("nvim"), QStringLiteral("vim"), QStringLiteral("vi"), QStringLiteral("nano")}) {
            if (!QStandardPaths::findExecutable(cand).isEmpty()) {
                editor = cand;
                break;
            }
        }
    }
    if (editor.isEmpty()) {
        m_statusMessage = tr("Use :comment <text> to post a comment.");
        QFile::remove(tmpPath);
        return;
    }

    if (m_stdinNotifier) m_stdinNotifier->setEnabled(false);
    disableRawMode();

    const int rc = QProcess::execute(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), editor + QStringLiteral(" \"") + tmpPath + QStringLiteral("\"")});

    enableRawMode();
    updateTerminalSize();
    if (m_stdinNotifier) m_stdinNotifier->setEnabled(true);

    QFile in(tmpPath);
    if (rc == 0 && in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString commentMd = QString::fromUtf8(in.readAll()).trimmed();
        in.close();
        if (!commentMd.isEmpty()) {
            m_app->addTaskComment(isClosedTab(), m_selectedRow, commentMd);
            if (m_mode == Mode::MarkdownView) {
                openTaskMarkdownView();
            }
            m_statusMessage = tr("Comment added & queued for Jira/YouTrack sync.");
        } else {
            m_statusMessage = tr("Empty comment ignored.");
        }
    }
    QFile::remove(tmpPath);
}

void TuiApp::openTaskMarkdownView() {
    TaskFilterProxyModel* proxy = currentProxy();
    if (proxy->rowCount() <= 0 || m_selectedRow >= proxy->rowCount()) {
        return;
    }
    const QModelIndex idx = proxy->index(m_selectedRow, 0);
    const QString desc = proxy->data(idx, TaskStoreModel::DescriptionRole).toString();
    const QString details = proxy->data(idx, TaskStoreModel::DetailsRole).toString();
    const QVariantList comments = proxy->data(idx, TaskStoreModel::CommentsRole).toList();
    const QString status = proxy->data(idx, TaskStoreModel::StatusRole).toString();

    m_markdownTitle = QStringLiteral("%1 [%2]").arg(desc, statusLabel(status));
    QString fullMd = QStringLiteral("# %1\n\n").arg(desc);
    if (!details.trimmed().isEmpty()) {
        fullMd += details.trimmed() + QStringLiteral("\n\n");
    } else {
        fullMd += QStringLiteral("*No description provided (press `E` to edit description).*\n\n");
    }
    if (!comments.isEmpty()) {
        fullMd += QStringLiteral("---\n## 💬 Comments (%1)\n\n").arg(comments.size());
        for (const QVariant& cv : comments) {
            const QVariantMap cm = cv.toMap();
            fullMd += QStringLiteral("### %1 (%2)\n%3\n\n").arg(
                cm.value(QStringLiteral("author")).toString(),
                cm.value(QStringLiteral("created")).toString(),
                cm.value(QStringLiteral("body")).toString().trimmed()
            );
        }
    }
    m_markdownContent = fullMd;
    m_markdownScroll = 0;
    m_mode = Mode::MarkdownView;
}

QString TuiApp::formatInlineMarkdown(const QString& text) {
    QString s = text;
    // Links: [label](url)
    static const QRegularExpression linkRe(QStringLiteral("\\[([^\\]]+)\\]\\(([^\\)]+)\\)"));
    s.replace(linkRe, QStringLiteral("\033[4m\033[38;5;81m\\1\033[0m\033[38;5;244m (\\2)\033[38;5;255m"));
    // Inline code: `code`
    static const QRegularExpression codeRe(QStringLiteral("`([^`]+)`"));
    s.replace(codeRe, QStringLiteral("\033[48;5;236m\033[38;5;209m \\1 \033[0m\033[38;5;255m"));
    // Bold: **text**
    static const QRegularExpression boldRe(QStringLiteral("\\*\\*([^\\*]+)\\*\\*"));
    s.replace(boldRe, QStringLiteral("\033[1m\033[38;5;255m\\1\033[22m"));
    // Italic: *text*
    static const QRegularExpression italicRe(QStringLiteral("(?<!\\*)\\*([^\\*]+)\\*(?!\\*)"));
    s.replace(italicRe, QStringLiteral("\033[3m\033[38;5;114m\\1\033[23m\033[38;5;255m"));
    return s;
}

QStringList TuiApp::renderMarkdownToAnsi(const QString& markdown, int maxWidth) {
    QStringList out;
    const int wrapCols = qMax(20, maxWidth - 4);
    const QStringList rawLines = markdown.split(QLatin1Char('\n'));
    bool inCodeBlock = false;
    QString codeLang;

    for (const QString& rawLine : rawLines) {
        const QString trimmed = rawLine.trimmed();

        // Fenced code blocks ```
        if (trimmed.startsWith(QStringLiteral("```"))) {
            if (!inCodeBlock) {
                inCodeBlock = true;
                codeLang = trimmed.mid(3).trimmed();
                const QString label = codeLang.isEmpty() ? QStringLiteral(" code ") : QStringLiteral(" %1 ").arg(codeLang);
                const int dashLen = qMax(2, wrapCols - static_cast<int>(label.size()) - 4);
                out.append(FG_GRAY + QStringLiteral("  ╭─") + FG_YELLOW + label + FG_GRAY + QString(dashLen, QChar(0x2500)) + QStringLiteral("╮") + ESC_RESET);
            } else {
                inCodeBlock = false;
                out.append(FG_GRAY + QStringLiteral("  ╰") + QString(qMax(2, wrapCols - 2), QChar(0x2500)) + QStringLiteral("╯") + ESC_RESET);
            }
            continue;
        }

        if (inCodeBlock) {
            out.append(FG_GRAY + QStringLiteral("  │ ") + FG_ORANGE + truncateVisible(rawLine, wrapCols - 4) + ESC_RESET);
            continue;
        }

        if (trimmed.isEmpty()) {
            out.append(QString());
            continue;
        }

        // Horizontal rule
        if (trimmed == QStringLiteral("---") || trimmed == QStringLiteral("***") || trimmed == QStringLiteral("___")) {
            out.append(FG_GRAY + QStringLiteral("  ") + QString(wrapCols, QChar(0x2500)) + ESC_RESET);
            continue;
        }

        // Headings (#, ##, ###)
        if (trimmed.startsWith(QLatin1Char('#'))) {
            int level = 0;
            while (level < trimmed.size() && trimmed.at(level) == QLatin1Char('#')) {
                ++level;
            }
            const QString hText = trimmed.mid(level).trimmed();
            if (level == 1) {
                out.append(ESC_BOLD + FG_CYAN + QStringLiteral("  █ ") + hText + ESC_RESET);
                out.append(FG_CYAN + QStringLiteral("  ") + QString(qMin(wrapCols, static_cast<int>(hText.size()) + 4), QChar(0x2550)) + ESC_RESET);
            } else if (level == 2) {
                out.append(ESC_BOLD + FG_YELLOW + QStringLiteral("  ◆ ") + hText + ESC_RESET);
            } else {
                out.append(ESC_BOLD + FG_PURPLE + QStringLiteral("  ▸ ") + hText + ESC_RESET);
            }
            continue;
        }

        // Blockquotes (> )
        if (trimmed.startsWith(QStringLiteral("> "))) {
            const QString qBody = trimmed.mid(2);
            out.append(FG_CYAN + QStringLiteral("  ┃ ") + FG_GRAY + formatInlineMarkdown(truncateVisible(qBody, wrapCols - 4)) + ESC_RESET);
            continue;
        }

        // Task list checkboxes (- [x] / - [ ])
        if (trimmed.startsWith(QStringLiteral("- [x] "), Qt::CaseInsensitive) || trimmed.startsWith(QStringLiteral("* [x] "), Qt::CaseInsensitive)) {
            out.append(FG_GREEN + QStringLiteral("  ☑ ") + FG_GRAY + ESC_STRIKE + truncateVisible(trimmed.mid(6), wrapCols - 4) + ESC_RESET);
            continue;
        }
        if (trimmed.startsWith(QStringLiteral("- [ ] ")) || trimmed.startsWith(QStringLiteral("* [ ] "))) {
            out.append(FG_YELLOW + QStringLiteral("  ☐ ") + FG_WHITE + formatInlineMarkdown(truncateVisible(trimmed.mid(6), wrapCols - 4)) + ESC_RESET);
            continue;
        }

        // Bullet lists (- / * / •)
        if (trimmed.startsWith(QStringLiteral("- ")) || trimmed.startsWith(QStringLiteral("* ")) || trimmed.startsWith(QStringLiteral("• "))) {
            const QString itemBody = trimmed.mid(2);
            out.append(FG_CYAN + QStringLiteral("  • ") + FG_WHITE + formatInlineMarkdown(truncateVisible(itemBody, wrapCols - 4)) + ESC_RESET);
            continue;
        }

        // Markdown tables (| ... |)
        if (trimmed.startsWith(QLatin1Char('|')) && trimmed.endsWith(QLatin1Char('|'))) {
            QString tableLine = trimmed;
            tableLine.replace(QLatin1Char('|'), FG_CYAN + QStringLiteral(" │ ") + FG_WHITE);
            out.append(QStringLiteral("  ") + tableLine + ESC_RESET);
            continue;
        }

        // Word-wrap normal paragraph lines
        QString rem = rawLine;
        while (rem.size() > wrapCols) {
            int splitPos = rem.lastIndexOf(QLatin1Char(' '), wrapCols);
            if (splitPos <= 0) splitPos = wrapCols;
            out.append(QStringLiteral("  ") + FG_WHITE + formatInlineMarkdown(rem.left(splitPos).trimmed()) + ESC_RESET);
            rem = rem.mid(splitPos).trimmed();
        }
        if (!rem.isEmpty()) {
            out.append(QStringLiteral("  ") + FG_WHITE + formatInlineMarkdown(rem) + ESC_RESET);
        }
    }
    return out;
}

void TuiApp::handleMarkdownViewKey(const QByteArray& seq) {
    if (seq == "\033" || seq == "q" || seq == "v" || seq == "M") {
        m_mode = Mode::Normal;
        return;
    }
    if (seq == "j" || seq == "\033[B" || seq == "\r" || seq == "\n") {
        ++m_markdownScroll;
        return;
    }
    if (seq == "k" || seq == "\033[A") {
        m_markdownScroll = qMax(0, m_markdownScroll - 1);
        return;
    }
    if (seq == "d" || seq == "\x04" || seq == " " || seq == "\033[6~") {
        m_markdownScroll += 10;
        return;
    }
    if (seq == "u" || seq == "\x15" || seq == "\033[5~") {
        m_markdownScroll = qMax(0, m_markdownScroll - 10);
        return;
    }
    if (seq == "g") {
        m_markdownScroll = 0;
        return;
    }
    if (seq == "G") {
        m_markdownScroll = 100000;
        return;
    }
    if (seq == "E" || seq == "e") {
        if (currentProxy()->rowCount() > 0) {
            editTaskDetailsInVim();
        }
        return;
    }
    if (seq == "C" || seq == "c") {
        if (currentProxy()->rowCount() > 0) {
            addTaskCommentInVim();
        }
        return;
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
    if (cmdLine == QStringLiteral("set") || cmdLine == QStringLiteral("settings") || cmdLine == QStringLiteral("config")) {
        m_settingsRow = 0;
        m_mode = Mode::Settings;
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
    if (cmdLine == QStringLiteral("md") || cmdLine == QStringLiteral("view")) {
        openTaskMarkdownView();
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("md ")) || cmdLine.startsWith(QStringLiteral("view "))) {
        const QString path = cmdLine.section(QLatin1Char(' '), 1).trimmed();
        openMarkdownFile(path);
        return;
    }
    if (cmdLine == QStringLiteral("comment")) {
        addTaskCommentInVim();
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("comment "))) {
        const QString cText = cmdLine.mid(8).trimmed();
        if (!cText.isEmpty() && currentProxy()->rowCount() > 0) {
            m_app->addTaskComment(isClosedTab(), m_selectedRow, cText);
            if (m_mode == Mode::MarkdownView) openTaskMarkdownView();
            m_statusMessage = tr("Comment added & queued for Jira/YouTrack sync.");
        }
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
        : QStringLiteral("[L:Lists] [O:Settings] [v:MD] [r:Sync] [?:Help]");
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
            QStringLiteral("  E (Shift+e) / C (Shift+c)         Edit task description (E) or Add comment (C) in Vim & sync to Jira/YouTrack"),
            QStringLiteral("  dd                                Delete selected task"),
            QStringLiteral("  Enter                             Set selected task as active Pomodoro task"),
            QStringLiteral("  t / S / B / R                     Start/Pause timer (t), Skip phase (B), Reset phase (R)"),
            QStringLiteral("  r / Ctrl+r                        Sync current list with Jira / YouTrack"),
            QStringLiteral("  L (a:Add, e:Edit, d:Delete)       Open List Manager (create/edit SQLite, Jira, YouTrack lists)"),
            QStringLiteral("  O or :set / :settings             Open Global Settings (Timer, Notifications, Storage/DB Backup)"),
            QStringLiteral("  v / M or :md <file.md>            Open Full-Screen Markdown Viewer for task or any .md file"),
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
    } else if (m_mode == Mode::MarkdownView) {
        out += BG_HEADER + FG_CYAN + ESC_BOLD
            + padRight(QStringLiteral(" 📖 MARKDOWN VIEWER: %1  (j/k/d/u/g/G scroll, E edit desc, C add comment, q/Esc close)").arg(m_markdownTitle), m_cols)
            + ESC_RESET + QStringLiteral("\033[K\r\n");
        const QStringList mdLines = renderMarkdownToAnsi(m_markdownContent, m_cols);
        const int availRows = bodyRows - 1;
        const int maxScroll = qMax(0, static_cast<int>(mdLines.size()) - availRows);
        m_markdownScroll = qBound(0, m_markdownScroll, maxScroll);
        for (int r = 0; r < availRows; ++r) {
            const int idx = m_markdownScroll + r;
            if (idx < mdLines.size()) {
                out += mdLines.at(idx) + QStringLiteral("\033[K\r\n");
            } else {
                out += FG_GRAY + QStringLiteral(" ~") + ESC_RESET + QStringLiteral("\033[K\r\n");
            }
        }
    } else if (m_mode == Mode::Settings || m_mode == Mode::EditSettingValue) {
        out += FG_CYAN + ESC_BOLD + QStringLiteral("  ⚙ SETTINGS (j/k move, Enter/Space/h/l change, q/Esc close):") + ESC_RESET + QStringLiteral("\033[K\r\n");
        SettingsManager* s = m_app->settings();
        const QStringList settingItems = {
            QStringLiteral("Pomodoro Work Duration (min)    : %1 min").arg(s->taskDuration() / 60),
            QStringLiteral("Pomodoro Break Duration (min)   : %1 min").arg(s->breakDuration() / 60),
            QStringLiteral("Reminder Before End (sec)       : %1 sec").arg(s->reminderTime()),
            QStringLiteral("Mute Sound Notifications        : %1").arg(s->muteSounds() ? QStringLiteral("[ON]") : QStringLiteral("[OFF]")),
            QStringLiteral("GUI Dark Theme                  : %1").arg(s->useDarkTheme() ? QStringLiteral("[ON]") : QStringLiteral("[OFF]")),
            QStringLiteral("Add New Tasks On Top            : %1").arg(s->newTasksOnTop() ? QStringLiteral("[ON]") : QStringLiteral("[OFF]")),
            QStringLiteral("Reset Timer On Task Switch      : %1").arg(s->resetTimerOnTaskSwitch() ? QStringLiteral("[ON]") : QStringLiteral("[OFF]")),
            QStringLiteral("Move SQLite Database Path       : %1").arg(m_app->databasePath()),
            QStringLiteral("Export SQLite Database Backup   : [Press Enter to export .db backup]")
        };
        for (int r = 1; r < bodyRows; ++r) {
            const int idx = r - 1;
            if (idx < settingItems.size()) {
                const bool isSel = (idx == m_settingsRow);
                const QString prefix = isSel ? QStringLiteral("  > ") : QStringLiteral("    ");
                out += (isSel ? (BG_SELECTED + FG_WHITE + ESC_BOLD) : FG_WHITE)
                    + padRight(prefix + settingItems.at(idx), m_cols) + ESC_RESET + QStringLiteral("\033[K\r\n");
            } else {
                out += QStringLiteral("\033[K\r\n");
            }
        }
    } else if (m_mode == Mode::ListEdit || m_mode == Mode::EditListField) {
        out += FG_CYAN + ESC_BOLD + QStringLiteral("  ✎ EDIT LIST CONFIGURATION (j/k move, Enter/Space edit, q/Esc cancel):") + ESC_RESET + QStringLiteral("\033[K\r\n");
        const QString prov = m_editingListCfg.value(QStringLiteral("provider")).toString();
        const QString tokenRaw = m_editingListCfg.value(QStringLiteral("apiToken")).toString();
        const QString tokenMasked = tokenRaw.isEmpty() ? QStringLiteral("(empty)") : (tokenRaw.left(4) + QStringLiteral("••••••••"));
        const QStringList editItems = {
            QStringLiteral("1. List Name                    : %1").arg(m_editingListCfg.value(QStringLiteral("name")).toString()),
            QStringLiteral("2. Provider (Enter to cycle)    : [%1] (SQLite / Jira / YouTrack)").arg(prov),
            QStringLiteral("3. Server URL                   : %1").arg(m_editingListCfg.value(QStringLiteral("jiraUrl")).toString()),
            QStringLiteral("4. Jira Email / Username        : %1").arg(m_editingListCfg.value(QStringLiteral("jiraUser")).toString()),
            QStringLiteral("5. API Token / Permanent Token  : %1").arg(tokenMasked),
            QStringLiteral("6. JQL / Search Query           : %1").arg(m_editingListCfg.value(QStringLiteral("jiraJql")).toString()),
            QStringLiteral("7. Auto-log Pomodoro Worklog    : %1").arg(m_editingListCfg.value(QStringLiteral("autoWorklog"), true).toBool() ? QStringLiteral("[ON]") : QStringLiteral("[OFF]")),
            QStringLiteral("8. >>> [ SAVE & ACTIVATE LIST ] <<<")
        };
        for (int r = 1; r < bodyRows; ++r) {
            const int idx = r - 1;
            if (idx < editItems.size()) {
                const bool isSel = (idx == m_listEditRow);
                const QString prefix = isSel ? QStringLiteral("  > ") : QStringLiteral("    ");
                out += (isSel ? (BG_SELECTED + (idx == 7 ? FG_GREEN : FG_WHITE) + ESC_BOLD) : (idx == 7 ? FG_GREEN : FG_WHITE))
                    + padRight(prefix + editItems.at(idx), m_cols) + ESC_RESET + QStringLiteral("\033[K\r\n");
            } else {
                out += QStringLiteral("\033[K\r\n");
            }
        }
    } else if (m_mode == Mode::ListPicker) {
        out += FG_CYAN + ESC_BOLD + QStringLiteral("  📋 LISTS (j/k move, Enter select, a:Add new, e:Edit Jira/YouTrack, d:Delete, Esc close):") + ESC_RESET + QStringLiteral("\033[K\r\n");
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
                    previewLines.append(renderMarkdownToAnsi(details, m_cols));
                } else {
                    previewLines.append(FG_GRAY + QStringLiteral("  (No detailed description — press 'E' to add, 'v' for full MD viewer)") + ESC_RESET);
                }

                if (!comments.isEmpty()) {
                    previewLines.append(FG_YELLOW + ESC_BOLD + QStringLiteral("  ── Comments (%1) ──").arg(comments.size()) + ESC_RESET);
                    for (const QVariant& cv : comments) {
                        const QVariantMap cm = cv.toMap();
                        previewLines.append(FG_PURPLE + QStringLiteral("  • %1 (%2):").arg(cm.value(QStringLiteral("author")).toString(), cm.value(QStringLiteral("created")).toString()) + ESC_RESET);
                        previewLines.append(renderMarkdownToAnsi(cm.value(QStringLiteral("body")).toString(), m_cols - 2));
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
    } else if (m_mode == Mode::MarkdownView) {
        modeTag = BG_MODE_CMD + QStringLiteral(" MARKDOWN ") + ESC_RESET;
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
    } else if (m_mode == Mode::EditSettingValue || m_mode == Mode::EditListField) {
        out += FG_YELLOW + ESC_BOLD + QStringLiteral("Enter value: ") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
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
