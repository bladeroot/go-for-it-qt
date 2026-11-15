#include "TuiApp.h"
#include "tui/TuiExternalEditor.h"
#include "tui/TuiUtils.h"
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

using namespace tui;

namespace {
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
    , m_dashboardView(controller)
    , m_commandHandler(this)
    , m_settingsView(controller)
    , m_listManagerView(controller)
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

    m_escTimer.setSingleShot(true);
    connect(&m_escTimer, &QTimer::timeout, this, [this]() {
        if (!m_stdinPending.isEmpty() && m_stdinPending.at(0) == '\033') {
            handleKeySequence(QByteArray("\033"));
            m_stdinPending.remove(0, 1);
            processPendingStdin();
            render();
        }
    });
}

TuiApp::~TuiApp() {
    if (m_app && m_app->timer() && m_app->timer()->isRunning()) {
        m_app->timer()->stop();
    }
    disableRawMode();
}

void TuiApp::quitApp() {
    if (m_app && m_app->timer() && m_app->timer()->isRunning()) {
        m_app->timer()->stop();
    }
    QCoreApplication::quit();
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

    m_escTimer.stop();
    m_stdinPending.append(buf, static_cast<int>(n));
    processPendingStdin();
    render();
}

void TuiApp::processPendingStdin() {
    int pos = 0;
    while (pos < m_stdinPending.size()) {
        const unsigned char b = static_cast<unsigned char>(m_stdinPending.at(pos));

        if (b == '\033') {
            if (pos + 1 >= m_stdinPending.size()) {
                // Lone trailing '\033' at the end of buffer
                if (!m_escTimer.isActive()) {
                    m_escTimer.start(50);
                }
                break;
            }

            const char next = m_stdinPending.at(pos + 1);
            if (next == '[') {
                // CSI sequence: '\033[' [0x20-0x3F]* [0x40-0x7E]
                int idx = pos + 2;
                bool foundFinal = false;
                while (idx < m_stdinPending.size()) {
                    const unsigned char c = static_cast<unsigned char>(m_stdinPending.at(idx));
                    if (c >= 0x40 && c <= 0x7E) {
                        foundFinal = true;
                        ++idx;
                        break;
                    } else if (c >= 0x20 && c <= 0x3F) {
                        ++idx;
                        if (idx - pos > 32) {
                            foundFinal = true;
                            break;
                        }
                    } else {
                        foundFinal = true;
                        break;
                    }
                }

                if (!foundFinal) {
                    // Incomplete trailing CSI sequence
                    break;
                }

                const QByteArray seq = m_stdinPending.mid(pos, idx - pos);
                pos = idx;
                m_escTimer.stop();
                handleKeySequence(seq);
                continue;
            } else if (next == 'O') {
                // SS3 sequence: '\033O' [0x20-0x7E]
                if (pos + 2 >= m_stdinPending.size()) {
                    break;
                }
                const QByteArray seq = m_stdinPending.mid(pos, 3);
                pos += 3;
                m_escTimer.stop();
                handleKeySequence(seq);
                continue;
            } else {
                // Lone '\033' followed by non-sequence character
                m_escTimer.stop();
                handleKeySequence(QByteArray("\033"));
                pos += 1;
                continue;
            }
        }

        // UTF-8 code point
        int expectedLen = 1;
        if ((b & 0x80) == 0) {
            expectedLen = 1;
        } else if ((b & 0xE0) == 0xC0) {
            expectedLen = 2;
        } else if ((b & 0xF0) == 0xE0) {
            expectedLen = 3;
        } else if ((b & 0xF8) == 0xF0) {
            expectedLen = 4;
        }

        bool validContinuation = true;
        for (int k = 1; k < expectedLen && (pos + k < m_stdinPending.size()); ++k) {
            const unsigned char cb = static_cast<unsigned char>(m_stdinPending.at(pos + k));
            if ((cb & 0xC0) != 0x80) {
                validContinuation = false;
                break;
            }
        }

        if (!validContinuation) {
            expectedLen = 1;
        } else if (pos + expectedLen > m_stdinPending.size()) {
            break;
        }

        const QByteArray token = m_stdinPending.mid(pos, expectedLen);
        pos += expectedLen;

        handleKeySequence(token);
    }

    if (pos > 0) {
        m_stdinPending.remove(0, pos);
    }
}

void TuiApp::handleKeySequence(const QByteArray& seq) {
    // Ctrl+C quits cleanly from any mode
    if (seq == QByteArray("\x03")) {
        quitApp();
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
                m_inputLine.setText(currentProxy()->data(idx, TaskStoreModel::SimpleTextRole).toString());
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
        m_inputLine.clear();
        m_mode = Mode::InsertTask;
        return;
    }

    // Edit selected task title: i or e
    if (seq == "i" || seq == "e") {
        if (count > 0) {
            const QModelIndex idx = currentProxy()->index(m_selectedRow, 0);
            m_inputLine.setText(currentProxy()->data(idx, TaskStoreModel::SimpleTextRole).toString());
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
        m_settingsView.reset();
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
        m_inputLine.setText(m_app->todoProxyModel()->filterQuery());
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
        m_inputLine.clear();
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
        quitApp();
    }
}

void TuiApp::handleInputLineKey(const QByteArray& seq) {
    const InputKeyResult res = m_inputLine.handleKey(seq);
    if (res == InputKeyResult::Cancel) {
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
    if (res == InputKeyResult::Commit) {
        const QString text = m_inputLine.trimmedText();
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
                QString restored = m_inputLine.text();
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
            const QString msg = m_settingsView.applyEditValue(text);
            if (!msg.isEmpty()) {
                m_statusMessage = msg;
            }
        } else if (prevMode == Mode::EditListField) {
            m_mode = Mode::ListEdit;
            m_listManagerView.applyFieldEdit(text);
        }
        return;
    }
    if (res == InputKeyResult::TextChanged && m_mode == Mode::Search) {
        m_app->todoProxyModel()->setFilterQuery(m_inputLine.text());
        m_app->doneProxyModel()->setFilterQuery(m_inputLine.text());
        clampSelection();
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
    m_listManagerView.openPicker();
    m_mode = Mode::ListPicker;
}

void TuiApp::handleListPickerKey(const QByteArray& seq) {
    QString msg;
    const ListPickerKeyResult res = m_listManagerView.handlePickerKey(seq, msg);
    if (!msg.isEmpty()) {
        m_statusMessage = msg;
    }
    if (res == ListPickerKeyResult::Close || res == ListPickerKeyResult::Applied) {
        m_mode = Mode::Normal;
    } else if (res == ListPickerKeyResult::EditList) {
        m_mode = Mode::ListEdit;
    }
}

void TuiApp::handleListEditKey(const QByteArray& seq) {
    QString editInitial;
    QString msg;
    const ListEditKeyResult res = m_listManagerView.handleEditKey(seq, editInitial, msg);
    if (!msg.isEmpty()) {
        m_statusMessage = msg;
    }
    if (res == ListEditKeyResult::BackToPicker) {
        m_mode = Mode::ListPicker;
    } else if (res == ListEditKeyResult::SavedAndActivated) {
        m_mode = Mode::Normal;
    } else if (res == ListEditKeyResult::EditField) {
        m_inputLine.setText(editInitial);
        m_mode = Mode::EditListField;
    }
}

void TuiApp::handleSettingsKey(const QByteArray& seq) {
    QString editInitial;
    const SettingsKeyResult res = m_settingsView.handleKey(seq, editInitial);
    if (res == SettingsKeyResult::Close) {
        m_mode = Mode::Normal;
    } else if (res == SettingsKeyResult::EditValue) {
        m_inputLine.setText(editInitial);
        m_mode = Mode::EditSettingValue;
    }
}


void TuiApp::openMarkdownFile(const QString& filePath) {
    const QString content = m_app->readMarkdownFile(filePath);
    if (content.isEmpty()) {
        m_statusMessage = tr("Could not open Markdown file: %1").arg(filePath);
        return;
    }
    m_markdownView.setTitle(QStringLiteral("📄 %1").arg(QFileInfo(filePath).fileName()));
    m_markdownView.setContent(content);
    m_mode = Mode::MarkdownView;
}


void TuiApp::editTaskDetailsInVim() {
    TaskFilterProxyModel* proxy = currentProxy();
    if (proxy->rowCount() <= 0 || m_selectedRow >= proxy->rowCount()) {
        return;
    }
    const QModelIndex idx = proxy->index(m_selectedRow, 0);
    const QString currentDetails = proxy->data(idx, TaskStoreModel::DetailsRole).toString();

    bool noEditor = false;
    auto result = TuiExternalEditor::editContent(
        currentDetails,
        QStringLiteral("gofi-task-details-XXXXXX.md"),
        [this](bool paused) {
            if (paused) {
                if (m_stdinNotifier) m_stdinNotifier->setEnabled(false);
                disableRawMode();
            } else {
                enableRawMode();
                updateTerminalSize();
                if (m_stdinNotifier) m_stdinNotifier->setEnabled(true);
            }
        },
        &noEditor
    );

    if (noEditor) {
        m_inputLine.setText(currentDetails);
        m_mode = Mode::EditDetails;
        return;
    }

    if (result.has_value()) {
        const QString updatedMd = *result;
        m_app->updateTaskDetails(isClosedTab(), m_selectedRow, updatedMd);
        if (m_mode == Mode::MarkdownView) {
            openTaskMarkdownView();
        }
        m_statusMessage = tr("Markdown description saved (%1 bytes).").arg(updatedMd.toUtf8().size());
    } else {
        m_statusMessage = tr("Markdown edit canceled.");
    }
}


void TuiApp::addTaskCommentInVim() {
    TaskFilterProxyModel* proxy = currentProxy();
    if (proxy->rowCount() <= 0 || m_selectedRow >= proxy->rowCount()) {
        return;
    }

    bool noEditor = false;
    auto result = TuiExternalEditor::editContent(
        QString(),
        QStringLiteral("gofi-task-comment-XXXXXX.md"),
        [this](bool paused) {
            if (paused) {
                if (m_stdinNotifier) m_stdinNotifier->setEnabled(false);
                disableRawMode();
            } else {
                enableRawMode();
                updateTerminalSize();
                if (m_stdinNotifier) m_stdinNotifier->setEnabled(true);
            }
        },
        &noEditor
    );

    if (noEditor) {
        m_statusMessage = tr("Use :comment <text> to post a comment.");
        return;
    }

    if (result.has_value()) {
        const QString commentMd = result->trimmed();
        if (!commentMd.isEmpty()) {
            if (m_app->addTaskComment(isClosedTab(), m_selectedRow, commentMd)) {
                if (m_mode == Mode::MarkdownView) {
                    openTaskMarkdownView();
                }
                m_statusMessage = tr("Comment added & queued for Jira/YouTrack sync.");
            } else {
                m_statusMessage = tr("Failed to save comment to database.");
            }
        } else {
            m_statusMessage = tr("Empty comment ignored.");
        }
    }
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

    m_markdownView.setTitle(QStringLiteral("%1%2 [%3]").arg(desc, prioLabel, statusLabel(status)));
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
    m_markdownView.setContent(fullMd);
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
    return TuiMarkdownView::formatInlineMarkdown(text);
}

QStringList TuiApp::renderMarkdownToAnsi(const QString& markdown, int maxWidth) {
    return TuiMarkdownView::renderMarkdownToAnsi(markdown, maxWidth);
}

void TuiApp::handleMarkdownViewKey(const QByteArray& seq) {
    switch (m_markdownView.handleKey(seq)) {
    case TuiMarkdownView::KeyResult::Close:
        m_mode = Mode::Normal;
        break;
    case TuiMarkdownView::KeyResult::EditDescription:
        if (currentProxy()->rowCount() > 0) {
            editTaskDetailsInVim();
        }
        break;
    case TuiMarkdownView::KeyResult::AddComment:
        if (currentProxy()->rowCount() > 0) {
            addTaskCommentInVim();
        }
        break;
    case TuiMarkdownView::KeyResult::OpenBrowser:
        if (currentProxy()->rowCount() > 0) {
            openSelectedTaskInBrowser();
        }
        break;
    case TuiMarkdownView::KeyResult::Consumed:
    case TuiMarkdownView::KeyResult::Ignored:
        break;
    }
}

void TuiApp::handleHelpKey(const QByteArray& seq) {
    if (seq == "\033" || seq == "q" || seq == "?" || seq == "\r" || seq == "\n") {
        m_mode = Mode::Normal;
    }
}

void TuiApp::openDashboardView() {
    m_dashboardView.resetScroll();
    m_mode = Mode::Dashboard;
}

void TuiApp::handleDashboardKey(const QByteArray& seq) {
    switch (m_dashboardView.handleKey(seq)) {
    case TuiDashboardView::KeyResult::Close:
        m_mode = Mode::Normal;
        break;
    case TuiDashboardView::KeyResult::ExportCsv: {
        const QString periodName = m_dashboardView.periodName();
        const QString outPath = TuiDashboardView::defaultBillingExportPath(QStringLiteral("billing-%1.csv").arg(periodName));
        if (m_app->analytics()->exportBillingCsv(outPath, periodName, m_app->activeListFilter())) {
            m_statusMessage = tr("Billing CSV exported to %1").arg(outPath);
        }
        break;
    }
    case TuiDashboardView::KeyResult::ExportPdf: {
        const QString periodName = m_dashboardView.periodName();
        const QString outPath = TuiDashboardView::defaultBillingExportPath(QStringLiteral("billing-%1.pdf").arg(periodName));
        if (m_app->analytics()->exportBillingPdf(outPath, periodName, m_app->activeListFilter())) {
            m_statusMessage = tr("Billing PDF exported to %1").arg(outPath);
        }
        break;
    }
    case TuiDashboardView::KeyResult::Consumed:
    case TuiDashboardView::KeyResult::Ignored:
        break;
    }
}

void TuiApp::executeExCommand(const QString& cmdLine) {
    m_commandHandler.execute(cmdLine);
}

QString TuiApp::truncateVisible(const QString& str, int maxCols) {
    return tui::truncateVisible(str, maxCols);
}

QString TuiApp::padRight(const QString& str, int width) {
    return tui::padRight(str, width);
}

QString TuiApp::formatSeconds(uint secs) {
    return tui::formatSeconds(secs);
}

QString TuiApp::sanitizeTerminalText(const QString& input, bool allowNewlines) {
    return tui::sanitizeTerminalText(input, allowNewlines);
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
    const QString syncInfo = m_app->issueTracker()->isSyncing()
        ? tr("⟳ Syncing...")
        : tr("[L:Lists] [O:Settings] [v:MD] [r:Sync] [?:Help]");
    const int syncInfoWidth = displayWidth(syncInfo);
    const int maxBadgeWidth = qMax(10, m_cols - syncInfoWidth - 2);
    const QString rawTitle = sanitizeTerminalText(m_app->currentFilterTitle());
    const QString rawListBadge = QStringLiteral(" 🍅 Go for It Qt [VIM TUI] │ 📋 %1 (%2) ")
        .arg(rawTitle, m_app->currentListProvider());
    const QString listBadge = truncateVisible(rawListBadge, maxBadgeWidth);
    const int headerGap = qMax(1, m_cols - displayWidth(listBadge) - syncInfoWidth - 1);
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
    QString tabRow = QStringLiteral(" ");
    for (int i = 0; i < tabNames.size(); ++i) {
        if (i == m_viewTab) {
            tabRow += BG_ACTIVE_TAB + tabNames.at(i) + ESC_RESET + QStringLiteral(" ");
        } else {
            tabRow += FG_GRAY + tabNames.at(i) + ESC_RESET + QStringLiteral(" ");
        }
    }
    const QString filterStr = currentProxy()->filterQuery();
    if (!filterStr.isEmpty()) {
        tabRow += FG_YELLOW + QStringLiteral(" 🔍 /%1").arg(filterStr) + ESC_RESET;
    }
    out += truncateVisible(tabRow, m_cols) + QStringLiteral("\033[K\r\n");

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
        out += m_markdownView.render(bodyRows, m_cols);
    } else if (m_mode == Mode::Settings || m_mode == Mode::EditSettingValue) {
        out += m_settingsView.render(bodyRows, m_cols);
    } else if (m_mode == Mode::ListEdit || m_mode == Mode::EditListField) {
        out += m_listManagerView.renderEdit(bodyRows, m_cols);
    } else if (m_mode == Mode::ListPicker) {
        out += m_listManagerView.renderPicker(bodyRows, m_cols);
    } else if (m_mode == Mode::Dashboard) {
        out += m_dashboardView.render(bodyRows, m_cols);
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
                    out += truncateVisible(previewLines.at(pIdx), m_cols) + QStringLiteral("\033[K\r\n");
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
        + padRight(QStringLiteral(" %1 │ %2").arg(sanitizeTerminalText(m_app->currentFilterTitle()), posTag), m_cols - 9)
        + ESC_RESET + QStringLiteral("\033[K\r\n");

    // -------------------------------------------------------------------------
    // Row m_rows: Command / Input / Status Message Bar
    // -------------------------------------------------------------------------
    if (m_mode == Mode::InsertTask) {
        out += m_inputLine.render(tr("New task: "), FG_GREEN);
    } else if (m_mode == Mode::EditTask) {
        out += m_inputLine.render(tr("Edit task: "), FG_YELLOW);
    } else if (m_mode == Mode::EditDetails) {
        out += m_inputLine.render(tr("Edit description: "), FG_YELLOW);
    } else if (m_mode == Mode::Search) {
        out += m_inputLine.render(QStringLiteral("/"), FG_CYAN);
    } else if (m_mode == Mode::Command) {
        out += m_inputLine.render(QStringLiteral(":"), FG_ORANGE);
    } else if (m_mode == Mode::EditSettingValue || m_mode == Mode::EditListField) {
        const bool isPassword = (m_mode == Mode::EditListField && m_listManagerView.isEditingPasswordField());
        out += m_inputLine.render(tr("Enter value: "), FG_YELLOW, isPassword);
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
