#include "AppController.h"
#include "DatabaseManager.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

namespace gofi {

AppController::AppController(const QString& logFilePath, const QString& customDbPath, QObject* parent)
    : QObject(parent)
    , m_settings(this)
    , m_ukTranslator(this)
    , m_shortcuts(this)
    , m_timer(&m_settings, this)
    , m_notifications(&m_settings, &m_timer, this)
    , m_listManager((DatabaseManager::instance().initialize(customDbPath), this))
    , m_issueTracker(this)
    , m_taskManager(this)
    , m_todoProxy(this)
    , m_doneProxy(this)
    , m_activityLog(logFilePath)
    , m_analytics(this)
    , m_ambient(&m_timer, this)
    , m_systemHooks(this, &m_settings, &m_timer, this)
    , m_desktopIntegration(this, &m_settings, &m_timer, this)
{
    applyLanguageTranslator();
    connect(&m_settings, &SettingsManager::languageChanged, this, [this]() {
        applyLanguageTranslator();
        emit languageChanged();
    });

    m_todoProxy.setSourceModel(m_taskManager.todoModel());
    m_doneProxy.setSourceModel(m_taskManager.doneModel());

    connect(&m_timer, &TaskTimer::timerStopped, this, &AppController::onTimerStopped);
    connect(&m_timer, &TaskTimer::timerFinished, this, [this](bool wasBreak) {
        if (!wasBreak) {
            m_lastSessionCompletedPomodoro = true;
        }
    });
    connect(&m_timer, &TaskTimer::activeTaskTimeUpdated, this, &AppController::onActiveTaskTimeUpdated);
    connect(&m_notifications, &NotificationService::desktopNotification, this, [this](const QString& title, const QString& body) {
        m_desktopIntegration.showTrayMessage(title, body);
        emit notificationRequested(title, body);
    });
    connect(&m_desktopIntegration, &DesktopIntegration::showWindowRequested, this, &AppController::showWindowRequested);
    connect(&m_desktopIntegration, &DesktopIntegration::quitRequested, this, &AppController::quitRequested);

    connect(&m_issueTracker, &IssueTrackerService::listIssuesSynced, this, [this](const QString& listId, int) {
        if (m_currentListId == listId) {
            m_taskManager.refresh();
            if (m_activeTodoRow < 0 && m_taskManager.todoModel()->rowCount() > 0) {
                setActiveTodoRow(0);
            } else {
                syncActiveTaskToTimer();
            }
        }
    });
    connect(&m_issueTracker, &IssueTrackerService::trackerChangesDetected, this,
            [this](const QString& listName, const QStringList& newIssues, const QStringList& statusChanges) {
        QStringList lines;
        if (!newIssues.isEmpty()) {
            lines.append(tr("New tasks (%1):").arg(newIssues.size()));
            for (int i = 0; i < qMin(3, static_cast<int>(newIssues.size())); ++i) {
                lines.append(QStringLiteral("• ") + newIssues.at(i));
            }
        }
        if (!statusChanges.isEmpty()) {
            lines.append(tr("Status changed (%1):").arg(statusChanges.size()));
            for (int i = 0; i < qMin(3, static_cast<int>(statusChanges.size())); ++i) {
                lines.append(QStringLiteral("• ") + statusChanges.at(i));
            }
        }
        if (!lines.isEmpty()) {
            m_notifications.notifyTaskEvent(
                tr("Updates in '%1'").arg(listName),
                lines.join(QLatin1Char('\n'))
            );
        }
    });

    connect(&m_autoSyncTimer, &QTimer::timeout, this, &AppController::syncAllRemoteLists);
    connect(&m_settings, &SettingsManager::autoSyncChanged, this, &AppController::updateAutoSyncTimer);
    updateAutoSyncTimer();

    connect(&m_dueReminderTimer, &QTimer::timeout, this, &AppController::checkDueReminders);
    if (!QStandardPaths::isTestModeEnabled()) {
        m_dueReminderTimer.start(15 * 60 * 1000);
    }

    connect(m_taskManager.todoModel(), &TaskStoreModel::countChanged, this, &AppController::onTodoListCountChanged);
    connect(m_taskManager.todoModel(), &TaskStoreModel::unviewedTodoCountChanged, this, &AppController::todoCountsChanged);
    connect(m_taskManager.todoModel(), &TaskStoreModel::totalTodoCountChanged, this, [this]() {
        emit todoCountsChanged();
        emit totalTodoCountChanged();
    });
    connect(&m_listManager, &ListManagerModel::listRemoved, this, [this](const QString& id) {
        if (m_currentListId == id) {
            setShowingOverview(true);
        }
    });
    connect(&m_listManager, &ListManagerModel::listUpdated, this, [this](const QString& id) {
        if (m_currentListId == id) {
            selectListById(id);
        }
    });

    const QString lastId = m_settings.lastLoadedListId();
    if (!lastId.isEmpty() && m_listManager.indexOfId(lastId) >= 0) {
        selectListById(lastId);
    } else if (m_listManager.rowCount() > 0) {
        selectListById(m_listManager.entry(0).id);
    } else {
        m_showingOverview = true;
    }
}

void AppController::setShowingOverview(bool value) {
    if (m_showingOverview == value) return;
    m_showingOverview = value;
    emit showingOverviewChanged();
}

void AppController::setActiveTodoRow(int sourceRow) {
    const int count = m_taskManager.todoModel()->rowCount();
    const int clamped = (sourceRow >= 0 && sourceRow < count) ? sourceRow : (count > 0 ? 0 : -1);
    if (m_activeTodoRow == clamped) {
        syncActiveTaskToTimer();
        return;
    }
    m_activeTodoRow = clamped;
    emit activeTodoRowChanged();
    syncActiveTaskToTimer();
}

bool AppController::isCurrentListRemote() const {
    if (m_isMultiListView) {
        for (int i = 0; i < m_listManager.rowCount(); ++i) {
            const TodoListEntry e = m_listManager.entry(i);
            if (m_activeFilterListIds.isEmpty() || m_activeFilterListIds.contains(e.id)) {
                if (e.provider != QStringLiteral("SQLite") && e.provider != QStringLiteral("Todo.txt")) {
                    return true;
                }
            }
        }
        return false;
    }
    return m_currentListProvider == QStringLiteral("Jira")
        || m_currentListProvider == QStringLiteral("YouTrack")
        || m_currentListProvider == QStringLiteral("GitHub")
        || m_currentListProvider == QStringLiteral("GitLab");
}

QString AppController::currentFilterTitle() const {
    if (m_isMultiListView) {
        return m_currentFilterTitle.isEmpty() ? tr("All Lists") : m_currentFilterTitle;
    }
    return m_currentListName;
}

void AppController::selectListById(const QString& listId) {
    const int row = m_listManager.indexOfId(listId);
    if (row < 0) return;

    const TodoListEntry entry = m_listManager.entry(row);
    m_currentListId = entry.id;
    m_currentListName = entry.name;
    m_currentListProvider = entry.provider;
    m_currentListActivityLogPath = entry.activityLogPath;
    m_activeFilterListIds = QStringList{entry.id};
    m_isMultiListView = false;
    m_currentFilterTitle = entry.name;
    m_settings.setLastLoadedListId(entry.id);

    m_timer.setListTimerOverrides(entry.customSchedule, entry.reminderTime);
    const bool isLocalList = (entry.provider == QStringLiteral("SQLite") || entry.provider == QStringLiteral("Todo.txt"));
    m_taskManager.setActiveList(
        entry.id,
        isLocalList,
        entry.addCreationDates
    );
    setActiveTodoRow(m_taskManager.todoModel()->rowCount() > 0 ? 0 : -1);
    setShowingOverview(false);
    emit currentListChanged();

    if (entry.provider != QStringLiteral("SQLite") && entry.provider != QStringLiteral("Todo.txt")) {
        m_issueTracker.syncListIssues(entry);
    }
}

void AppController::showAllLists() {
    m_isMultiListView = true;
    m_activeFilterListIds.clear();
    m_currentFilterTitle = tr("All Lists");
    m_currentListName = tr("All Lists");
    m_currentListProvider = QStringLiteral("All");

    m_taskManager.setActiveLists(QStringList{QStringLiteral("__ALL__")}, m_currentListId, true);
    setActiveTodoRow(m_taskManager.todoModel()->rowCount() > 0 ? 0 : -1);
    setShowingOverview(false);
    emit currentListChanged();
    syncAllRemoteLists();
}

void AppController::showProviderLists(const QString& provider) {
    QStringList matchingIds;
    for (int i = 0; i < m_listManager.rowCount(); ++i) {
        const TodoListEntry e = m_listManager.entry(i);
        if (e.provider.compare(provider, Qt::CaseInsensitive) == 0) {
            matchingIds.append(e.id);
        }
    }
    if (matchingIds.isEmpty()) return;

    if (matchingIds.size() == 1) {
        selectListById(matchingIds.first());
        return;
    }

    m_isMultiListView = true;
    m_activeFilterListIds = matchingIds;
    m_currentFilterTitle = provider;
    m_currentListName = provider;
    m_currentListProvider = provider;

    m_taskManager.setActiveLists(matchingIds, matchingIds.first(), true);
    setActiveTodoRow(m_taskManager.todoModel()->rowCount() > 0 ? 0 : -1);
    setShowingOverview(false);
    emit currentListChanged();

    for (const QString& id : matchingIds) {
        const int idx = m_listManager.indexOfId(id);
        if (idx >= 0) {
            const TodoListEntry e = m_listManager.entry(idx);
            if (e.provider != QStringLiteral("SQLite") && e.provider != QStringLiteral("Todo.txt")) {
                m_issueTracker.syncListIssues(e);
            }
        }
    }
}

void AppController::setMultiListFilter(const QStringList& listIds, const QString& customTitle) {
    if (listIds.isEmpty()) {
        showAllLists();
        return;
    }
    if (listIds.size() == 1) {
        selectListById(listIds.first());
        return;
    }

    m_isMultiListView = true;
    m_activeFilterListIds = listIds;
    m_currentFilterTitle = customTitle.trimmed().isEmpty() ? tr("%1 Lists").arg(listIds.size()) : customTitle.trimmed();
    m_currentListName = m_currentFilterTitle;

    m_taskManager.setActiveLists(listIds, listIds.first(), true);
    setActiveTodoRow(m_taskManager.todoModel()->rowCount() > 0 ? 0 : -1);
    setShowingOverview(false);
    emit currentListChanged();
}

void AppController::syncCurrentList() {
    if (m_isMultiListView) {
        syncAllRemoteLists();
        return;
    }
    const int row = m_listManager.indexOfId(m_currentListId);
    if (row < 0) return;
    const TodoListEntry entry = m_listManager.entry(row);
    if (entry.provider != QStringLiteral("SQLite") && entry.provider != QStringLiteral("Todo.txt")) {
        m_issueTracker.syncListIssues(entry);
    } else {
        m_taskManager.refresh();
    }
}

void AppController::syncAllRemoteLists() {
    if (m_issueTracker.isSyncing()) return;
    for (int i = 0; i < m_listManager.rowCount(); ++i) {
        const TodoListEntry entry = m_listManager.entry(i);
        if (m_isMultiListView && !m_activeFilterListIds.isEmpty() && !m_activeFilterListIds.contains(entry.id)) {
            continue;
        }
        if (entry.provider != QStringLiteral("SQLite") && entry.provider != QStringLiteral("Todo.txt")) {
            m_issueTracker.syncListIssues(entry);
        }
    }
}

void AppController::addNewTask(const QString& simpleText) {
    m_taskManager.addNewTask(simpleText, m_settings.newTasksOnTop(), m_taskManager.addCreationDates());
    if (m_activeTodoRow < 0 && m_taskManager.todoModel()->rowCount() > 0) {
        setActiveTodoRow(0);
    }
    if (!simpleText.trimmed().isEmpty()) {
        m_notifications.notifyTaskEvent(tr("New task added"), simpleText.trimmed());
    }
}

void AppController::toggleTaskDone(bool fromDoneList, int proxyRow) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return;

    const QModelIndex idx = store->index(sourceRow, 0);
    const QString desc = store->data(idx, TaskStoreModel::DescriptionRole).toString();
    const bool currentDone = store->data(idx, TaskStoreModel::DoneRole).toBool();
    const bool willBeDone = !currentDone;

    // Pre-fetch metadata BEFORE mutating the model, because setData triggers taskDoneToggled
    // which immediately removes the row from the store.
    const QString remoteKey = store->data(idx, TaskStoreModel::JiraKeyRole).toString();
    const QString taskId = store->data(idx, TaskStoreModel::IdRole).toString();
    const QString listId = store->data(idx, TaskStoreModel::ListIdRole).toString();
    const QString effectiveListId = !listId.isEmpty() ? listId : m_currentListId;

    store->setData(idx, willBeDone, TaskStoreModel::DoneRole);
    m_notifications.notifyTaskEvent(
        tr("Task status changed"),
        QStringLiteral("%1 → %2").arg(desc, willBeDone ? tr("Done") : tr("To-Do"))
    );
    if (!remoteKey.isEmpty() && !effectiveListId.isEmpty()) {
        for (const TodoListEntry& listEntry : m_listManager.entries()) {
            if (listEntry.id == effectiveListId && listEntry.provider != QStringLiteral("SQLite")) {
                m_issueTracker.updateIssueStatusRemote(listEntry, remoteKey, willBeDone, willBeDone ? QStringLiteral("done") : QStringLiteral("todo"));
                break;
            }
        }
    }
    if (willBeDone) {
        m_systemHooks.onTaskCompleted(desc, remoteKey, taskId);
    }
}

void AppController::changeTaskStatus(bool fromDoneList, int proxyRow, const QString& newStatus) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return;

    const QModelIndex idx = store->index(sourceRow, 0);
    const QString desc = store->data(idx, TaskStoreModel::DescriptionRole).toString();
    const QString oldStatus = store->data(idx, TaskStoreModel::StatusRole).toString();

    // Pre-fetch metadata and check for completion status case-insensitively BEFORE mutating model
    const QString remoteKey = store->data(idx, TaskStoreModel::JiraKeyRole).toString();
    const QString taskId = store->data(idx, TaskStoreModel::IdRole).toString();
    const QString listId = store->data(idx, TaskStoreModel::ListIdRole).toString();
    const QString effectiveListId = !listId.isEmpty() ? listId : m_currentListId;
    const bool isCompleting = (newStatus.compare(QStringLiteral("done"), Qt::CaseInsensitive) == 0);

    store->setTaskStatus(sourceRow, newStatus);
    if (oldStatus != newStatus) {
        m_notifications.notifyTaskEvent(
            tr("Task status changed"),
            QStringLiteral("%1: %2 → %3").arg(desc, oldStatus, newStatus)
        );
        if (!remoteKey.isEmpty() && !effectiveListId.isEmpty()) {
            for (const TodoListEntry& listEntry : m_listManager.entries()) {
                if (listEntry.id == effectiveListId && listEntry.provider != QStringLiteral("SQLite")) {
                    m_issueTracker.updateIssueStatusRemote(listEntry, remoteKey, isCompleting, newStatus);
                    break;
                }
            }
        }
        if (isCompleting) {
            m_systemHooks.onTaskCompleted(desc, remoteKey, taskId);
        }
    }
}

void AppController::changeTaskPriority(bool fromDoneList, int proxyRow, const QString& priorityLetter) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return;

    quint8 prio = TxtTask::NoPriority;
    const QString p = priorityLetter.trimmed().toUpper();
    if (!p.isEmpty() && p != QStringLiteral("NONE") && p != QStringLiteral("CLEAR")) {
        const QChar ch = p.at(0);
        if (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) {
            prio = static_cast<quint8>(ch.unicode() - 'A');
        }
    }

    const QModelIndex idx = store->index(sourceRow, 0);
    store->setData(idx, prio, TaskStoreModel::PriorityRole);
}

void AppController::updateTaskText(bool fromDoneList, int proxyRow, const QString& simpleText) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return;

    store->updateTaskText(sourceRow, simpleText);
    if (!fromDoneList && sourceRow == m_activeTodoRow) {
        syncActiveTaskToTimer();
    }
}

void AppController::updateTaskDetails(bool fromDoneList, int proxyRow, const QString& details) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return;

    store->updateTaskDetails(sourceRow, details);

    const TxtTask task = store->taskAt(sourceRow);
    const QString targetListId = task.listId().isEmpty() ? m_currentListId : task.listId();
    const int listIdx = m_listManager.indexOfId(targetListId);
    if (listIdx >= 0 && !task.jiraKey().isEmpty()) {
        const TodoListEntry listEntry = m_listManager.entry(listIdx);
        if (listEntry.provider != QStringLiteral("SQLite") && listEntry.provider != QStringLiteral("Todo.txt")) {
            m_issueTracker.updateIssueDescription(listEntry, task.jiraKey(), details);
        }
    }
}

void AppController::addTaskComment(bool fromDoneList, int proxyRow, const QString& commentText) {
    if (commentText.trimmed().isEmpty()) return;
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return;

    const TxtTask task = store->taskAt(sourceRow);
    const QString targetListId = task.listId().isEmpty() ? m_currentListId : task.listId();
    const int listIdx = m_listManager.indexOfId(targetListId);
    QString author = QStringLiteral("Me");
    if (listIdx >= 0) {
        const TodoListEntry listEntry = m_listManager.entry(listIdx);
        if (!listEntry.jiraUser.trimmed().isEmpty()) {
            author = listEntry.jiraUser.trimmed();
        }
    }
    store->appendTaskComment(sourceRow, author, commentText);

    if (listIdx >= 0 && !task.jiraKey().isEmpty()) {
        const TodoListEntry listEntry = m_listManager.entry(listIdx);
        if (listEntry.provider != QStringLiteral("SQLite") && listEntry.provider != QStringLiteral("Todo.txt")) {
            m_issueTracker.postIssueComment(listEntry, task.jiraKey(), commentText);
        }
    }
}

int AppController::unviewedTodoCount() const {
    return m_taskManager.todoModel()->unviewedTodoCount();
}

int AppController::totalTodoCount() const {
    return m_taskManager.todoModel()->totalTodoCount();
}

void AppController::markTaskViewed(bool fromDoneList, int proxyRow) {
    if (fromDoneList) return;
    const int sourceRow = m_todoProxy.mapToSourceRow(proxyRow);
    if (sourceRow >= 0) {
        m_taskManager.todoModel()->markTaskViewed(sourceRow);
    }
}

void AppController::removeTask(bool fromDoneList, int proxyRow) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return;

    store->removeAt(sourceRow);
}

void AppController::moveTaskRow(bool fromDoneList, int fromProxyRow, int toProxyRow) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int fromSource = proxy.mapToSourceRow(fromProxyRow);
    const int toSource = proxy.mapToSourceRow(toProxyRow);
    if (fromSource < 0 || toSource < 0 || fromSource == toSource) return;

    store->moveItem(fromSource, toSource);
    if (!fromDoneList && m_activeTodoRow == fromSource) {
        m_activeTodoRow = toSource;
        emit activeTodoRowChanged();
    }
}

void AppController::moveActiveTaskByOffset(int delta) {
    const int count = m_taskManager.todoModel()->rowCount();
    if (m_activeTodoRow < 0 || m_activeTodoRow >= count) return;
    const int target = qBound(0, m_activeTodoRow + delta, count - 1);
    if (target != m_activeTodoRow) {
        m_taskManager.todoModel()->moveItem(m_activeTodoRow, target);
        m_activeTodoRow = target;
        emit activeTodoRowChanged();
    }
}

void AppController::switchToNextTask() {
    const int count = m_taskManager.todoModel()->rowCount();
    if (count == 0) return;
    setActiveTodoRow(qMin(count - 1, m_activeTodoRow + 1));
}

void AppController::switchToPreviousTask() {
    const int count = m_taskManager.todoModel()->rowCount();
    if (count == 0) return;
    setActiveTodoRow(qMax(0, m_activeTodoRow - 1));
}

void AppController::markActiveTaskDone() {
    if (m_activeTodoRow < 0 || m_activeTodoRow >= m_taskManager.todoModel()->rowCount()) {
        return;
    }
    const bool wasRunning = m_timer.isRunning();
    m_timer.stop();
    const QModelIndex idx = m_taskManager.todoModel()->index(m_activeTodoRow, 0);
    m_taskManager.todoModel()->setData(idx, true, TaskStoreModel::DoneRole);

    if (wasRunning && m_timer.hasActiveTask() && (m_timer.isBreakActive() || !m_settings.resetTimerOnTaskSwitch())) {
        m_timer.start();
    }
}

void AppController::clearDoneTasks() {
    m_taskManager.clearDoneList();
}

void AppController::sortAllTasks() {
    m_taskManager.sortTasks();
    syncActiveTaskToTimer();
}

QString AppController::databasePath() const {
    return DatabaseManager::instance().databasePath();
}

QString AppController::configFilePath() const {
    QSettings s(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("settings"));
    return s.fileName();
}

bool AppController::changeDatabasePath(const QString& newDbPath, bool copyExisting) {
    if (!DatabaseManager::instance().relocateDatabase(newDbPath, copyExisting)) {
        return false;
    }
    m_listManager.reloadFromDatabase();
    if (m_listManager.rowCount() > 0) {
        selectListById(m_listManager.entry(0).id);
    }
    emit storagePathChanged();
    return true;
}

bool AppController::exportBackup(const QString& targetFilePath) {
    return DatabaseManager::instance().exportDatabaseBackup(targetFilePath);
}

QString AppController::readMarkdownFile(const QString& filePath) const {
    QString cleanPath = filePath.trimmed();
    if (cleanPath.startsWith(QStringLiteral("~/"))) {
        cleanPath = QDir::homePath() + cleanPath.mid(1);
    }
    QFile f(cleanPath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QString::fromUtf8(f.readAll());
}

bool AppController::importBackup(const QString& sourceFilePath) {
    if (!DatabaseManager::instance().importDatabaseBackup(sourceFilePath)) {
        return false;
    }
    m_listManager.reloadFromDatabase();
    if (m_listManager.rowCount() > 0) {
        selectListById(m_listManager.entry(0).id);
    }
    emit storagePathChanged();
    return true;
}

void AppController::onTimerStopped(const QDateTime& startTimeUtc, uint runtimeSeconds) {
    if (m_timer.isBreakActive() || runtimeSeconds == 0) {
        return;
    }
    const QDateTime stopUtc = QDateTime::currentDateTimeUtc();
    qint64 activeTaskId = -1;
    QString jiraKey;
    QString targetListId = m_currentListId;
    QString targetListName = m_currentListName;
    if (m_activeTodoRow >= 0 && m_activeTodoRow < m_taskManager.todoModel()->rowCount()) {
        const TxtTask activeTask = m_taskManager.todoModel()->taskAt(m_activeTodoRow);
        activeTaskId = activeTask.id();
        jiraKey = activeTask.jiraKey();
        if (!activeTask.listId().isEmpty()) {
            targetListId = activeTask.listId();
            targetListName = activeTask.listName().isEmpty() ? m_currentListName : activeTask.listName();
        }
    }
    const bool completedPomodoro = m_lastSessionCompletedPomodoro;
    m_lastSessionCompletedPomodoro = false;

    const qint64 logId = m_activityLog.logTask(
        targetListId,
        targetListName,
        activeTaskId,
        jiraKey,
        m_timer.activeTaskDescription(),
        startTimeUtc,
        runtimeSeconds,
        stopUtc,
        completedPomodoro
    );
    if (!m_currentListActivityLogPath.isEmpty()) {
        ActivityLog::logTaskInFile(
            m_currentListActivityLogPath,
            targetListName,
            m_timer.activeTaskDescription(),
            startTimeUtc,
            runtimeSeconds,
            stopUtc
        );
    }

    const int listIdx = m_listManager.indexOfId(targetListId);
    if (listIdx >= 0 && !jiraKey.isEmpty()) {
        const TodoListEntry listEntry = m_listManager.entry(listIdx);
        if (listEntry.provider != QStringLiteral("SQLite") && listEntry.provider != QStringLiteral("Todo.txt")
            && listEntry.autoWorklog) {
            m_issueTracker.postWorklog(listEntry, activeTaskId, logId, jiraKey, startTimeUtc, runtimeSeconds);
        }
    }
}

void AppController::onActiveTaskTimeUpdated(uint totalSeconds) {
    if (m_activeTodoRow >= 0 && m_activeTodoRow < m_taskManager.todoModel()->rowCount()) {
        m_taskManager.todoModel()->setTaskTimerValue(m_activeTodoRow, totalSeconds);
    }
}

void AppController::onTodoListCountChanged() {
    const int count = m_taskManager.todoModel()->rowCount();
    if (count == 0) {
        m_activeTodoRow = -1;
        emit activeTodoRowChanged();
        m_timer.clearActiveTask();
    } else if (m_activeTodoRow < 0 || m_activeTodoRow >= count) {
        m_activeTodoRow = qMin(qMax(0, m_activeTodoRow), count - 1);
        emit activeTodoRowChanged();
        syncActiveTaskToTimer();
    } else {
        syncActiveTaskToTimer();
    }
    checkDueReminders();
}

void AppController::applyLanguageTranslator() {
    if (!QCoreApplication::instance()) return;
    QCoreApplication::removeTranslator(&m_ukTranslator);
    const QString eff = m_settings.effectiveLanguage();
    if (eff != QStringLiteral("en")) {
        m_ukTranslator.setLanguageCode(eff);
        QCoreApplication::installTranslator(&m_ukTranslator);
    }
}

void AppController::updateAutoSyncTimer() {
    const int mins = m_settings.autoSyncIntervalMinutes();
    if (mins > 0) {
        m_autoSyncTimer.start(mins * 60 * 1000);
    } else {
        m_autoSyncTimer.stop();
    }
}

void AppController::syncActiveTaskToTimer() {
    if (m_activeTodoRow < 0 || m_activeTodoRow >= m_taskManager.todoModel()->rowCount()) {
        m_timer.clearActiveTask();
        return;
    }
    const TxtTask task = m_taskManager.todoModel()->taskAt(m_activeTodoRow);
    m_timer.setActiveTask(task.description(), task.timerValue(), task.duration());
}

QUrl AppController::getTaskBrowserUrl(const QString& listId, const QString& jiraKey, const QString& description) const {
    TodoListEntry listEntry;
    const int listIdx = m_listManager.indexOfId(listId);
    if (listIdx >= 0) {
        listEntry = m_listManager.entry(listIdx);
    }
    return IssueTrackerService::buildIssueBrowserUrl(listEntry, jiraKey, description);
}

QUrl AppController::taskBrowserUrl(bool fromDoneList, int proxyRow) const {
    const TaskFilterProxyModel* proxy = fromDoneList ? &m_doneProxy : &m_todoProxy;
    if (proxyRow < 0 || proxyRow >= proxy->rowCount()) return QUrl();

    const QModelIndex idx = proxy->index(proxyRow, 0);
    const QString listId = proxy->data(idx, TaskStoreModel::ListIdRole).toString();
    const QString jiraKey = proxy->data(idx, TaskStoreModel::JiraKeyRole).toString();
    const QString description = proxy->data(idx, TaskStoreModel::DescriptionRole).toString();

    return getTaskBrowserUrl(listId, jiraKey, description);
}

void AppController::openTaskInBrowser(bool fromDoneList, int proxyRow) {
    const QUrl url = taskBrowserUrl(fromDoneList, proxyRow);
    if (!url.isValid() || url.isEmpty()) {
        m_notifications.notifyTaskEvent(tr("Go for It"), tr("No web URL available for this task."));
        return;
    }
    openUrlExternally(url);
}

void AppController::openUrlExternally(const QUrl& url) {
    if (!url.isValid() || url.isEmpty()) return;
    const bool opened = QDesktopServices::openUrl(url);
    if (!opened) {
        QProcess::startDetached(QStringLiteral("xdg-open"), {url.toString()});
    }
    m_issueTracker.setStatusMessage(tr("Opening %1 in browser...").arg(url.toString()));
}

void AppController::checkDueReminders() {
    TaskStoreModel* model = m_taskManager.todoModel();
    if (!model) return;

    const int total = model->rowCount();
    for (int i = 0; i < total; ++i) {
        const TxtTask task = model->taskAt(i);
        if (task.isDone() || !task.dueDate().isValid()) continue;

        if (m_notifiedDueTaskIds.contains(task.id())) continue;

        if (task.isOverdue()) {
            m_notifications.notifyTaskEvent(
                tr("Task Overdue"),
                tr("Task '%1' is overdue (was due %2)").arg(task.description(), task.dueDateString())
            );
            m_notifiedDueTaskIds.insert(task.id());
        } else if (task.isDueToday()) {
            m_notifications.notifyTaskEvent(
                tr("Task Due Today"),
                tr("Task '%1' is due today!").arg(task.description())
            );
            m_notifiedDueTaskIds.insert(task.id());
        }
    }
}


} // namespace gofi
