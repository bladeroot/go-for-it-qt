#include "tui/TuiTaskListView.h"
#include "tui/TuiUtils.h"
#include "tui/TuiMarkdownView.h"
#include "core/AppController.h"
#include "core/TaskTimer.h"
#include "core/ListManagerModel.h"
#include "todotxt/TaskFilterProxyModel.h"
#include "todotxt/TaskStoreModel.h"
#include "todotxt/TxtTask.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QUrl>

namespace gofi {

TuiTaskListView::TuiTaskListView(AppController* controller)
    : m_app(controller) {
}

void TuiTaskListView::setController(AppController* controller) {
    m_app = controller;
}

void TuiTaskListView::setViewTab(int tab) {
    switchViewTab(tab);
}

bool TuiTaskListView::isClosedTab() const {
    return m_viewTab >= 4;
}

TaskFilterProxyModel* TuiTaskListView::currentProxy() const {
    if (!m_app) return nullptr;
    return isClosedTab() ? m_app->doneProxyModel() : m_app->todoProxyModel();
}

void TuiTaskListView::switchViewTab(int tabIndex) {
    if (!m_app) return;
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

void TuiTaskListView::clampSelection() {
    TaskFilterProxyModel* proxy = currentProxy();
    const int count = proxy ? proxy->rowCount() : 0;
    if (count <= 0) {
        m_selectedRow = 0;
        m_scrollOffset = 0;
        return;
    }
    m_selectedRow = qBound(0, m_selectedRow, count - 1);
}

void TuiTaskListView::resetSelection() {
    m_selectedRow = 0;
    m_scrollOffset = 0;
    m_detailsScroll = 0;
    m_pendingKey = 0;
}

QString TuiTaskListView::openSelectedTaskInBrowser() {
    auto tr = [](const char* sourceText) {
        return QCoreApplication::translate("TuiApp", sourceText);
    };
    TaskFilterProxyModel* proxy = currentProxy();
    if (!proxy || !m_app) return {};
    const int count = proxy->rowCount();
    if (count <= 0 || m_selectedRow < 0 || m_selectedRow >= count) return {};

    const QModelIndex idx = proxy->index(m_selectedRow, 0);
    const QString listId = proxy->data(idx, TaskStoreModel::ListIdRole).toString();
    const QString jiraKey = proxy->data(idx, TaskStoreModel::JiraKeyRole).toString();
    const QString desc = proxy->data(idx, TaskStoreModel::DescriptionRole).toString();

    const QUrl taskUrl = m_app->getTaskBrowserUrl(listId, jiraKey, desc);
    if (taskUrl.isValid() && !taskUrl.isEmpty()) {
        m_app->openUrlExternally(taskUrl);
        return tr("Opening in browser: %1").arg(taskUrl.toString());
    }
    return tr("No web URL available for selected task.");
}

TaskListKeyResult TuiTaskListView::handleKey(const QByteArray& seq, QString& statusMessage, QString& initialText) {
    auto tr = [](const char* sourceText) {
        return QCoreApplication::translate("TuiApp", sourceText);
    };
    TaskFilterProxyModel* proxy = currentProxy();
    const int count = proxy ? proxy->rowCount() : 0;

    // Handle 2-key Vim combos (gg, gt, gT, dd, cw)
    if (m_pendingKey != 0) {
        const char prev = m_pendingKey;
        m_pendingKey = 0;
        if (prev == 'g' && seq == "g") {
            m_selectedRow = 0;
            m_detailsScroll = 0;
            return TaskListKeyResult::Handled;
        }
        if (prev == 'g' && seq == "t") {
            switchViewTab((m_viewTab + 1) % 6);
            return TaskListKeyResult::Handled;
        }
        if (prev == 'g' && seq == "T") {
            switchViewTab((m_viewTab + 5) % 6);
            return TaskListKeyResult::Handled;
        }
        if (prev == 'd' && seq == "d") {
            if (count > 0 && m_app) {
                m_app->removeTask(isClosedTab(), m_selectedRow);
                clampSelection();
                statusMessage = tr("Task deleted.");
            }
            return TaskListKeyResult::Handled;
        }
        if (prev == 'c' && (seq == "w" || seq == "c")) {
            if (count > 0 && proxy) {
                const QModelIndex idx = proxy->index(m_selectedRow, 0);
                initialText = proxy->data(idx, TaskStoreModel::SimpleTextRole).toString();
                return TaskListKeyResult::StartEditTitle;
            }
            return TaskListKeyResult::Handled;
        }
        if (prev == 'g' && seq == "x") {
            if (count > 0) {
                statusMessage = openSelectedTaskInBrowser();
            }
            return TaskListKeyResult::Handled;
        }
    }

    if (seq == "g" || seq == "d" || seq == "c") {
        m_pendingKey = seq.at(0);
        return TaskListKeyResult::Handled;
    }

    // Navigation: j / Down
    if (seq == "j" || seq == "\033[B") {
        if (count > 0) {
            m_selectedRow = qMin(count - 1, m_selectedRow + 1);
            m_detailsScroll = 0;
        }
        return TaskListKeyResult::Handled;
    }
    // Navigation: k / Up
    if (seq == "k" || seq == "\033[A") {
        if (count > 0) {
            m_selectedRow = qMax(0, m_selectedRow - 1);
            m_detailsScroll = 0;
        }
        return TaskListKeyResult::Handled;
    }
    // Jump to bottom: G
    if (seq == "G") {
        if (count > 0) {
            m_selectedRow = count - 1;
            m_detailsScroll = 0;
        }
        return TaskListKeyResult::Handled;
    }
    // Half-page down: Ctrl+D
    if (seq == "\x04") {
        if (count > 0) m_selectedRow = qMin(count - 1, m_selectedRow + 8);
        return TaskListKeyResult::Handled;
    }
    // Half-page up: Ctrl+U
    if (seq == "\x15") {
        if (count > 0) m_selectedRow = qMax(0, m_selectedRow - 8);
        return TaskListKeyResult::Handled;
    }
    // Scroll details pane down/up: Ctrl+E / Ctrl+Y or PgDn / PgUp
    if (seq == "\x05" || seq == "\033[6~") {
        ++m_detailsScroll;
        return TaskListKeyResult::Handled;
    }
    if (seq == "\x19" || seq == "\033[5~") {
        m_detailsScroll = qMax(0, m_detailsScroll - 1);
        return TaskListKeyResult::Handled;
    }

    // Reorder task down/up: J / K
    if (seq == "J") {
        if (count > 1 && m_selectedRow < count - 1 && m_app) {
            m_app->moveTaskRow(isClosedTab(), m_selectedRow, m_selectedRow + 1);
            ++m_selectedRow;
        }
        return TaskListKeyResult::Handled;
    }
    if (seq == "K") {
        if (count > 1 && m_selectedRow > 0 && m_app) {
            m_app->moveTaskRow(isClosedTab(), m_selectedRow, m_selectedRow - 1);
            --m_selectedRow;
        }
        return TaskListKeyResult::Handled;
    }

    // Switch status tabs: h / l / Left / Right / Tab / 0..6
    if (seq == "h" || seq == "\033[D") {
        switchViewTab((m_viewTab + 5) % 6);
        return TaskListKeyResult::Handled;
    }
    if (seq == "l" || seq == "\033[C" || seq == "\t") {
        switchViewTab((m_viewTab + 1) % 6);
        return TaskListKeyResult::Handled;
    }
    if (seq.size() == 1 && seq.at(0) >= '1' && seq.at(0) <= '6') {
        switchViewTab(seq.at(0) - '1');
        return TaskListKeyResult::Handled;
    }
    if (seq == "0") {
        switchViewTab(0);
        return TaskListKeyResult::Handled;
    }

    // Toggle task Done: Space or x
    if (seq == " " || seq == "x") {
        if (count > 0 && m_app) {
            m_app->toggleTaskDone(isClosedTab(), m_selectedRow);
            clampSelection();
        }
        return TaskListKeyResult::Handled;
    }

    // Open status picker: s
    if (seq == "s") {
        if (count > 0) {
            return TaskListKeyResult::OpenStatusPicker;
        }
        return TaskListKeyResult::Handled;
    }

    // Open task / issue in web browser: w
    if (seq == "w") {
        if (count > 0) {
            statusMessage = openSelectedTaskInBrowser();
        }
        return TaskListKeyResult::Handled;
    }

    // Add new task: o or a
    if (seq == "o" || seq == "a" || seq == "A") {
        return TaskListKeyResult::StartInsert;
    }

    // Edit selected task title: i or e
    if (seq == "i" || seq == "e") {
        if (count > 0 && proxy) {
            const QModelIndex idx = proxy->index(m_selectedRow, 0);
            initialText = proxy->data(idx, TaskStoreModel::SimpleTextRole).toString();
            return TaskListKeyResult::StartEditTitle;
        }
        return TaskListKeyResult::Handled;
    }

    // Edit selected task details in $EDITOR / nvim / vim: E
    if (seq == "E") {
        if (count > 0) {
            return TaskListKeyResult::StartEditDetails;
        }
        return TaskListKeyResult::Handled;
    }

    // Add comment to selected task in $EDITOR / nvim / vim: C
    if (seq == "C") {
        if (count > 0) {
            return TaskListKeyResult::StartAddComment;
        }
        return TaskListKeyResult::Handled;
    }

    // Select task for Pomodoro timer: Enter
    if (seq == "\r" || seq == "\n") {
        if (!isClosedTab() && count > 0 && m_app) {
            m_app->markTaskViewed(false, m_selectedRow);
            const int srcRow = m_app->todoProxyModel()->mapToSourceRow(m_selectedRow);
            if (srcRow >= 0) {
                m_app->setActiveTodoRow(srcRow);
                statusMessage = tr("Selected active Pomodoro task.");
            }
        }
        return TaskListKeyResult::Handled;
    }

    // Start / Pause Pomodoro timer: t or S
    if (seq == "t" || seq == "S") {
        if (!isClosedTab() && count > 0 && m_app) {
            const int srcRow = m_app->todoProxyModel()->mapToSourceRow(m_selectedRow);
            if (srcRow >= 0 && m_app->activeTodoRow() != srcRow && !m_app->timer()->isRunning()) {
                m_app->setActiveTodoRow(srcRow);
            }
        }
        if (m_app) {
            m_app->timer()->toggleRunning();
        }
        return TaskListKeyResult::Handled;
    }

    // Skip break / timer phase: B
    if (seq == "B") {
        if (m_app) m_app->timer()->skipIteration();
        return TaskListKeyResult::Handled;
    }
    // Reset current timer phase: R
    if (seq == "R") {
        if (m_app) m_app->timer()->reset();
        return TaskListKeyResult::Handled;
    }

    // Sync current list (Jira / YouTrack / SQLite): r or Ctrl+R
    if (seq == "r" || seq == "\x12") {
        if (m_app) {
            statusMessage = tr("Syncing list '%1'...").arg(m_app->currentFilterTitle());
            m_app->syncCurrentList();
        }
        return TaskListKeyResult::Handled;
    }

    // Open Settings: O (Shift+o)
    if (seq == "O") {
        return TaskListKeyResult::OpenSettings;
    }

    // Cycle priority: P (Shift+P: None -> A -> B -> C -> D -> E -> None)
    if (seq == "P") {
        if (count > 0 && m_selectedRow < count && proxy && m_app) {
            const QModelIndex idx = proxy->index(m_selectedRow, 0);
            const quint8 curPrio = static_cast<quint8>(proxy->data(idx, TaskStoreModel::PriorityRole).toUInt());
            quint8 nextPrio = 0;
            if (curPrio == 0) nextPrio = 1;      // B
            else if (curPrio == 1) nextPrio = 2; // C
            else if (curPrio == 2) nextPrio = 3; // D
            else if (curPrio == 3) nextPrio = 4; // E
            else if (curPrio == 4) nextPrio = TxtTask::NoPriority;
            else nextPrio = 0; // A

            const QString prioStr = (nextPrio < 26) ? QString(QChar('A' + nextPrio)) : QStringLiteral("none");
            m_app->changeTaskPriority(isClosedTab(), m_selectedRow, prioStr);
            statusMessage = tr("Task priority set to: %1").arg(prioStr.toUpper());
        }
        return TaskListKeyResult::Handled;
    }

    // Switch lists: L (picker) or [ / ]
    if (seq == "L") {
        return TaskListKeyResult::OpenListPicker;
    }
    if (seq == "[") {
        if (m_app) {
            const int nLists = m_app->listManager()->rowCount();
            if (nLists > 0) {
                const int cur = qMax(0, m_app->listManager()->indexOfId(m_app->currentListId()));
                const int prev = (cur + nLists - 1) % nLists;
                m_app->selectListById(m_app->listManager()->entry(prev).id);
            }
        }
        return TaskListKeyResult::Handled;
    }
    if (seq == "]") {
        if (m_app) {
            const int nLists = m_app->listManager()->rowCount();
            if (nLists > 0) {
                const int cur = qMax(0, m_app->listManager()->indexOfId(m_app->currentListId()));
                const int next = (cur + 1) % nLists;
                m_app->selectListById(m_app->listManager()->entry(next).id);
            }
        }
        return TaskListKeyResult::Handled;
    }

    // Open Full-Screen Markdown Viewer for task description & comments: v or M
    if (seq == "v" || seq == "M") {
        return TaskListKeyResult::OpenTaskMarkdown;
    }

    // Open Productivity Dashboard & Reports: U
    if (seq == "U") {
        return TaskListKeyResult::OpenDashboard;
    }

    // Toggle preview pane: p
    if (seq == "p") {
        m_showPreview = !m_showPreview;
        return TaskListKeyResult::Handled;
    }

    // Search: /
    if (seq == "/") {
        return TaskListKeyResult::StartSearch;
    }

    // Clear search: Esc in Normal mode
    if (seq == "\033") {
        if (m_app) {
            m_app->todoProxyModel()->setFilterQuery(QString());
            m_app->doneProxyModel()->setFilterQuery(QString());
        }
        statusMessage.clear();
        clampSelection();
        return TaskListKeyResult::Handled;
    }

    // Ex Command line: :
    if (seq == ":") {
        return TaskListKeyResult::StartCommand;
    }

    // Help overlay: ?
    if (seq == "?") {
        return TaskListKeyResult::OpenHelp;
    }

    // Quit shortcut: q or ZZ
    if (seq == "q") {
        return TaskListKeyResult::Quit;
    }

    return TaskListKeyResult::Handled;
}

QString TuiTaskListView::renderTabRow(int cols) const {
    using namespace tui;
    auto tr = [](const char* sourceText) {
        return QCoreApplication::translate("TuiApp", sourceText);
    };
    const int unviewed = m_app ? m_app->unviewedTodoCount() : 0;
    const int total = m_app ? m_app->totalTodoCount() : 0;
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
    TaskFilterProxyModel* proxy = currentProxy();
    const QString filterStr = proxy ? proxy->filterQuery() : QString();
    if (!filterStr.isEmpty()) {
        tabRow += FG_YELLOW + QStringLiteral(" 🔍 /%1").arg(filterStr) + ESC_RESET;
    }
    return truncateVisible(tabRow, cols) + QStringLiteral("\033[K\r\n");
}

QString TuiTaskListView::renderHelp(int bodyRows, int cols) const {
    using namespace tui;
    auto tr = [](const char* sourceText) {
        return QCoreApplication::translate("TuiApp", sourceText);
    };
    QString out;
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
                + truncateVisible(helpLines.at(r), cols) + ESC_RESET + QStringLiteral("\033[K\r\n");
        } else {
            out += QStringLiteral("\033[K\r\n");
        }
    }
    return out;
}

QString TuiTaskListView::renderTaskList(int bodyRows, int cols) {
    using namespace tui;
    auto tr = [](const char* sourceText) {
        return QCoreApplication::translate("TuiApp", sourceText);
    };
    QString out;
    const int previewRows = m_showPreview ? qMax(4, bodyRows / 3) : 0;
    const int listRows = qMax(2, bodyRows - (m_showPreview ? (previewRows + 1) : 0));

    // Adjust scrollOffset so m_selectedRow is visible
    if (m_selectedRow < m_scrollOffset) {
        m_scrollOffset = m_selectedRow;
    } else if (m_selectedRow >= m_scrollOffset + listRows) {
        m_scrollOffset = m_selectedRow - listRows + 1;
    }

    TaskFilterProxyModel* proxy = currentProxy();
    const int totalTasks = proxy ? proxy->rowCount() : 0;

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
        const bool isActivePomodoro = (!isClosedTab() && m_app && srcRow == m_app->activeTodoRow());

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
        if (m_app && m_app->isMultiListView()) {
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
        const int availDescCols = qMax(10, cols - 14 - provCols - displayWidth(metaSuffix) - displayWidth(dueBadgePlain));
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

    // Preview Pane
    if (m_showPreview) {
        const QString prevHdr = tr("── Preview (p: toggle, E: edit description, Ctrl+e/Ctrl+y: scroll) ");
        out += FG_GRAY + prevHdr
            + QString(qMax(0, cols - static_cast<int>(prevHdr.size())), QChar(0x2500)) + ESC_RESET + QStringLiteral("\033[K\r\n");

        QStringList previewLines;
        if (totalTasks > 0 && m_selectedRow < totalTasks && proxy) {
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
            const QUrl taskUrl = m_app ? m_app->getTaskBrowserUrl(listId, jiraKey, desc) : QUrl();
            if (taskUrl.isValid() && !taskUrl.isEmpty()) {
                previewLines.append(FG_CYAN + tr("  🌐 Browser: %1 (press 'w' or 'gx' to open)").arg(taskUrl.toString()) + ESC_RESET);
            }

            if (!details.isEmpty()) {
                previewLines.append(TuiMarkdownView::renderMarkdownToAnsi(details, cols));
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
                    previewLines.append(TuiMarkdownView::renderMarkdownToAnsi(cm.value(QStringLiteral("body")).toString(), cols - 2));
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
                out += truncateVisible(previewLines.at(pIdx), cols) + QStringLiteral("\033[K\r\n");
            } else {
                out += QStringLiteral("\033[K\r\n");
            }
        }
    }
    return out;
}

} // namespace gofi
