#include "TuiApp.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
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
    if (status == QStringLiteral("in_progress")) return TuiApp::tr("In Progress");
    if (status == QStringLiteral("paused")) return TuiApp::tr("Paused");
    if (status == QStringLiteral("done")) return TuiApp::tr("Done");
    if (status == QStringLiteral("canceled")) return TuiApp::tr("Canceled / Rejected");
    return TuiApp::tr("To-Do");
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
    connect(m_app, &AppController::languageChanged, this, [this]() {
        m_statusMessage = tr("Vim keys: j/k move, Space toggle done, s status, o add, i edit, t timer, r sync, L lists, / search, : cmd, ? help");
        render();
    });
    connect(m_app->timer(), &TaskTimer::timerUpdated, this, &TuiApp::render);
    connect(m_app->timer(), &TaskTimer::runningChanged, this, &TuiApp::render);
    connect(m_app->timer(), &TaskTimer::breakActiveChanged, this, &TuiApp::render);
    connect(m_app->issueTracker(), &IssueTrackerService::syncingChanged, this, &TuiApp::render);
    connect(m_app->issueTracker(), &IssueTrackerService::statusMessageChanged, this, [this]() {
        m_statusMessage = sanitizeTerminalText(m_app->issueTracker()->lastStatusMessage());
        render();
    });
    connect(m_app, &AppController::databaseError, this, [this](const QString& err) {
        m_statusMessage = sanitizeTerminalText(tr("Database Error: %1").arg(err));
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
    case Mode::Dashboard:
        handleDashboardKey(seq);
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
        if (prev == 'g' && seq == "x") {
            if (count > 0) {
                openSelectedTaskInBrowser();
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

    // Open task / issue in web browser: w
    if (seq == "w") {
        if (count > 0) {
            openSelectedTaskInBrowser();
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
            m_app->markTaskViewed(false, m_selectedRow);
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
        m_statusMessage = tr("Syncing list '%1'...").arg(m_app->currentFilterTitle());
        m_app->syncCurrentList();
        return;
    }

    // Open Settings: O (Shift+o)
    if (seq == "O") {
        m_settingsRow = 0;
        m_mode = Mode::Settings;
        return;
    }

    // Cycle priority: P (Shift+P: None -> A -> B -> C -> D -> E -> None)
    if (seq == "P") {
        if (count > 0 && m_selectedRow < count) {
            const QModelIndex idx = currentProxy()->index(m_selectedRow, 0);
            const quint8 curPrio = static_cast<quint8>(currentProxy()->data(idx, TaskStoreModel::PriorityRole).toUInt());
            quint8 nextPrio = 0;
            if (curPrio == 0) nextPrio = 1;      // B
            else if (curPrio == 1) nextPrio = 2; // C
            else if (curPrio == 2) nextPrio = 3; // D
            else if (curPrio == 3) nextPrio = 4; // E
            else if (curPrio == 4) nextPrio = TxtTask::NoPriority;
            else nextPrio = 0; // A

            const QString prioStr = (nextPrio < 26) ? QString(QChar('A' + nextPrio)) : QStringLiteral("none");
            m_app->changeTaskPriority(isClosedTab(), m_selectedRow, prioStr);
            m_statusMessage = tr("Task priority set to: %1").arg(prioStr.toUpper());
        }
        return;
    }

    // Switch lists: L (picker) or [ / ]
    if (seq == "L") {
        openListPicker();
        return;
    }
    if (seq == "[") {
        const int nLists = m_app->listManager()->rowCount();
        if (nLists > 0) {
            const int cur = qMax(0, m_app->listManager()->indexOfId(m_app->currentListId()));
            const int prev = (cur + nLists - 1) % nLists;
            m_app->selectListById(m_app->listManager()->entry(prev).id);
        }
        return;
    }
    if (seq == "]") {
        const int nLists = m_app->listManager()->rowCount();
        if (nLists > 0) {
            const int cur = qMax(0, m_app->listManager()->indexOfId(m_app->currentListId()));
            const int next = (cur + 1) % nLists;
            m_app->selectListById(m_app->listManager()->entry(next).id);
        }
        return;
    }

    // Open Full-Screen Markdown Viewer for task description & comments: v or M
    if (seq == "v" || seq == "M") {
        openTaskMarkdownView();
        return;
    }

    // Open Productivity Dashboard & Reports: U
    if (seq == "U") {
        openDashboardView();
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
            } else if (m_settingsRow == 10) {
                m_app->settings()->setAutoSyncIntervalMinutes(qBound(0, text.toInt(), 1440));
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

void TuiApp::openListPicker() {
    m_listPickerRow = qMax(0, m_app->listManager()->indexOfId(m_app->currentListId()));
    m_selectedListIds.clear();
    if (m_app->isMultiListView()) {
        const QStringList active = m_app->activeListIds();
        for (const QString& id : active) {
            m_selectedListIds.insert(id);
        }
    } else if (!m_app->currentListId().isEmpty()) {
        m_selectedListIds.insert(m_app->currentListId());
    }
    m_mode = Mode::ListPicker;
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
    // Toggle multi-selection checkbox with Space
    if (seq == " ") {
        if (m_listPickerRow >= 0 && m_listPickerRow < nLists) {
            const QString id = m_app->listManager()->entry(m_listPickerRow).id;
            if (m_selectedListIds.contains(id)) {
                m_selectedListIds.remove(id);
            } else {
                m_selectedListIds.insert(id);
            }
        }
        return;
    }
    // Quick filter: All Lists
    if (seq == "*" || seq == "A") {
        m_mode = Mode::Normal;
        m_app->showAllLists();
        m_statusMessage = tr("Switched to All Lists (%1 lists).").arg(nLists);
        return;
    }
    // Quick filter: All Local SQLite lists
    if (seq == "l") {
        m_mode = Mode::Normal;
        m_app->showProviderLists(QStringLiteral("SQLite"));
        m_statusMessage = tr("Switched to Local lists view.");
        return;
    }
    // Quick filter: All Remote Trackers
    if (seq == "t" || seq == "T") {
        QStringList trackerIds;
        for (int i = 0; i < nLists; ++i) {
            const auto entry = m_app->listManager()->entry(i);
            if (entry.provider != QStringLiteral("SQLite")) {
                trackerIds.append(entry.id);
            }
        }
        if (!trackerIds.isEmpty()) {
            m_mode = Mode::Normal;
            m_app->setMultiListFilter(trackerIds, tr("All Trackers (%1)").arg(trackerIds.size()));
            m_statusMessage = tr("Switched to All Trackers view (%1 lists).").arg(trackerIds.size());
            return;
        }
    }
    // Add list: a, o, n, +
    if (seq == "a" || seq == "o" || seq == "n" || seq == "+") {
        m_editingListCfg = m_app->listManager()->getListConfig(QString());
        m_editingListCfg[QStringLiteral("name")] = QStringLiteral("New List");
        m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Jira");
        m_listEditRow = 0;
        m_mode = Mode::ListEdit;
        return;
    }
    // Edit list: e, i
    if ((seq == "e" || seq == "i") && m_listPickerRow >= 0 && m_listPickerRow < nLists) {
        const TodoListEntry entry = m_app->listManager()->entry(m_listPickerRow);
        m_editingListCfg = m_app->listManager()->getListConfig(entry.id);
        m_listEditRow = 0;
        m_mode = Mode::ListEdit;
        return;
    }
    // Delete list: d
    if (seq == "d" && nLists > 1 && m_listPickerRow >= 0 && m_listPickerRow < nLists) {
        const TodoListEntry entry = m_app->listManager()->entry(m_listPickerRow);
        m_selectedListIds.remove(entry.id);
        m_app->listManager()->removeListById(entry.id);
        m_listPickerRow = qBound(0, m_listPickerRow, m_app->listManager()->rowCount() - 1);
        m_statusMessage = tr("Removed list '%1'.").arg(entry.name);
        return;
    }
    // Apply selection: Enter
    if (seq == "\r" || seq == "\n") {
        m_mode = Mode::Normal;
        if (m_selectedListIds.size() > 1) {
            QStringList ids = m_selectedListIds.values();
            m_app->setMultiListFilter(ids, tr("Selected Lists (%1)").arg(ids.size()));
            m_statusMessage = tr("Showing %1 selected lists.").arg(ids.size());
        } else if (m_selectedListIds.size() == 1) {
            const QString selId = *m_selectedListIds.begin();
            m_app->selectListById(selId);
            m_statusMessage = tr("Switched to list '%1'.").arg(m_app->currentListName());
        } else if (m_listPickerRow >= 0 && m_listPickerRow < nLists) {
            const TodoListEntry entry = m_app->listManager()->entry(m_listPickerRow);
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
        m_listEditRow = qMin(8, m_listEditRow + 1);
        return;
    }
    if (seq == "k" || seq == "\033[A") {
        m_listEditRow = qMax(0, m_listEditRow - 1);
        return;
    }
    if (seq == " " || seq == "\r" || seq == "\n" || seq == "e" || seq == "i" || seq == "h" || seq == "l") {
        if (m_listEditRow == 1) {
            // Cycle provider: SQLite -> Jira -> YouTrack -> GitHub -> GitLab -> Redmine -> Todoist -> Nextcloud
            const QString cur = m_editingListCfg.value(QStringLiteral("provider")).toString();
            if (cur == QStringLiteral("SQLite")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Jira");
            else if (cur == QStringLiteral("Jira")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("YouTrack");
            else if (cur == QStringLiteral("YouTrack")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("GitHub");
            else if (cur == QStringLiteral("GitHub")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("GitLab");
            else if (cur == QStringLiteral("GitLab")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Redmine");
            else if (cur == QStringLiteral("Redmine")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Todoist");
            else if (cur == QStringLiteral("Todoist")) m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("Nextcloud");
            else m_editingListCfg[QStringLiteral("provider")] = QStringLiteral("SQLite");
            return;
        }
        if (m_listEditRow == 6) {
            m_editingListCfg[QStringLiteral("autoWorklog")] = !m_editingListCfg.value(QStringLiteral("autoWorklog"), true).toBool();
            return;
        }
        if (m_listEditRow == 7) {
            m_editingListCfg[QStringLiteral("syncStatusToRemote")] = !m_editingListCfg.value(QStringLiteral("syncStatusToRemote"), false).toBool();
            return;
        }
        if (m_listEditRow == 8) {
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
        m_settingsRow = qMin(14, m_settingsRow + 1);
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
        else if (m_settingsRow == 10) s->setAutoSyncIntervalMinutes(qMax(0, s->autoSyncIntervalMinutes() - 1));
        return;
    }
    if (seq == "l" || seq == "+" || seq == "=" || seq == "\033[C") {
        if (m_settingsRow == 0) s->setTaskDuration(qMin(86400, s->taskDuration() + 60));
        else if (m_settingsRow == 1) s->setBreakDuration(qMin(86400, s->breakDuration() + 60));
        else if (m_settingsRow == 2) s->setReminderTime(qMin(3600, s->reminderTime() + 10));
        else if (m_settingsRow == 10) s->setAutoSyncIntervalMinutes(qMin(1440, s->autoSyncIntervalMinutes() + 1));
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
        } else if (m_settingsRow == 9) {
            const QStringList langs = m_app->availableLanguageCodes();
            const int idx = langs.indexOf(s->language());
            if (idx >= 0 && !langs.isEmpty()) {
                s->setLanguage(langs.at((idx + 1) % langs.size()));
            } else {
                s->setLanguage(langs.isEmpty() ? QStringLiteral("system") : langs.first());
            }
        } else if (m_settingsRow == 10) {
            m_inputBuffer = QString::number(s->autoSyncIntervalMinutes());
            m_inputCursor = static_cast<int>(m_inputBuffer.size());
            m_mode = Mode::EditSettingValue;
        } else if (m_settingsRow == 11) {
            s->setNotifyTaskChangesSound(!s->notifyTaskChangesSound());
        } else if (m_settingsRow == 12) {
            s->setNotifyTaskChangesPopup(!s->notifyTaskChangesPopup());
        } else if (m_settingsRow == 13) {
            s->setStartMinimized(!s->startMinimized());
        } else if (m_settingsRow == 14) {
            s->setStartInTray(!s->startInTray());
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

    QTemporaryFile tempFile(QDir::tempPath() + QStringLiteral("/gofi-task-details-XXXXXX.md"));
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        m_statusMessage = tr("Failed to create temporary Markdown file.");
        return;
    }
    tempFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    tempFile.write(currentDetails.toUtf8());
    tempFile.flush();
    const QString tmpPath = tempFile.fileName();
    tempFile.close();

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
    QTemporaryFile tempFile(QDir::tempPath() + QStringLiteral("/gofi-task-comment-XXXXXX.md"));
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        m_statusMessage = tr("Failed to create temporary comment file.");
        return;
    }
    tempFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QString tmpPath = tempFile.fileName();
    tempFile.close();

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
    m_app->markTaskViewed(isClosedTab(), m_selectedRow);
    const QModelIndex idx = proxy->index(m_selectedRow, 0);
    const QString desc = proxy->data(idx, TaskStoreModel::DescriptionRole).toString();
    const QString details = proxy->data(idx, TaskStoreModel::DetailsRole).toString();
    const QVariantList comments = proxy->data(idx, TaskStoreModel::CommentsRole).toList();
    const QString status = proxy->data(idx, TaskStoreModel::StatusRole).toString();
    const quint8 prio = static_cast<quint8>(proxy->data(idx, TaskStoreModel::PriorityRole).toUInt());
    QString prioLabel;
    if (prio < 26) {
        prioLabel = QStringLiteral(" [(%1)]").arg(QChar('A' + prio));
    }

    m_markdownTitle = QStringLiteral("%1%2 [%3]").arg(desc, prioLabel, statusLabel(status));
    QString fullMd = QStringLiteral("# %1%2\n\n").arg(desc, prioLabel);

    const QString listId = proxy->data(idx, TaskStoreModel::ListIdRole).toString();
    const QString jiraKey = proxy->data(idx, TaskStoreModel::JiraKeyRole).toString();
    const QUrl taskUrl = m_app->getTaskBrowserUrl(listId, jiraKey, desc);
    if (taskUrl.isValid() && !taskUrl.isEmpty()) {
        fullMd += QStringLiteral("🌐 **Web URL:** [%1](%1) *(press `w` to open in browser)*\n\n").arg(taskUrl.toString());
    }

    if (!details.trimmed().isEmpty()) {
        fullMd += details.trimmed() + QStringLiteral("\n\n");
    } else {
        fullMd += tr("*No description provided (press `E` to edit description).*") + QStringLiteral("\n\n");
    }
    if (!comments.isEmpty()) {
        fullMd += QStringLiteral("---\n## 💬 ") + tr("Comments (%1)").arg(comments.size()) + QStringLiteral("\n\n");
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

void TuiApp::openSelectedTaskInBrowser() {
    TaskFilterProxyModel* proxy = currentProxy();
    const int count = proxy->rowCount();
    if (count <= 0 || m_selectedRow < 0 || m_selectedRow >= count) return;

    const QModelIndex idx = proxy->index(m_selectedRow, 0);
    const QString listId = proxy->data(idx, TaskStoreModel::ListIdRole).toString();
    const QString jiraKey = proxy->data(idx, TaskStoreModel::JiraKeyRole).toString();
    const QString desc = proxy->data(idx, TaskStoreModel::DescriptionRole).toString();

    const QUrl url = m_app->getTaskBrowserUrl(listId, jiraKey, desc);
    if (url.isValid() && !url.isEmpty()) {
        m_statusMessage = tr("Opening in browser: %1").arg(url.toString());
        m_app->openUrlExternally(url);
    } else {
        m_statusMessage = tr("No web URL available for selected task.");
    }
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
    const QString safeMarkdown = sanitizeTerminalText(markdown, true);
    const QStringList rawLines = safeMarkdown.split(QLatin1Char('\n'));
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
    if (seq == "w" || seq == "gx") {
        if (currentProxy()->rowCount() > 0) {
            openSelectedTaskInBrowser();
        }
        return;
    }
}

void TuiApp::handleHelpKey(const QByteArray& seq) {
    if (seq == "\033" || seq == "q" || seq == "?" || seq == "\r" || seq == "\n") {
        m_mode = Mode::Normal;
    }
}

void TuiApp::openDashboardView() {
    m_dashboardScroll = 0;
    m_mode = Mode::Dashboard;
}

void TuiApp::handleDashboardKey(const QByteArray& seq) {
    if (seq == "\033" || seq == "q") {
        m_mode = Mode::Normal;
        return;
    }
    if (seq == "1") { m_dashboardPeriod = 0; m_dashboardScroll = 0; return; }
    if (seq == "2") { m_dashboardPeriod = 1; m_dashboardScroll = 0; return; }
    if (seq == "3") { m_dashboardPeriod = 2; m_dashboardScroll = 0; return; }
    if (seq == "4") { m_dashboardPeriod = 3; m_dashboardScroll = 0; return; }

    if (seq == "j" || seq == "\033[B") { ++m_dashboardScroll; return; }
    if (seq == "k" || seq == "\033[A") { m_dashboardScroll = qMax(0, m_dashboardScroll - 1); return; }
    if (seq == "d" || seq == "\x04" || seq == " ") { m_dashboardScroll += 8; return; }
    if (seq == "u" || seq == "\x15") { m_dashboardScroll = qMax(0, m_dashboardScroll - 8); return; }
    if (seq == "g") { m_dashboardScroll = 0; return; }
    if (seq == "G") { m_dashboardScroll = 100000; return; }

    const QString periodName = (m_dashboardPeriod == 0) ? QStringLiteral("day")
                             : (m_dashboardPeriod == 1) ? QStringLiteral("week")
                             : (m_dashboardPeriod == 2) ? QStringLiteral("month") : QStringLiteral("all");

    if (seq == "c") {
        const QString outPath = QStringLiteral("/tmp/billing-%1.csv").arg(periodName);
        if (m_app->analytics()->exportBillingCsv(outPath, periodName, m_app->currentListId())) {
            m_statusMessage = tr("Billing CSV exported to %1").arg(outPath);
        }
        return;
    }
    if (seq == "p") {
        const QString outPath = QStringLiteral("/tmp/billing-%1.pdf").arg(periodName);
        if (m_app->analytics()->exportBillingPdf(outPath, periodName, m_app->currentListId())) {
            m_statusMessage = tr("Billing PDF exported to %1").arg(outPath);
        }
        return;
    }
}

void TuiApp::executeExCommand(const QString& cmdLine) {
    if (cmdLine.isEmpty()) return;
    if (cmdLine == QStringLiteral("q") || cmdLine == QStringLiteral("quit") || cmdLine == QStringLiteral("qa")) {
        QCoreApplication::quit();
        return;
    }
    if (cmdLine == QStringLiteral("w") || cmdLine == QStringLiteral("sync") || cmdLine == QStringLiteral("e")) {
        m_statusMessage = tr("Syncing '%1'...").arg(m_app->currentFilterTitle());
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
        openListPicker();
        return;
    }
    if (cmdLine == QStringLiteral("help") || cmdLine == QStringLiteral("h")) {
        m_mode = Mode::Help;
        return;
    }
    if (cmdLine == QStringLiteral("stats") || cmdLine == QStringLiteral("dashboard") || cmdLine == QStringLiteral("report")) {
        openDashboardView();
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("export-csv"))) {
        const QString rest = cmdLine.mid(10).trimmed();
        const QString outPath = !rest.isEmpty() ? rest : QStringLiteral("/tmp/billing-report.csv");
        if (m_app->analytics()->exportBillingCsv(outPath, QStringLiteral("month"), m_app->currentListId())) {
            m_statusMessage = tr("Billing CSV exported to %1").arg(outPath);
        } else {
            m_statusMessage = tr("Failed to export CSV report.");
        }
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("export-pdf"))) {
        const QString rest = cmdLine.mid(10).trimmed();
        const QString outPath = !rest.isEmpty() ? rest : QStringLiteral("/tmp/billing-report.pdf");
        if (m_app->analytics()->exportBillingPdf(outPath, QStringLiteral("month"), m_app->currentListId())) {
            m_statusMessage = tr("Billing PDF exported to %1").arg(outPath);
        } else {
            m_statusMessage = tr("Failed to export PDF report.");
        }
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
    if (cmdLine.startsWith(QStringLiteral("ambient"))) {
        const QString mode = cmdLine.mid(7).trimmed().toLower();
        if (mode.isEmpty() || mode == QStringLiteral("toggle")) {
            const QString cur = m_app->ambient()->ambientMode();
            const QString next = (cur == QStringLiteral("none")) ? QStringLiteral("rain") : QStringLiteral("none");
            m_app->ambient()->setAmbientMode(next);
            m_statusMessage = tr("Ambient focus sound: %1").arg(next);
        } else if (mode == QStringLiteral("none") || mode == QStringLiteral("off")) {
            m_app->ambient()->setAmbientMode(QStringLiteral("none"));
            m_statusMessage = tr("Ambient focus sound turned off.");
        } else if (mode.startsWith(QStringLiteral("white"))) {
            m_app->ambient()->setAmbientMode(QStringLiteral("white_noise"));
            m_statusMessage = tr("Ambient focus sound: White Noise.");
        } else if (mode.startsWith(QStringLiteral("rain"))) {
            m_app->ambient()->setAmbientMode(QStringLiteral("rain"));
            m_statusMessage = tr("Ambient focus sound: Gentle Rain.");
        } else if (mode.startsWith(QStringLiteral("clock")) || mode.startsWith(QStringLiteral("tick"))) {
            m_app->ambient()->setAmbientMode(QStringLiteral("clock"));
            m_statusMessage = tr("Ambient focus sound: Mechanical Clock Ticking.");
        } else {
            m_statusMessage = tr("Unknown ambient mode. Available: none, white, rain, clock");
        }
        return;
    }
    if (cmdLine == QStringLiteral("today")) {
        m_app->todoProxyModel()->setDueFilter(QStringLiteral("today"));
        clampSelection();
        m_statusMessage = tr("Filtered tasks due today.");
        return;
    }
    if (cmdLine == QStringLiteral("overdue")) {
        m_app->todoProxyModel()->setDueFilter(QStringLiteral("overdue"));
        clampSelection();
        m_statusMessage = tr("Filtered overdue tasks.");
        return;
    }
    if (cmdLine == QStringLiteral("week")) {
        m_app->todoProxyModel()->setDueFilter(QStringLiteral("week"));
        clampSelection();
        m_statusMessage = tr("Filtered tasks due this week.");
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("dnd"))) {
        const QString arg = cmdLine.mid(3).trimmed().toLower();
        if (arg == QStringLiteral("on") || arg == QStringLiteral("1") || arg == QStringLiteral("enable")) {
            m_app->settings()->setDndFocusEnabled(true);
            m_statusMessage = tr("Do Not Disturb (DND) focus mode enabled for Work cycles.");
        } else if (arg == QStringLiteral("off") || arg == QStringLiteral("0") || arg == QStringLiteral("disable")) {
            m_app->settings()->setDndFocusEnabled(false);
            m_statusMessage = tr("Do Not Disturb (DND) focus mode disabled.");
        } else {
            const bool cur = m_app->settings()->dndFocusEnabled();
            m_app->settings()->setDndFocusEnabled(!cur);
            m_statusMessage = tr("Do Not Disturb (DND) focus mode: %1").arg(!cur ? tr("ON") : tr("OFF"));
        }
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("hooks"))) {
        const QString arg = cmdLine.mid(5).trimmed().toLower();
        if (arg == QStringLiteral("on") || arg == QStringLiteral("1") || arg == QStringLiteral("enable")) {
            m_app->settings()->setCustomHooksEnabled(true);
            m_statusMessage = tr("System event hooks enabled.");
        } else if (arg == QStringLiteral("off") || arg == QStringLiteral("0") || arg == QStringLiteral("disable")) {
            m_app->settings()->setCustomHooksEnabled(false);
            m_statusMessage = tr("System event hooks disabled.");
        } else {
            const bool cur = m_app->settings()->customHooksEnabled();
            m_app->settings()->setCustomHooksEnabled(!cur);
            m_statusMessage = tr("System event hooks: %1").arg(!cur ? tr("ON") : tr("OFF"));
        }
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("hook"))) {
        QString event = cmdLine.mid(4).trimmed();
        if (event.startsWith(QStringLiteral("test "))) {
            event = event.mid(5).trimmed();
        } else if (event == QStringLiteral("test")) {
            event = QStringLiteral("work_start");
        }
        if (event.isEmpty()) event = QStringLiteral("work_start");
        m_app->systemHooks()->testHook(event);
        m_statusMessage = tr("Tested hook event '%1' (dir: %2)").arg(event, m_app->systemHooks()->hooksDirectory());
        return;
    }
    if (cmdLine == QStringLiteral("all")) {
        m_app->todoProxyModel()->setDueFilter(QString());
        clampSelection();
        m_statusMessage = tr("Showing all tasks.");
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("p ")) || cmdLine.startsWith(QStringLiteral("prio ")) || cmdLine.startsWith(QStringLiteral("priority "))) {
        const QString prioArg = cmdLine.section(QLatin1Char(' '), 1).trimmed();
        if (currentProxy()->rowCount() > 0 && m_selectedRow >= 0) {
            m_app->changeTaskPriority(isClosedTab(), m_selectedRow, prioArg);
            m_statusMessage = tr("Priority set to %1").arg(prioArg.toUpper());
        }
        return;
    }
    m_statusMessage = tr("Unknown command: :%1 (try :help, :sync, :sort, :clear, :list, :q)").arg(cmdLine);
}

namespace {
int codepointDisplayWidth(uint32_t cp) {
    if (cp == 0 || cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
    // Combining marks and variation selectors
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0xFE00 && cp <= 0xFE0F)) return 0;
    // Wide emoji / symbols / CJK blocks
    if (cp >= 0x1F000 ||
        (cp >= 0x231A && cp <= 0x23FA) ||
        (cp >= 0x2614 && cp <= 0x2615) ||
        (cp >= 0x2648 && cp <= 0x2653) ||
        cp == 0x267F || cp == 0x2693 || cp == 0x26A1 ||
        (cp >= 0x26AA && cp <= 0x26AB) ||
        (cp >= 0x26BD && cp <= 0x26BE) ||
        (cp >= 0x26C4 && cp <= 0x26C5) ||
        cp == 0x26CE || cp == 0x26D4 || cp == 0x26EA ||
        (cp >= 0x26F2 && cp <= 0x26F5) ||
        cp == 0x26FA || cp == 0x26FD || cp == 0x2705 ||
        (cp >= 0x270A && cp <= 0x270B) ||
        cp == 0x2728 || cp == 0x274C || cp == 0x274E ||
        (cp >= 0x2753 && cp <= 0x2755) || cp == 0x2757 ||
        (cp >= 0x2795 && cp <= 0x2797) || cp == 0x27B0 || cp == 0x27BF ||
        (cp >= 0x2B1B && cp <= 0x2B1C) || cp == 0x2B50 || cp == 0x2B55 ||
        (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6)) {
        return 2;
    }
    return 1;
}

int displayWidth(const QString& str) {
    int w = 0;
    const QList<uint> ucs4 = str.toUcs4();
    for (uint cp : ucs4) {
        w += codepointDisplayWidth(cp);
    }
    return w;
}
} // namespace

QString TuiApp::truncateVisible(const QString& str, int maxCols) {
    if (maxCols <= 0) return {};
    if (displayWidth(str) <= maxCols) return str;
    if (maxCols <= 1) return QStringLiteral("…");
    QString res;
    int curCols = 0;
    for (int i = 0; i < str.size(); ++i) {
        uint cp = str.at(i).unicode();
        int step = 1;
        if (str.at(i).isHighSurrogate() && i + 1 < str.size() && str.at(i + 1).isLowSurrogate()) {
            cp = QChar::surrogateToUcs4(str.at(i), str.at(i + 1));
            step = 2;
        }
        const int cw = codepointDisplayWidth(cp);
        if (curCols + cw > maxCols - 1) {
            break;
        }
        res += str.mid(i, step);
        curCols += cw;
        if (step == 2) ++i;
    }
    return res + QStringLiteral("…");
}

QString TuiApp::padRight(const QString& str, int width) {
    const int dw = displayWidth(str);
    if (dw >= width) return truncateVisible(str, width);
    return str + QString(width - dw, QLatin1Char(' '));
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

QString TuiApp::sanitizeTerminalText(const QString& input, bool allowNewlines) {
    if (input.isEmpty()) return {};

    // Strip ANSI escape sequences:
    // - OSC sequences: ESC ] ... (BEL | ESC \)
    // - CSI sequences: ESC [ ... [@-~]
    // - DCS/APC/PM: ESC [P^_X] ... ESC \ or two-byte ESC .
    static const QRegularExpression oscRe(QStringLiteral("\x1b\\][^\x07\x1b]*(\x07|\x1b\\\\)"));
    static const QRegularExpression csiRe(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
    static const QRegularExpression escGeneralRe(QStringLiteral("\x1b[P^_X][^\x1b]*\x1b\\\\|\x1b."));

    QString clean = input;
    clean.remove(oscRe);
    clean.remove(csiRe);
    clean.remove(escGeneralRe);
    clean.remove(QLatin1Char('\x1b'));

    QString result;
    result.reserve(clean.size());
    for (int i = 0; i < clean.size(); ++i) {
        const QChar ch = clean.at(i);
        const ushort u = ch.unicode();

        if (allowNewlines && ch == QLatin1Char('\n')) {
            result.append(QLatin1Char('\n'));
        } else if (ch == QLatin1Char('\t')) {
            result.append(QStringLiteral("    "));
        } else if (u < 0x20 || u == 0x7F || (u >= 0x80 && u <= 0x9F)) {
            if (ch == QLatin1Char('\r')) {
                continue;
            }
            result.append(QLatin1Char(' '));
        } else {
            result.append(ch);
        }
    }
    return result;
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
        .arg(m_app->currentFilterTitle(), m_app->currentListProvider());
    const QString syncInfo = m_app->issueTracker()->isSyncing()
        ? tr("⟳ Syncing...")
        : tr("[L:Lists] [O:Settings] [v:MD] [r:Sync] [?:Help]");
    const int headerGap = qMax(1, m_cols - displayWidth(listBadge) - displayWidth(syncInfo) - 1);
    out += BG_HEADER + FG_CYAN + ESC_BOLD + listBadge + ESC_RESET + BG_HEADER
        + QString(headerGap, QLatin1Char(' ')) + FG_GRAY + syncInfo + QLatin1Char(' ') + ESC_RESET + QStringLiteral("\033[K\r\n");

    // -------------------------------------------------------------------------
    // Row 2: Pomodoro Timer Banner
    // -------------------------------------------------------------------------
    TaskTimer* timer = m_app->timer();
    const QString stateColor = timer->isRunning()
        ? (timer->isBreakActive() ? FG_GREEN : FG_ORANGE)
        : (timer->isBreakActive() ? FG_GREEN : FG_YELLOW);
    const QString stateText = timer->isRunning()
        ? (timer->isBreakActive() ? tr("♦ BREAK RUNNING") : tr("▶ WORK RUNNING"))
        : (timer->isBreakActive() ? tr("♦ BREAK PAUSED")  : tr("‖ TIMER READY"));
    const QString paddedState = padRight(stateText, 20);
    const QString clockStr = padRight(formatSeconds(timer->remainingSeconds()), 7);
    const int barWidth = 16;
    const int filled = qBound(0, static_cast<int>(timer->progress() * barWidth), barWidth);
    const QString progBar = QStringLiteral("[")
        + QString(filled, QChar(0x2588))
        + QString(barWidth - filled, QChar(0x2591))
        + QStringLiteral("]");
    // Total prefix columns: 1 (" ") + 20 (state) + 2 ("  ") + 7 (clock) + 1 (" ") + 18 (progBar) + 4 (" │ ") = 53
    const int availActiveCols = qMax(8, m_cols - 54);
    QString rawActiveDesc = timer->hasActiveTask() ? sanitizeTerminalText(timer->activeTaskDescription()) : tr("No active task");
    rawActiveDesc.replace(QLatin1Char('\r'), QLatin1Char(' '));
    rawActiveDesc.replace(QLatin1Char('\n'), QLatin1Char(' '));
    const QString activeTaskTitle = truncateVisible(rawActiveDesc, availActiveCols);

    out += QStringLiteral(" ") + ESC_BOLD + stateColor + paddedState + ESC_RESET
        + QStringLiteral("  ") + ESC_BOLD + FG_WHITE + clockStr + ESC_RESET
        + QStringLiteral(" ") + FG_CYAN + progBar + ESC_RESET
        + FG_GRAY + QStringLiteral(" │ ") + FG_WHITE + activeTaskTitle + ESC_RESET + QStringLiteral("\033[K\r\n");

    // -------------------------------------------------------------------------
    // Row 3: View / Status Filter Tabs (1..6)
    // -------------------------------------------------------------------------
    const int unviewed = m_app->unviewedTodoCount();
    const int total = m_app->totalTodoCount();
    QString todoTabLabel;
    if (total > 0) {
        if (unviewed > 0) {
            todoTabLabel = tr(" 2:To-Do (%1/%2) ").arg(unviewed).arg(total);
        } else {
            todoTabLabel = tr(" 2:To-Do (%1) ").arg(total);
        }
    } else {
        todoTabLabel = tr(" 2:To-Do ");
    }

    const QStringList tabNames = {
        tr(" 1:All Open "),
        todoTabLabel,
        tr(" 3:In Progress "),
        tr(" 4:Paused "),
        tr(" 5:Done "),
        tr(" 6:Canceled ")
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
            tr("  VIM NAVIGATION & ACTIONS"),
            tr("  j / k / gg / G / Ctrl+d / Ctrl+u  Move cursor down / up / top / bottom / half-page"),
            tr("  J / K (Shift+j / Shift+k)         Move selected task down / up in order"),
            tr("  h / l or 1..6 or gt / gT          Switch status tab (All Open, To-Do, In Progress, Paused, Done, Canceled)"),
            tr("  Space or x                        Toggle task Done / To-Do"),
            tr("  s                                 Open Status Picker (1:To-Do, 2:In Progress, 3:Paused, 4:Done, 5:Canceled)"),
            tr("  o / a                             Add new task (INSERT mode)"),
            tr("  i / e / cw                        Edit selected task title inline"),
            tr("  E (Shift+e) / C (Shift+c)         Edit task description (E) or Add comment (C) in Vim & sync to Jira/YouTrack"),
            tr("  dd                                Delete selected task"),
            tr("  Enter                             Set selected task as active Pomodoro task"),
            tr("  t / S / B / R                     Start/Pause timer (t), Skip phase (B), Reset phase (R)"),
            tr("  r / Ctrl+r                        Sync current list with Jira / YouTrack"),
            tr("  L (a:Add, e:Edit, d:Delete)       Open List Manager (create/edit SQLite, Jira, YouTrack lists)"),
            tr("  O or :set / :settings             Open Global Settings (Timer, Notifications, Storage/DB Backup)"),
            tr("  v / M or :md <file.md>            Open Full-Screen Markdown Viewer for task or any .md file"),
            tr("  w / gx                            Open task / issue in web browser (GitHub, GitLab, Jira, YouTrack...)"),
            tr("  U or :stats / :dashboard          Open Productivity Dashboard (Pomodoro stats, Heatmap, +Project/@Context)"),
            tr("  p / Ctrl+e / Ctrl+y               Toggle preview pane (p), scroll description/comments (Ctrl+e/y)"),
            tr("  / and :                           Search tasks (/) or Ex commands (:sync, :sort, :dnd, :hooks, :hook test, :q)")
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
            + padRight(tr(" 📖 MARKDOWN VIEWER: %1  (j/k/d/u/g/G scroll, E edit desc, C add comment, q/Esc close)").arg(m_markdownTitle), m_cols)
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
        out += FG_CYAN + ESC_BOLD + tr("  ⚙ SETTINGS (j/k move, Enter/Space/h/l change, q/Esc close):") + ESC_RESET + QStringLiteral("\033[K\r\n");
        SettingsManager* s = m_app->settings();
        const QStringList settingItems = {
            tr("Pomodoro Work Duration (min)    : %1 min").arg(s->taskDuration() / 60),
            tr("Pomodoro Break Duration (min)   : %1 min").arg(s->breakDuration() / 60),
            tr("Reminder Before End (sec)       : %1 sec").arg(s->reminderTime()),
            tr("Mute Sound Notifications        : %1").arg(s->muteSounds() ? tr("[ON]") : tr("[OFF]")),
            tr("GUI Dark Theme                  : %1").arg(s->useDarkTheme() ? tr("[ON]") : tr("[OFF]")),
            tr("Add New Tasks On Top            : %1").arg(s->newTasksOnTop() ? tr("[ON]") : tr("[OFF]")),
            tr("Reset Timer On Task Switch      : %1").arg(s->resetTimerOnTaskSwitch() ? tr("[ON]") : tr("[OFF]")),
            tr("Move SQLite Database Path       : %1").arg(m_app->databasePath()),
            tr("Export SQLite Database Backup   : [Press Enter to export .db backup]"),
            tr("Language (Enter to cycle)       : [%1] (%2)").arg(s->language(), m_app->languageDisplayName(s->language())),
            tr("Auto-Sync Jira/YouTrack (min)   : %1 min").arg(s->autoSyncIntervalMinutes()),
            tr("Sound On New/Changed Tasks      : %1").arg(s->notifyTaskChangesSound() ? tr("[ON]") : tr("[OFF]")),
            tr("Popup On New/Changed Tasks      : %1").arg(s->notifyTaskChangesPopup() ? tr("[ON]") : tr("[OFF]")),
            tr("GUI Start Minimized             : %1").arg(s->startMinimized() ? tr("[ON]") : tr("[OFF]")),
            tr("GUI Start In System Tray        : %1").arg(s->startInTray() ? tr("[ON]") : tr("[OFF]"))
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
        out += FG_CYAN + ESC_BOLD + tr("  ✎ EDIT LIST CONFIGURATION (j/k move, Enter/Space edit, q/Esc cancel):") + ESC_RESET + QStringLiteral("\033[K\r\n");
        const QString prov = m_editingListCfg.value(QStringLiteral("provider")).toString();
        const QString tokenRaw = m_editingListCfg.value(QStringLiteral("apiToken")).toString();
        const QString tokenMasked = tokenRaw.isEmpty() ? tr("(empty)") : (tokenRaw.left(4) + QStringLiteral("••••••••"));
        const QStringList editItems = {
            tr("1. List Name                    : %1").arg(m_editingListCfg.value(QStringLiteral("name")).toString()),
            tr("2. Provider (Enter to cycle)    : [%1] (SQLite / Jira / YouTrack / GitHub / GitLab)").arg(prov),
            tr("3. Server URL                   : %1").arg(m_editingListCfg.value(QStringLiteral("jiraUrl")).toString()),
            tr("4. Jira Email / Username        : %1").arg(m_editingListCfg.value(QStringLiteral("jiraUser")).toString()),
            tr("5. API Token / Permanent Token  : %1").arg(tokenMasked),
            tr("6. JQL / Search Query           : %1").arg(m_editingListCfg.value(QStringLiteral("jiraJql")).toString()),
            tr("7. Auto-log Pomodoro Worklog    : %1").arg(m_editingListCfg.value(QStringLiteral("autoWorklog"), true).toBool() ? tr("[ON]") : tr("[OFF]")),
            tr("8. Sync Status Back to Tracker  : %1").arg(m_editingListCfg.value(QStringLiteral("syncStatusToRemote"), false).toBool() ? tr("[ON]") : tr("[OFF]")),
            tr("9. >>> [ SAVE & ACTIVATE LIST ] <<<")
        };
        for (int r = 1; r < bodyRows; ++r) {
            const int idx = r - 1;
            if (idx < editItems.size()) {
                const bool isSel = (idx == m_listEditRow);
                const QString prefix = isSel ? QStringLiteral("  > ") : QStringLiteral("    ");
                out += (isSel ? (BG_SELECTED + (idx == 8 ? FG_GREEN : FG_WHITE) + ESC_BOLD) : (idx == 8 ? FG_GREEN : FG_WHITE))
                    + padRight(prefix + editItems.at(idx), m_cols) + ESC_RESET + QStringLiteral("\033[K\r\n");
            } else {
                out += QStringLiteral("\033[K\r\n");
            }
        }
    } else if (m_mode == Mode::ListPicker) {
        out += FG_CYAN + ESC_BOLD + tr("  📋 LISTS (Space:toggle [x], Enter:apply, *:All, l:Local, t:Trackers, a/o:Add, e:Edit, d:Del, Esc:back):") + ESC_RESET + QStringLiteral("\033[K\r\n");
        const int nLists = m_app->listManager()->rowCount();
        for (int r = 1; r < bodyRows; ++r) {
            const int idx = r - 1;
            if (idx < nLists) {
                const TodoListEntry e = m_app->listManager()->entry(idx);
                const bool isSel = (idx == m_listPickerRow);
                const bool isChecked = m_selectedListIds.contains(e.id);
                const QString cursor = isSel ? QStringLiteral(" > ") : QStringLiteral("   ");
                const QString check = isChecked ? (FG_GREEN + QStringLiteral("[x]") + ESC_RESET) : (FG_GRAY + QStringLiteral("[ ]") + ESC_RESET);

                QString provColor = FG_WHITE;
                if (e.provider == QStringLiteral("GitHub")) provColor = FG_PURPLE;
                else if (e.provider == QStringLiteral("GitLab")) provColor = FG_ORANGE;
                else if (e.provider == QStringLiteral("Jira")) provColor = FG_CYAN;
                else if (e.provider == QStringLiteral("YouTrack")) provColor = FG_YELLOW;
                else if (e.provider == QStringLiteral("Redmine")) provColor = FG_RED;
                else if (e.provider == QStringLiteral("Todoist")) provColor = FG_ORANGE;
                else if (e.provider == QStringLiteral("Nextcloud")) provColor = FG_CYAN;
                else if (e.provider == QStringLiteral("SQLite")) provColor = FG_GREEN;

                const QString provBadge = provColor + QStringLiteral("[%1]").arg(padRight(e.provider, 8)) + ESC_RESET;
                const bool isCur = (e.id == m_app->currentListId() && !m_app->isMultiListView());
                const QString activeTag = isCur ? (FG_CYAN + tr(" (active)") + ESC_RESET) : QString();

                const QString line = QStringLiteral("%1 %2 %3 %4 %5")
                    .arg(cursor, check, provBadge, sanitizeTerminalText(e.name), activeTag);
                out += (isSel ? (BG_SELECTED + FG_WHITE + ESC_BOLD) : FG_WHITE)
                    + padRight(line, m_cols) + ESC_RESET + QStringLiteral("\033[K\r\n");
            } else {
                out += QStringLiteral("\033[K\r\n");
            }
        }
    } else if (m_mode == Mode::Dashboard) {
        const QString periodKey = (m_dashboardPeriod == 0) ? QStringLiteral("day")
                                : (m_dashboardPeriod == 1) ? QStringLiteral("week")
                                : (m_dashboardPeriod == 2) ? QStringLiteral("month") : QStringLiteral("all");
        const QString periodTitle = (m_dashboardPeriod == 0) ? tr("TODAY")
                                  : (m_dashboardPeriod == 1) ? tr("THIS WEEK")
                                  : (m_dashboardPeriod == 2) ? tr("THIS MONTH") : tr("ALL TIME");

        out += BG_HEADER + FG_CYAN + ESC_BOLD
            + padRight(tr(" 📊 PRODUCTIVITY DASHBOARD: %1  ([1]Day [2]Week [3]Month [4]All │ [c]CSV [p]PDF │ q/Esc:Close)").arg(periodTitle), m_cols)
            + ESC_RESET + QStringLiteral("\033[K\r\n");

        const QVariantMap stats = m_app->analytics()->getProductivityStats(periodKey, m_app->currentListId());
        const QVariantList heatmap = m_app->analytics()->getActivityHeatmap(28, m_app->currentListId());

        QStringList lines;
        lines.append(FG_YELLOW + ESC_BOLD + tr("  ── SUMMARY METRICS ──") + ESC_RESET);
        lines.append(QStringLiteral("  ⏱ Total Focus: %1 (%2h)   🍅 Pomodoros: %3   📋 Active Tasks: %4   ⚡ Sessions: %5")
            .arg(stats.value(QStringLiteral("totalFormattedTime")).toString(),
                 QString::number(stats.value(QStringLiteral("totalHours")).toDouble(), 'f', 2),
                 QString::number(stats.value(QStringLiteral("completedPomodoros")).toInt()),
                 QString::number(stats.value(QStringLiteral("activeTaskCount")).toInt()),
                 QString::number(stats.value(QStringLiteral("totalSessions")).toInt())));

        lines.append(QString());
        lines.append(FG_GREEN + ESC_BOLD + tr("  ── 28-DAY ACTIVITY HEATMAP ──") + ESC_RESET);
        QString heatRow = QStringLiteral("  ");
        for (const QVariant& hv : heatmap) {
            const QVariantMap hm = hv.toMap();
            const int lvl = hm.value(QStringLiteral("intensity")).toInt();
            if (lvl == 0) heatRow += FG_GRAY + QStringLiteral("[ ]") + ESC_RESET;
            else if (lvl == 1) heatRow += QStringLiteral("\033[38;5;108m[░]") + ESC_RESET;
            else if (lvl == 2) heatRow += QStringLiteral("\033[38;5;114m[▒]") + ESC_RESET;
            else if (lvl == 3) heatRow += QStringLiteral("\033[38;5;120m[▓]") + ESC_RESET;
            else heatRow += QStringLiteral("\033[38;5;46m[█]") + ESC_RESET;
        }
        lines.append(heatRow);

        lines.append(QString());
        lines.append(FG_ORANGE + ESC_BOLD + tr("  ── PROJECTS (+Project) ──") + ESC_RESET);
        const QVariantList projs = stats.value(QStringLiteral("projectBreakdown")).toList();
        if (projs.isEmpty()) {
            lines.append(FG_GRAY + tr("  (No project tags recorded in this period)") + ESC_RESET);
        } else {
            for (const QVariant& pv : projs) {
                const QVariantMap pm = pv.toMap();
                const double pct = pm.value(QStringLiteral("percentage")).toDouble();
                const int barLen = qBound(0, static_cast<int>(pct / 5.0), 20);
                const QString bar = QString(barLen, QChar(0x2588)) + QString(20 - barLen, QChar(0x2591));
                lines.append(QStringLiteral("  +%-16s [%1] %2%  (%3)")
                    .arg(pm.value(QStringLiteral("name")).toString())
                    .arg(bar, QString::number(pct, 'f', 1), pm.value(QStringLiteral("formattedTime")).toString()));
            }
        }

        lines.append(QString());
        lines.append(FG_PURPLE + ESC_BOLD + tr("  ── CONTEXTS (@Context) ──") + ESC_RESET);
        const QVariantList ctxs = stats.value(QStringLiteral("contextBreakdown")).toList();
        if (ctxs.isEmpty()) {
            lines.append(FG_GRAY + tr("  (No context tags recorded in this period)") + ESC_RESET);
        } else {
            for (const QVariant& cv : ctxs) {
                const QVariantMap cm = cv.toMap();
                const double pct = cm.value(QStringLiteral("percentage")).toDouble();
                const int barLen = qBound(0, static_cast<int>(pct / 5.0), 20);
                const QString bar = QString(barLen, QChar(0x2588)) + QString(20 - barLen, QChar(0x2591));
                lines.append(QStringLiteral("  @%-16s [%1] %2%  (%3)")
                    .arg(cm.value(QStringLiteral("name")).toString())
                    .arg(bar, QString::number(pct, 'f', 1), cm.value(QStringLiteral("formattedTime")).toString()));
            }
        }

        lines.append(QString());
        lines.append(FG_CYAN + ESC_BOLD + tr("  ── TOP TASKS (Time & Billing) ──") + ESC_RESET);
        const QVariantList tasks = stats.value(QStringLiteral("taskBreakdown")).toList();
        if (tasks.isEmpty()) {
            lines.append(FG_GRAY + tr("  (No tasks tracked during this period)") + ESC_RESET);
        } else {
            for (const QVariant& tv : tasks) {
                const QVariantMap tm = tv.toMap();
                const QString key = tm.value(QStringLiteral("jiraKey")).toString();
                const QString badge = !key.isEmpty() ? QStringLiteral("[%1] ").arg(key) : QString();
                lines.append(QStringLiteral("  • %1%2 (%3, %4 sessions)")
                    .arg(badge,
                         sanitizeTerminalText(tm.value(QStringLiteral("description")).toString()),
                         tm.value(QStringLiteral("formattedTime")).toString(),
                         QString::number(tm.value(QStringLiteral("sessions")).toInt())));
            }
        }

        const int availRows = bodyRows - 1;
        const int maxScroll = qMax(0, static_cast<int>(lines.size()) - availRows);
        m_dashboardScroll = qBound(0, m_dashboardScroll, maxScroll);
        for (int r = 0; r < availRows; ++r) {
            const int idx = m_dashboardScroll + r;
            if (idx < lines.size()) {
                out += truncateVisible(lines.at(idx), m_cols) + QStringLiteral("\033[K\r\n");
            } else {
                out += FG_GRAY + QStringLiteral(" ~") + ESC_RESET + QStringLiteral("\033[K\r\n");
            }
        }
    } else {
        for (int r = 0; r < listRows; ++r) {
            const int rowIdx = m_scrollOffset + r;
            if (rowIdx >= totalTasks) {
                if (totalTasks == 0 && r == 0) {
                    out += FG_GRAY + tr("   (No tasks in this view — press 'o' to add a task or 'r' to sync Jira/YouTrack)")
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
            const QString desc = sanitizeTerminalText(proxy->data(idx, TaskStoreModel::DescriptionRole).toString());
            const QString details = proxy->data(idx, TaskStoreModel::DetailsRole).toString();
            const QVariantList comments = proxy->data(idx, TaskStoreModel::CommentsRole).toList();
            const uint durSec = proxy->data(idx, TaskStoreModel::DurationRole).toUInt();
            const uint timerSec = proxy->data(idx, TaskStoreModel::TimerValueRole).toUInt();
            const bool isViewed = proxy->data(idx, TaskStoreModel::IsViewedRole).toBool();
            const QString unreadDot = (!isViewed && (status == QStringLiteral("todo") || status.isEmpty()) && !isClosedTab())
                ? (FG_RED + ESC_BOLD + QStringLiteral("●") + ESC_RESET)
                : QStringLiteral(" ");

            QString statusBadge = QStringLiteral("[ ]");
            QString statusColor = FG_WHITE;
            if (status == QStringLiteral("in_progress")) {
                statusBadge = QStringLiteral("[▶]");
                statusColor = FG_CYAN;
            } else if (status == QStringLiteral("paused")) {
                statusBadge = QStringLiteral("[=]");
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
                if (prio == 0) prioColor = FG_RED + ESC_BOLD;
                else if (prio == 1) prioColor = FG_ORANGE + ESC_BOLD;
                else if (prio == 2) prioColor = FG_YELLOW + ESC_BOLD;
                else if (prio == 3) prioColor = FG_CYAN;
                else if (prio == 4) prioColor = FG_GREEN;
                else prioColor = FG_GRAY;
            }

            QString metaSuffix;
            if (!details.isEmpty()) metaSuffix += QStringLiteral(" [MD]");
            if (!comments.isEmpty()) metaSuffix += QStringLiteral(" [#%1]").arg(comments.size());
            if (durSec > 0 || timerSec > 0) {
                metaSuffix += QStringLiteral(" [%1/%2]").arg(formatSeconds(timerSec), formatSeconds(durSec));
            }

            const QString dueDateStr = proxy->data(idx, TaskStoreModel::DueDateStringRole).toString();
            const bool isOverdue = proxy->data(idx, TaskStoreModel::IsOverdueRole).toBool();
            const bool isDueToday = proxy->data(idx, TaskStoreModel::IsDueTodayRole).toBool();
            const bool isDueThisWeek = proxy->data(idx, TaskStoreModel::IsDueThisWeekRole).toBool();

            QString dueBadgePlain;
            QString dueBadgeStyled;
            if (!dueDateStr.isEmpty()) {
                dueBadgePlain = QStringLiteral(" [due:%1]").arg(dueDateStr);
                if (isOverdue) {
                    dueBadgeStyled = FG_RED + ESC_BOLD + dueBadgePlain + ESC_RESET;
                } else if (isDueToday) {
                    dueBadgeStyled = FG_ORANGE + ESC_BOLD + dueBadgePlain + ESC_RESET;
                } else if (isDueThisWeek) {
                    dueBadgeStyled = FG_CYAN + dueBadgePlain + ESC_RESET;
                } else {
                    dueBadgeStyled = FG_GRAY + dueBadgePlain + ESC_RESET;
                }
            }

            QString singleLineDesc = desc;
            singleLineDesc.replace(QLatin1Char('\r'), QLatin1Char(' '));
            singleLineDesc.replace(QLatin1Char('\n'), QLatin1Char(' '));
            singleLineDesc.replace(QLatin1Char('\t'), QLatin1Char(' '));

            QString provBadge;
            int provCols = 0;
            if (m_app->isMultiListView()) {
                const QString prov = proxy->data(idx, TaskStoreModel::ListProviderRole).toString();
                if (prov == QStringLiteral("GitHub")) {
                    provBadge = FG_PURPLE + QStringLiteral("[GH] ") + ESC_RESET;
                    provCols = 5;
                } else if (prov == QStringLiteral("GitLab")) {
                    provBadge = FG_ORANGE + QStringLiteral("[GL] ") + ESC_RESET;
                    provCols = 5;
                } else if (prov == QStringLiteral("Jira")) {
                    provBadge = FG_CYAN + QStringLiteral("[Jira] ") + ESC_RESET;
                    provCols = 7;
                } else if (prov == QStringLiteral("YouTrack")) {
                    provBadge = FG_YELLOW + QStringLiteral("[YT] ") + ESC_RESET;
                    provCols = 5;
                } else if (prov == QStringLiteral("Redmine")) {
                    provBadge = FG_RED + QStringLiteral("[RM] ") + ESC_RESET;
                    provCols = 5;
                } else if (prov == QStringLiteral("Todoist")) {
                    provBadge = FG_ORANGE + QStringLiteral("[TD] ") + ESC_RESET;
                    provCols = 5;
                } else if (prov == QStringLiteral("Nextcloud")) {
                    provBadge = FG_CYAN + QStringLiteral("[NC] ") + ESC_RESET;
                    provCols = 5;
                } else if (prov == QStringLiteral("SQLite")) {
                    provBadge = FG_GREEN + QStringLiteral("[Local] ") + ESC_RESET;
                    provCols = 8;
                }
            }

            const QString cursorMark = isSelected ? QStringLiteral(" > ") : (isActivePomodoro ? QStringLiteral(" • ") : QStringLiteral("   "));
            const int availDescCols = qMax(10, m_cols - 14 - provCols - displayWidth(metaSuffix) - displayWidth(dueBadgePlain));
            const QString cleanDesc = padRight(singleLineDesc, availDescCols);

            if (isSelected) {
                out += BG_SELECTED + FG_CYAN + ESC_BOLD + cursorMark + unreadDot + QLatin1Char(' ') + statusColor + statusBadge + QLatin1Char(' ')
                    + prioColor + prioStr + ESC_RESET + BG_SELECTED + QLatin1Char(' ')
                    + provBadge
                    + FG_WHITE + cleanDesc
                    + FG_PURPLE + metaSuffix
                    + (!dueBadgePlain.isEmpty() ? (ESC_BOLD + dueBadgePlain) : QString())
                    + ESC_RESET + QStringLiteral("\033[K\r\n");
            } else {
                out += (isActivePomodoro ? FG_CYAN : FG_GRAY) + cursorMark + unreadDot + QLatin1Char(' ') + statusColor + statusBadge + QLatin1Char(' ')
                    + prioColor + prioStr + ESC_RESET + QLatin1Char(' ')
                    + provBadge
                    + (isClosedTab() ? (FG_GRAY + ESC_STRIKE) : FG_WHITE) + cleanDesc + ESC_RESET
                    + FG_GRAY + metaSuffix + ESC_RESET
                    + dueBadgeStyled
                    + QStringLiteral("\033[K\r\n");
            }
        }

        // ---------------------------------------------------------------------
        // Preview Pane (Full Description + Comments of Selected Task)
        // ---------------------------------------------------------------------
        if (m_showPreview) {
            const QString prevHdr = tr("── Preview (p: toggle, E: edit description, Ctrl+e/Ctrl+y: scroll) ");
            out += FG_GRAY + prevHdr
                + QString(qMax(0, m_cols - static_cast<int>(prevHdr.size())), QChar(0x2500)) + ESC_RESET + QStringLiteral("\033[K\r\n");

            QStringList previewLines;
            if (totalTasks > 0 && m_selectedRow < totalTasks) {
                const QModelIndex idx = proxy->index(m_selectedRow, 0);
                const QString desc = sanitizeTerminalText(proxy->data(idx, TaskStoreModel::DescriptionRole).toString());
                const QString details = proxy->data(idx, TaskStoreModel::DetailsRole).toString();
                const QVariantList comments = proxy->data(idx, TaskStoreModel::CommentsRole).toList();
                const QString status = proxy->data(idx, TaskStoreModel::StatusRole).toString();
                const quint8 prio = static_cast<quint8>(proxy->data(idx, TaskStoreModel::PriorityRole).toUInt());
                QString prioBadge;
                if (prio < 26) {
                    QString color = FG_GRAY;
                    if (prio == 0) color = FG_RED + ESC_BOLD;
                    else if (prio == 1) color = FG_ORANGE + ESC_BOLD;
                    else if (prio == 2) color = FG_YELLOW + ESC_BOLD;
                    else if (prio == 3) color = FG_CYAN;
                    else if (prio == 4) color = FG_GREEN;
                    prioBadge = QStringLiteral("  ") + color + QStringLiteral("[(%1)]").arg(QChar('A' + prio)) + ESC_RESET;
                }

                previewLines.append(ESC_BOLD + FG_CYAN + QStringLiteral("  %1").arg(desc) + ESC_RESET
                    + QStringLiteral("  [%1]").arg(statusLabel(status))
                    + prioBadge);

                const QString listId = proxy->data(idx, TaskStoreModel::ListIdRole).toString();
                const QString jiraKey = proxy->data(idx, TaskStoreModel::JiraKeyRole).toString();
                const QUrl taskUrl = m_app->getTaskBrowserUrl(listId, jiraKey, desc);
                if (taskUrl.isValid() && !taskUrl.isEmpty()) {
                    previewLines.append(FG_CYAN + tr("  🌐 Browser: %1 (press 'w' or 'gx' to open)").arg(taskUrl.toString()) + ESC_RESET);
                }

                if (!details.isEmpty()) {
                    previewLines.append(renderMarkdownToAnsi(details, m_cols));
                } else {
                    previewLines.append(FG_GRAY + tr("  (No detailed description — press 'E' to add, 'v' for full MD viewer)") + ESC_RESET);
                }

                if (!comments.isEmpty()) {
                    previewLines.append(FG_YELLOW + ESC_BOLD + tr("  ── Comments (%1) ──").arg(comments.size()) + ESC_RESET);
                    for (const QVariant& cv : comments) {
                        const QVariantMap cm = cv.toMap();
                        const QString author = sanitizeTerminalText(cm.value(QStringLiteral("author")).toString());
                        const QString created = sanitizeTerminalText(cm.value(QStringLiteral("created")).toString());
                        previewLines.append(FG_PURPLE + QStringLiteral("  • %1 (%2):").arg(author, created) + ESC_RESET);
                        previewLines.append(renderMarkdownToAnsi(cm.value(QStringLiteral("body")).toString(), m_cols - 2));
                    }
                }
            } else {
                previewLines.append(FG_GRAY + tr("  (No task selected)") + ESC_RESET);
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
    } else if (m_mode == Mode::Dashboard) {
        modeTag = BG_MODE_CMD + QStringLiteral(" DASHBOARD ") + ESC_RESET;
    }

    const QString posTag = QStringLiteral(" %1/%2 ")
        .arg(totalTasks > 0 ? (m_selectedRow + 1) : 0)
        .arg(totalTasks);
    out += modeTag + BG_HEADER + FG_WHITE
        + padRight(QStringLiteral(" %1 │ %2").arg(m_app->currentFilterTitle(), posTag), m_cols - 9)
        + ESC_RESET + QStringLiteral("\033[K\r\n");

    // -------------------------------------------------------------------------
    // Row m_rows: Command / Input / Status Message Bar
    // -------------------------------------------------------------------------
    if (m_mode == Mode::InsertTask) {
        out += FG_GREEN + ESC_BOLD + tr("New task: ") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
    } else if (m_mode == Mode::EditTask) {
        out += FG_YELLOW + ESC_BOLD + tr("Edit task: ") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
    } else if (m_mode == Mode::EditDetails) {
        out += FG_YELLOW + ESC_BOLD + tr("Edit description: ") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
    } else if (m_mode == Mode::Search) {
        out += FG_CYAN + ESC_BOLD + QStringLiteral("/") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
    } else if (m_mode == Mode::Command) {
        out += FG_ORANGE + ESC_BOLD + QStringLiteral(":") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
    } else if (m_mode == Mode::EditSettingValue || m_mode == Mode::EditListField) {
        out += FG_YELLOW + ESC_BOLD + tr("Enter value: ") + ESC_RESET + m_inputBuffer + QStringLiteral("█\033[K");
    } else if (m_mode == Mode::StatusPicker) {
        out += FG_YELLOW + ESC_BOLD
            + tr("Set status: [1/t] To-Do  [2/i] In Progress  [3/p] Paused  [4/d] Done  [5/c] Canceled  [Esc] Cancel")
            + ESC_RESET + QStringLiteral("\033[K");
    } else {
        out += FG_GRAY + truncateVisible(sanitizeTerminalText(m_statusMessage), m_cols - 1) + ESC_RESET + QStringLiteral("\033[K");
    }

    const QByteArray frameBytes = out.toUtf8();
    (void)::write(STDOUT_FILENO, frameBytes.constData(), static_cast<size_t>(frameBytes.size()));
}

} // namespace gofi
