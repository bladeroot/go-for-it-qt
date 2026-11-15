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

TuiApp::TuiApp(AppController* controller, QObject* parent)
    : QObject(parent)
    , m_app(controller)
    , m_terminal(this)
    , m_taskListView(controller)
    , m_dashboardView(controller)
    , m_commandHandler(this)
    , m_settingsView(controller)
    , m_listManagerView(controller)
{
    connect(m_app, &AppController::currentListChanged, this, [this]() {
        m_taskListView.resetSelection();
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

    connect(&m_terminal, &TuiTerminal::keySequenceReceived, this, &TuiApp::handleKeySequence);
    connect(&m_terminal, &TuiTerminal::redrawRequested, this, &TuiApp::render);
}

TuiApp::~TuiApp() {
    if (m_app && m_app->timer() && m_app->timer()->isRunning()) {
        m_app->timer()->stop();
    }
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
    if (!m_terminal.initialize()) {
        return false;
    }
    switchViewTab(0);

    m_statusMessage = tr("Vim keys: j/k move, Space toggle done, s status, o add, i edit, t timer, r sync, L lists, / search, : cmd, ? help");
    render();
    return true;
}

void TuiApp::onStdinActivated() {
    m_terminal.onStdinActivated();
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
    QString initialText;
    const TaskListKeyResult res = m_taskListView.handleKey(seq, m_statusMessage, initialText);
    switch (res) {
    case TaskListKeyResult::Handled:
        break;
    case TaskListKeyResult::StartInsert:
        m_inputLine.clear();
        m_mode = Mode::InsertTask;
        break;
    case TaskListKeyResult::StartEditTitle:
        m_inputLine.setText(initialText);
        m_mode = Mode::EditTask;
        break;
    case TaskListKeyResult::StartEditDetails:
        editTaskDetailsInVim();
        break;
    case TaskListKeyResult::StartAddComment:
        addTaskCommentInVim();
        break;
    case TaskListKeyResult::OpenStatusPicker:
        m_mode = Mode::StatusPicker;
        break;
    case TaskListKeyResult::OpenListPicker:
        openListPicker();
        break;
    case TaskListKeyResult::OpenSettings:
        m_settingsView.reset();
        m_mode = Mode::Settings;
        break;
    case TaskListKeyResult::OpenTaskMarkdown:
        openTaskMarkdownView();
        break;
    case TaskListKeyResult::StartSearch:
        m_inputLine.setText(m_app->todoProxyModel()->filterQuery());
        m_mode = Mode::Search;
        break;
    case TaskListKeyResult::StartCommand:
        m_inputLine.clear();
        m_mode = Mode::Command;
        break;
    case TaskListKeyResult::OpenHelp:
        m_mode = Mode::Help;
        break;
    case TaskListKeyResult::OpenDashboard:
        openDashboardView();
        break;
    case TaskListKeyResult::Quit:
        quitApp();
        break;
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
                m_app->updateTaskText(isClosedTab(), selectedRow(), text);
                m_statusMessage = tr("Updated task.");
            }
        } else if (prevMode == Mode::EditDetails) {
            if (currentProxy()->rowCount() > 0) {
                QString restored = m_inputLine.text();
                restored.replace(QStringLiteral(" \\n "), QStringLiteral("\n"));
                m_app->updateTaskDetails(isClosedTab(), selectedRow(), restored.trimmed());
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
            m_app->changeTaskStatus(isClosedTab(), selectedRow(), targetStatus);
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
    if (proxy->rowCount() <= 0 || selectedRow() >= proxy->rowCount()) {
        return;
    }
    const QModelIndex idx = proxy->index(selectedRow(), 0);
    const QString currentDetails = proxy->data(idx, TaskStoreModel::DetailsRole).toString();

    bool noEditor = false;
    auto result = TuiExternalEditor::editContent(
        currentDetails,
        QStringLiteral("gofi-task-details-XXXXXX.md"),
        [this](bool paused) {
            m_terminal.setSuspended(paused);
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
        m_app->updateTaskDetails(isClosedTab(), selectedRow(), updatedMd);
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
    if (proxy->rowCount() <= 0 || selectedRow() < 0 || selectedRow() >= proxy->rowCount()) {
        m_statusMessage = tr("No task selected.");
        return;
    }

    bool noEditor = false;
    auto result = TuiExternalEditor::editContent(
        QString(),
        QStringLiteral("gofi-task-comment-XXXXXX.md"),
        [this](bool paused) {
            m_terminal.setSuspended(paused);
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
            if (m_app->addTaskComment(isClosedTab(), selectedRow(), commentMd)) {
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
    if (proxy->rowCount() <= 0 || selectedRow() >= proxy->rowCount()) {
        return;
    }
    m_app->markTaskViewed(isClosedTab(), selectedRow());
    const QModelIndex idx = proxy->index(selectedRow(), 0);
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
    m_statusMessage = m_taskListView.openSelectedTaskInBrowser();
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
        if (!AnalyticsService::canExportPdf()) {
            m_statusMessage = tr("PDF export needs a graphical session: run go-for-it-qt --tui instead.");
        } else if (m_app->analytics()->exportBillingPdf(outPath, periodName, m_app->activeListFilter())) {
            m_statusMessage = tr("Billing PDF exported to %1").arg(outPath);
        } else {
            m_statusMessage = tr("Failed to export PDF report.");
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
    if (!m_terminal.isRawMode()) return;
    clampSelection();

    const int cols = m_terminal.cols();
    const int rows = m_terminal.rows();

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
    const int maxBadgeWidth = qMax(10, cols - syncInfoWidth - 2);
    const QString rawTitle = sanitizeTerminalText(m_app->currentFilterTitle());
    const QString rawListBadge = QStringLiteral(" 🍅 Go for It Qt [VIM TUI] │ 📋 %1 (%2) ")
        .arg(rawTitle, m_app->currentListProvider());
    const QString listBadge = truncateVisible(rawListBadge, maxBadgeWidth);
    const int headerGap = qMax(1, cols - displayWidth(listBadge) - syncInfoWidth - 1);
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
    const int availActiveCols = qMax(8, cols - 54);
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
    out += m_taskListView.renderTabRow(cols);

    // -------------------------------------------------------------------------
    // Row 4: Horizontal Divider
    // -------------------------------------------------------------------------
    out += FG_GRAY + QString(cols, QChar(0x2500)) + ESC_RESET + QStringLiteral("\033[K\r\n");

    // Middle viewport (rows 5 .. rows - 2)
    const int bodyRows = qMax(4, rows - 6);

    if (m_mode == Mode::Help) {
        out += m_taskListView.renderHelp(bodyRows, cols);
    } else if (m_mode == Mode::MarkdownView) {
        out += m_markdownView.render(bodyRows, cols);
    } else if (m_mode == Mode::Settings || m_mode == Mode::EditSettingValue) {
        out += m_settingsView.render(bodyRows, cols);
    } else if (m_mode == Mode::ListEdit || m_mode == Mode::EditListField) {
        out += m_listManagerView.renderEdit(bodyRows, cols);
    } else if (m_mode == Mode::ListPicker) {
        out += m_listManagerView.renderPicker(bodyRows, cols);
    } else if (m_mode == Mode::Dashboard) {
        out += m_dashboardView.render(bodyRows, cols);
    } else {
        out += m_taskListView.renderTaskList(bodyRows, cols);
    }

    // -------------------------------------------------------------------------
    // Row rows - 1: Vim Statusline
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

    const int totalTasks = currentProxy() ? currentProxy()->rowCount() : 0;
    const QString posTag = QStringLiteral(" %1/%2 ")
        .arg(totalTasks > 0 ? (selectedRow() + 1) : 0)
        .arg(totalTasks);
    out += modeTag + BG_HEADER + FG_WHITE
        + padRight(QStringLiteral(" %1 │ %2").arg(sanitizeTerminalText(m_app->currentFilterTitle()), posTag), cols - 9)
        + ESC_RESET + QStringLiteral("\033[K\r\n");

    // -------------------------------------------------------------------------
    // Row rows: Command / Input / Status Message Bar
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
        out += FG_GRAY + truncateVisible(sanitizeTerminalText(m_statusMessage), cols - 1) + ESC_RESET + QStringLiteral("\033[K");
    }

    m_terminal.write(out);
}

} // namespace gofi
