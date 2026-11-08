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
    , m_databaseReady(customDbPath.isEmpty() ? DatabaseManager::instance().initialize() : DatabaseManager::instance().initialize(customDbPath))
    , m_listManager(this)
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
    if (!m_databaseReady) {
        qCritical() << "Error: Failed to initialize database at:" << (customDbPath.isEmpty() ? QStringLiteral("default") : customDbPath);
    }
    m_settings.setSupportedLanguages(m_ukTranslator.availableLanguageCodes());
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
    connect(&m_issueTracker, &IssueTrackerService::listStatusMappingUpdated, &m_listManager, &ListManagerModel::updateListStatusMapping);

    m_issueTracker.setActiveTaskChecker([this](const QString& listId, const QString& jiraKey) {
        if (m_timer.isRunning() && m_timer.hasActiveTask()) {
            if (!jiraKey.isEmpty() && jiraKey == m_activeTaskJiraKey) {
                if (listId.isEmpty() || m_activeTaskListId.isEmpty() || listId == m_activeTaskListId) {
                    return true;
                }
            }
        }
        return false;
    });

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
    connect(m_taskManager.todoModel(), &TaskStoreModel::tasksReordered, this, [this]() {
        if (m_activeTaskId >= 0) {
            const int found = findActiveTodoRowById(m_activeTaskId);
            if (found >= 0 && found != m_activeTodoRow) {
                m_activeTodoRow = found;
                emit activeTodoRowChanged();
            }
        }
    });
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
        if (m_currentListId == id && !m_isMultiListView) {
            selectListById(id);
        }
    });
    connect(&m_taskManager, &TaskManager::errorOccurred, this, [this](const QString& message) {
        qCritical() << "TaskManager database error:" << message;
        m_notifications.notifyTaskEvent(tr("Database Error"), message);
        emit databaseError(message);
    });
    connect(&m_listManager, &ListManagerModel::errorOccurred, this, [this](const QString& message) {
        qCritical() << "ListManager database error:" << message;
        m_notifications.notifyTaskEvent(tr("Database Error"), message);
        emit databaseError(message);
    });
    connect(&m_issueTracker, &IssueTrackerService::syncFailed, this, [this](const QString&, const QString& message) {
        if (message.contains(QStringLiteral("database"), Qt::CaseInsensitive)) {
            qCritical() << "IssueTracker database error:" << message;
            m_notifications.notifyTaskEvent(tr("Database Error"), message);
            emit databaseError(message);
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
    if (m_timer.isRunning()) {
        m_timer.stop();
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

bool AppController::toggleTaskDone(bool fromDoneList, int proxyRow) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return false;
    return toggleTaskDoneSource(fromDoneList, sourceRow);
}

bool AppController::toggleTaskDoneSource(bool fromDoneList, int sourceRow) {
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    if (sourceRow < 0 || sourceRow >= store->rowCount()) return false;

    const QModelIndex idx = store->index(sourceRow, 0);
    const QString desc = store->data(idx, TaskStoreModel::DescriptionRole).toString();
    const bool currentDone = store->data(idx, TaskStoreModel::DoneRole).toBool();
    const bool willBeDone = !currentDone;

    // Pre-fetch metadata BEFORE mutating the model
    const QString remoteKey = store->data(idx, TaskStoreModel::JiraKeyRole).toString();
    const QString taskId = store->data(idx, TaskStoreModel::IdRole).toString();
    const QString listId = store->data(idx, TaskStoreModel::ListIdRole).toString();
    const QString effectiveListId = !listId.isEmpty() ? listId : m_currentListId;

    const qint64 timedTaskId = m_timer.activeTaskId();
    const qint64 targetTaskId = taskId.toLongLong();
    const bool isTimingThisTask = !fromDoneList && m_timer.isRunning() &&
        (sourceRow == m_activeTodoRow || (timedTaskId >= 0 && targetTaskId == timedTaskId));

    if (isTimingThisTask) {
        m_timer.flushActiveTaskSeconds();
    }

    if (!m_taskManager.toggleTaskDone(fromDoneList, sourceRow)) {
        return false;
    }

    if (isTimingThisTask && m_timer.isRunning()) {
        m_timer.stop();
    }

    m_notifications.notifyTaskEvent(
        tr("Task status changed"),
        QStringLiteral("%1 → %2").arg(desc, willBeDone ? tr("Done") : tr("To-Do"))
    );
    if (!remoteKey.isEmpty() && !effectiveListId.isEmpty()) {
        for (const TodoListEntry& listEntry : m_listManager.entries()) {
            if (listEntry.id == effectiveListId && listEntry.provider != QStringLiteral("SQLite") && listEntry.syncStatusToRemote) {
                m_issueTracker.updateIssueStatusRemote(listEntry, remoteKey, willBeDone, willBeDone ? QStringLiteral("done") : QStringLiteral("todo"));
                break;
            }
        }
    }
    if (willBeDone) {
        m_systemHooks.onTaskCompleted(desc, remoteKey, taskId);
    }
    return true;
}

bool AppController::changeTaskStatus(bool fromDoneList, int proxyRow, const QString& newStatus) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return false;
    return changeTaskStatusSource(fromDoneList, sourceRow, newStatus);
}

bool AppController::changeTaskStatusSource(bool fromDoneList, int sourceRow, const QString& newStatus) {
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    if (sourceRow < 0 || sourceRow >= store->rowCount()) return false;

    const QModelIndex idx = store->index(sourceRow, 0);
    const QString desc = store->data(idx, TaskStoreModel::DescriptionRole).toString();
    const QString oldStatus = store->data(idx, TaskStoreModel::StatusRole).toString();

    // Pre-fetch metadata and check for completion status case-insensitively BEFORE mutating model
    const QString remoteKey = store->data(idx, TaskStoreModel::JiraKeyRole).toString();
    const QString taskId = store->data(idx, TaskStoreModel::IdRole).toString();
    const QString listId = store->data(idx, TaskStoreModel::ListIdRole).toString();
    const QString effectiveListId = !listId.isEmpty() ? listId : m_currentListId;
    const QString normNewStatus = TxtTask::normalizeStatus(newStatus);
    const bool wasClosed = TxtTask::isClosedStatus(oldStatus);
    const bool willBeClosed = TxtTask::isClosedStatus(normNewStatus);
    const bool willMoveToDone = (!fromDoneList && !wasClosed && willBeClosed);

    const qint64 timedTaskId = m_timer.activeTaskId();
    const qint64 targetTaskId = taskId.toLongLong();
    const bool isTimingThisTask = willMoveToDone && m_timer.isRunning() &&
        (sourceRow == m_activeTodoRow || (timedTaskId >= 0 && targetTaskId == timedTaskId));

    if (isTimingThisTask) {
        m_timer.flushActiveTaskSeconds();
    }

    if (!m_taskManager.setTaskStatus(fromDoneList, sourceRow, normNewStatus)) {
        return false;
    }

    if (isTimingThisTask && m_timer.isRunning()) {
        m_timer.stop();
    }

    if (oldStatus != normNewStatus) {
        m_notifications.notifyTaskEvent(
            tr("Task status changed"),
            QStringLiteral("%1: %2 → %3").arg(desc, oldStatus, normNewStatus)
        );
        if (!remoteKey.isEmpty() && !effectiveListId.isEmpty() && wasClosed != willBeClosed) {
            for (const TodoListEntry& listEntry : m_listManager.entries()) {
                if (listEntry.id == effectiveListId && listEntry.provider != QStringLiteral("SQLite") && listEntry.syncStatusToRemote) {
                    m_issueTracker.updateIssueStatusRemote(listEntry, remoteKey, willBeClosed, normNewStatus);
                    break;
                }
            }
        }
        if (willBeClosed && !wasClosed) {
            m_systemHooks.onTaskCompleted(desc, remoteKey, taskId);
        }
    }
    return true;
}

bool AppController::changeTaskPriority(bool fromDoneList, int proxyRow, const QString& priorityLetter) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return false;

    quint8 prio = TxtTask::NoPriority;
    const QString p = priorityLetter.trimmed().toUpper();
    if (!p.isEmpty() && p != QStringLiteral("NONE") && p != QStringLiteral("CLEAR")) {
        const QChar ch = p.at(0);
        if (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z')) {
            prio = static_cast<quint8>(ch.unicode() - 'A');
        }
    }

    const QModelIndex idx = store->index(sourceRow, 0);
    return store->setData(idx, prio, TaskStoreModel::PriorityRole);
}

bool AppController::updateTaskText(bool fromDoneList, int proxyRow, const QString& simpleText) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return false;

    if (!store->updateTaskText(sourceRow, simpleText)) {
        return false;
    }
    if (!fromDoneList && sourceRow == m_activeTodoRow) {
        syncActiveTaskToTimer();
    }
    return true;
}

bool AppController::updateTaskDetails(bool fromDoneList, int proxyRow, const QString& details) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return false;

    const TxtTask task = store->taskAt(sourceRow);
    if (!store->updateTaskDetails(sourceRow, details)) {
        return false;
    }

    const QString targetListId = task.listId().isEmpty() ? m_currentListId : task.listId();
    const int listIdx = m_listManager.indexOfId(targetListId);
    if (listIdx >= 0 && !task.jiraKey().isEmpty()) {
        const TodoListEntry listEntry = m_listManager.entry(listIdx);
        if (listEntry.provider != QStringLiteral("SQLite") && listEntry.provider != QStringLiteral("Todo.txt")) {
            m_issueTracker.updateIssueDescription(listEntry, task.jiraKey(), details);
        }
    }
    return true;
}

bool AppController::addTaskComment(bool fromDoneList, int proxyRow, const QString& commentText) {
    if (commentText.trimmed().isEmpty()) return false;
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return false;

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
    if (!store->appendTaskComment(sourceRow, author, commentText)) {
        return false;
    }

    if (listIdx >= 0 && !task.jiraKey().isEmpty()) {
        const TodoListEntry listEntry = m_listManager.entry(listIdx);
        if (listEntry.provider != QStringLiteral("SQLite") && listEntry.provider != QStringLiteral("Todo.txt")) {
            m_issueTracker.postIssueComment(listEntry, task.jiraKey(), commentText);
        }
    }
    return true;
}

int AppController::unviewedTodoCount() const {
    return m_taskManager.todoModel()->unviewedTodoCount();
}

int AppController::totalTodoCount() const {
    return m_taskManager.todoModel()->totalTodoCount();
}

bool AppController::markTaskViewed(bool fromDoneList, int proxyRow) {
    if (fromDoneList) return false;
    const int sourceRow = m_todoProxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return false;
    return m_taskManager.todoModel()->markTaskViewed(sourceRow);
}

bool AppController::removeTask(bool fromDoneList, int proxyRow) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int sourceRow = proxy.mapToSourceRow(proxyRow);
    if (sourceRow < 0) return false;

    const qint64 timedTaskId = m_timer.activeTaskId();
    const qint64 removingTaskId = store->taskAt(sourceRow).id();
    const bool isTimingThisTask = !fromDoneList && m_timer.isRunning() &&
        (sourceRow == m_activeTodoRow || (timedTaskId >= 0 && removingTaskId == timedTaskId));

    const bool removingActive = (!fromDoneList && sourceRow == m_activeTodoRow);
    if (!store->removeAt(sourceRow)) {
        return false;
    }

    if (isTimingThisTask && m_timer.isRunning()) {
        m_timer.stop();
    }

    if (!fromDoneList) {
        if (removingActive) {
            const int count = m_taskManager.todoModel()->rowCount();
            m_activeTodoRow = count > 0 ? qBound(0, sourceRow, count - 1) : -1;
            emit activeTodoRowChanged();
            syncActiveTaskToTimer();
        } else if (m_activeTaskId >= 0) {
            const int found = findActiveTodoRowById(m_activeTaskId);
            if (found >= 0 && found != m_activeTodoRow) {
                m_activeTodoRow = found;
                emit activeTodoRowChanged();
            }
        }
    }
    return true;
}

void AppController::moveTaskRow(bool fromDoneList, int fromProxyRow, int toProxyRow) {
    TaskFilterProxyModel& proxy = fromDoneList ? m_doneProxy : m_todoProxy;
    TaskStoreModel* store = fromDoneList ? m_taskManager.doneModel() : m_taskManager.todoModel();
    const int fromSource = proxy.mapToSourceRow(fromProxyRow);
    const int toSource = proxy.mapToSourceRow(toProxyRow);
    if (fromSource < 0 || toSource < 0 || fromSource == toSource) return;

    store->moveItem(fromSource, toSource);
    if (!fromDoneList) {
        if (m_activeTaskId >= 0) {
            const int found = findActiveTodoRowById(m_activeTaskId);
            if (found >= 0 && found != m_activeTodoRow) {
                m_activeTodoRow = found;
                emit activeTodoRowChanged();
            }
        } else if (m_activeTodoRow == fromSource) {
            m_activeTodoRow = toSource;
            emit activeTodoRowChanged();
        }
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
    toggleTaskDoneSource(false, m_activeTodoRow);

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
    QString taskDescription = m_timer.activeTaskDescription();

    const qint64 timedTaskId = m_timer.activeTaskId();
    TxtTask taskInfo;
    bool foundTask = false;

    if (timedTaskId >= 0) {
        activeTaskId = timedTaskId;
        const int row = findActiveTodoRowById(timedTaskId);
        if (row >= 0 && row < m_taskManager.todoModel()->rowCount()) {
            taskInfo = m_taskManager.todoModel()->taskAt(row);
            foundTask = true;
        } else {
            for (int r = 0; r < m_taskManager.doneModel()->rowCount(); ++r) {
                if (m_taskManager.doneModel()->taskAt(r).id() == timedTaskId) {
                    taskInfo = m_taskManager.doneModel()->taskAt(r);
                    foundTask = true;
                    break;
                }
            }
            if (!foundTask) {
                const auto dbTask = DatabaseManager::instance().loadTask(timedTaskId);
                if (dbTask.has_value()) {
                    taskInfo = *dbTask;
                    foundTask = true;
                }
            }
        }
    } else if (m_activeTodoRow >= 0 && m_activeTodoRow < m_taskManager.todoModel()->rowCount()) {
        taskInfo = m_taskManager.todoModel()->taskAt(m_activeTodoRow);
        foundTask = true;
    }

    if (foundTask) {
        activeTaskId = taskInfo.id();
        jiraKey = taskInfo.jiraKey();
        if (!taskInfo.description().isEmpty()) {
            taskDescription = taskInfo.description();
        }
        if (!taskInfo.listId().isEmpty()) {
            targetListId = taskInfo.listId();
            targetListName = taskInfo.listName().isEmpty() ? m_currentListName : taskInfo.listName();
        }
    }

    if (targetListName.isEmpty() || targetListName == QStringLiteral("__ALL__")) {
        const int idx = m_listManager.indexOfId(targetListId);
        if (idx >= 0) {
            targetListName = m_listManager.entry(idx).name;
        } else if (!m_currentListName.isEmpty() && m_currentListName != QStringLiteral("__ALL__")) {
            targetListName = m_currentListName;
        }
    }

    const bool completedPomodoro = m_lastSessionCompletedPomodoro;
    m_lastSessionCompletedPomodoro = false;

    const qint64 logId = m_activityLog.logTask(
        targetListId,
        targetListName,
        activeTaskId,
        jiraKey,
        taskDescription,
        startTimeUtc,
        runtimeSeconds,
        stopUtc,
        completedPomodoro
    );

    if (logId < 0) {
        const QString errMsg = tr("Failed to save activity log record to database for '%1'.").arg(taskDescription);
        qCritical() << errMsg;
        m_notifications.notifyTaskEvent(tr("Database Error"), errMsg);
        emit databaseError(errMsg);
    }

    const int listIdx = m_listManager.indexOfId(targetListId);
    QString logPath;
    if (listIdx >= 0) {
        logPath = m_listManager.entry(listIdx).activityLogPath;
    }
    if (logPath.isEmpty() && m_currentListId == targetListId) {
        logPath = m_currentListActivityLogPath;
    }

    if (!logPath.isEmpty()) {
        ActivityLog::logTaskInFile(
            logPath,
            targetListName,
            taskDescription,
            startTimeUtc,
            runtimeSeconds,
            stopUtc
        );
    }

    if (listIdx >= 0 && !jiraKey.isEmpty() && logId >= 0) {
        const TodoListEntry listEntry = m_listManager.entry(listIdx);
        if (listEntry.provider != QStringLiteral("SQLite") && listEntry.provider != QStringLiteral("Todo.txt")
            && listEntry.autoWorklog) {
            m_issueTracker.postWorklog(listEntry, activeTaskId, logId, jiraKey, startTimeUtc, runtimeSeconds);
        }
    }
}

void AppController::onActiveTaskTimeUpdated(uint totalSeconds) {
    const qint64 timedTaskId = m_timer.activeTaskId();
    bool ok = true;
    bool reportedByStore = false;
    if (timedTaskId >= 0) {
        const int row = findActiveTodoRowById(timedTaskId);
        if (row >= 0 && row < m_taskManager.todoModel()->rowCount()) {
            reportedByStore = true;
            ok = m_taskManager.todoModel()->setTaskTimerValue(row, totalSeconds);
        } else {
            bool foundInDone = false;
            for (int r = 0; r < m_taskManager.doneModel()->rowCount(); ++r) {
                if (m_taskManager.doneModel()->taskAt(r).id() == timedTaskId) {
                    reportedByStore = true;
                    ok = m_taskManager.doneModel()->setTaskTimerValue(r, totalSeconds);
                    foundInDone = true;
                    break;
                }
            }
            if (!foundInDone) {
                bool queryError = false;
                const auto existingTask = DatabaseManager::instance().loadTask(timedTaskId, &queryError);
                if (queryError) {
                    ok = false;
                } else if (existingTask.has_value()) {
                    ok = DatabaseManager::instance().updateTaskTimer(timedTaskId, totalSeconds);
                }
            }
        }
    } else if (m_activeTodoRow >= 0 && m_activeTodoRow < m_taskManager.todoModel()->rowCount()) {
        reportedByStore = true;
        ok = m_taskManager.todoModel()->setTaskTimerValue(m_activeTodoRow, totalSeconds);
    }

    if (!ok) {
        const QString err = tr("Failed to save task timer progress to database.");
        qWarning() << "AppController::onActiveTaskTimeUpdated database error:" << err;
        if (!reportedByStore) {
            m_notifications.notifyTaskEvent(tr("Database Error"), err);
            emit databaseError(err);
        }
    }
}

void AppController::onTodoListCountChanged() {
    const int count = m_taskManager.todoModel()->rowCount();
    if (count == 0) {
        if (m_timer.isRunning()) {
            m_timer.stop();
        }
        m_activeTodoRow = -1;
        m_activeTaskId = -1;
        emit activeTodoRowChanged();
        m_timer.clearActiveTask();
    } else {
        if (m_activeTaskId >= 0) {
            const int found = findActiveTodoRowById(m_activeTaskId);
            if (found >= 0) {
                if (m_activeTodoRow != found) {
                    m_activeTodoRow = found;
                    emit activeTodoRowChanged();
                }
            } else {
                if (m_timer.isRunning()) {
                    m_timer.stop();
                }
                m_activeTodoRow = qMin(qMax(0, m_activeTodoRow), count - 1);
                emit activeTodoRowChanged();
                syncActiveTaskToTimer();
            }
        } else if (m_activeTodoRow < 0 || m_activeTodoRow >= count) {
            if (m_timer.isRunning()) {
                m_timer.stop();
            }
            m_activeTodoRow = qMin(qMax(0, m_activeTodoRow), count - 1);
            emit activeTodoRowChanged();
            syncActiveTaskToTimer();
        } else {
            syncActiveTaskToTimer();
        }
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

void AppController::reloadTranslations() {
    m_ukTranslator.reloadTranslations();
    m_settings.setSupportedLanguages(m_ukTranslator.availableLanguageCodes());
    applyLanguageTranslator();
    emit languageChanged();
}

QVariantList AppController::availableLanguages() const {
    return m_ukTranslator.availableLanguagesModel();
}

QStringList AppController::availableLanguageCodes() const {
    return m_ukTranslator.availableLanguageCodes();
}

QString AppController::languageDisplayName(const QString& code) const {
    return m_ukTranslator.languageDisplayName(code);
}

void AppController::updateAutoSyncTimer() {
    const int mins = m_settings.autoSyncIntervalMinutes();
    if (mins > 0) {
        m_autoSyncTimer.start(mins * 60 * 1000);
    } else {
        m_autoSyncTimer.stop();
    }
}

int AppController::findActiveTodoRowById(qint64 taskId) const {
    if (taskId < 0) return -1;
    const QVector<TxtTask>& tasks = m_taskManager.todoModel()->tasks();
    for (int i = 0; i < tasks.size(); ++i) {
        if (tasks[i].id() == taskId) {
            return i;
        }
    }
    return -1;
}

void AppController::syncActiveTaskToTimer() {
    if (m_activeTodoRow < 0 || m_activeTodoRow >= m_taskManager.todoModel()->rowCount()) {
        m_activeTaskId = -1;
        m_activeTaskJiraKey.clear();
        m_activeTaskListId.clear();
        m_timer.clearActiveTask();
        return;
    }
    const TxtTask task = m_taskManager.todoModel()->taskAt(m_activeTodoRow);
    m_activeTaskId = task.id();
    m_activeTaskJiraKey = task.jiraKey();
    m_activeTaskListId = task.listId();
    m_timer.setActiveTask(task.id(), task.description(), task.timerValue(), task.duration());
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
