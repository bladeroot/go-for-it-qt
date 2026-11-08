#include "TaskManager.h"
#include "../core/DatabaseManager.h"

namespace gofi {

TaskManager::TaskManager(QObject* parent)
    : QObject(parent)
    , m_todoStore(false, this)
    , m_doneStore(true, this)
{
    connect(&m_todoStore, &TaskStoreModel::taskDoneToggled, this, &TaskManager::onTodoTaskDoneToggled);
    connect(&m_doneStore, &TaskStoreModel::taskDoneToggled, this, &TaskManager::onDoneTaskDoneToggled);

    connect(&m_todoStore, &TaskStoreModel::taskUpdated, this, &TaskManager::onTaskUpdated);
    connect(&m_doneStore, &TaskStoreModel::taskUpdated, this, &TaskManager::onTaskUpdated);

    connect(&m_todoStore, &TaskStoreModel::taskRemoved, this, &TaskManager::onTaskRemoved);
    connect(&m_doneStore, &TaskStoreModel::taskRemoved, this, &TaskManager::onTaskRemoved);

    connect(&m_todoStore, &TaskStoreModel::tasksReordered, this, &TaskManager::onTodoTasksReordered);
    connect(&m_doneStore, &TaskStoreModel::tasksReordered, this, &TaskManager::onDoneTasksReordered);

    connect(&m_todoStore, &TaskStoreModel::taskBecameInvalid, &m_todoStore, &TaskStoreModel::removeAt);
    connect(&m_doneStore, &TaskStoreModel::taskBecameInvalid, &m_doneStore, &TaskStoreModel::removeAt);
}

void TaskManager::setActiveList(const QString& listId,
                                bool addDefaultTodosIfNew,
                                bool addCreationDates)
{
    m_listId = listId;
    m_listIds = QStringList{listId};
    m_addCreationDates = addCreationDates;
    loadFromDatabase(addDefaultTodosIfNew);
}

void TaskManager::setActiveLists(const QStringList& listIds,
                                 const QString& primaryListId,
                                 bool addCreationDates)
{
    m_listIds = listIds;
    m_listId = primaryListId.isEmpty() ? (listIds.isEmpty() ? QString() : listIds.first()) : primaryListId;
    m_addCreationDates = addCreationDates;
    loadFromDatabase(false);
}

void TaskManager::addNewTask(const QString& simpleText, bool onTop, bool addCreationDate) {
    const QString trimmed = simpleText.trimmed();
    if (trimmed.isEmpty() || m_listId.isEmpty()) {
        return;
    }
    TxtTask task = TxtTask::fromSimpleTxt(trimmed, false, addCreationDate && m_addCreationDates);
    if (!task.isValid()) {
        return;
    }

    const int order = onTop ? 0 : m_todoStore.rowCount();
    const qint64 newId = DatabaseManager::instance().insertTask(m_listId, task, order);
    if (newId < 0) {
        emit errorOccurred(tr("Failed to save new task to SQLite database."));
        return;
    }
    task.setId(newId);

    if (onTop) {
        m_todoStore.prependTask(task);
        updateTasksOrders(false, m_todoStore.tasks());
    } else {
        m_todoStore.appendTask(task);
    }
}

bool TaskManager::toggleTaskDone(bool fromDoneList, int sourceRow) {
    TaskStoreModel* sourceStore = fromDoneList ? &m_doneStore : &m_todoStore;
    TaskStoreModel* destStore = fromDoneList ? &m_todoStore : &m_doneStore;
    if (sourceRow < 0 || sourceRow >= sourceStore->rowCount()) {
        return false;
    }

    TxtTask task = sourceStore->taskAt(sourceRow);
    const bool willBeDone = !fromDoneList;
    task.setDone(willBeDone);
    const QString targetListId = !task.listId().isEmpty() ? task.listId() : m_listId;
    int newOrder = 0;
    for (int i = 0; i < destStore->rowCount(); ++i) {
        const TxtTask t = destStore->taskAt(i);
        const QString lid = !t.listId().isEmpty() ? t.listId() : m_listId;
        if (lid == targetListId) {
            newOrder++;
        }
    }

    QMap<QString, QVector<TxtTask>> remainingPerList;
    for (int i = 0; i < sourceStore->rowCount(); ++i) {
        if (i == sourceRow) continue;
        const TxtTask t = sourceStore->taskAt(i);
        const QString lid = !t.listId().isEmpty() ? t.listId() : m_listId;
        if (!lid.isEmpty()) {
            remainingPerList[lid].append(t);
        }
    }

    if (!DatabaseManager::instance().moveTaskDoneAndReorder(task, newOrder, remainingPerList, fromDoneList)) {
        emit errorOccurred(tr("Failed to update task status in database."));
        return false;
    }

    m_movingBetweenStores = true;
    sourceStore->removeAt(sourceRow);
    destStore->appendTask(task);
    m_movingBetweenStores = false;
    return true;
}

bool TaskManager::setTaskStatus(bool fromDoneList, int sourceRow, const QString& newStatus) {
    TaskStoreModel* store = fromDoneList ? &m_doneStore : &m_todoStore;
    if (sourceRow < 0 || sourceRow >= store->rowCount()) {
        return false;
    }

    TxtTask task = store->taskAt(sourceRow);
    const QString oldStatus = task.status();
    const QString normNewStatus = TxtTask::normalizeStatus(newStatus);
    const bool wasClosed = TxtTask::isClosedStatus(oldStatus);
    const bool willBeClosed = TxtTask::isClosedStatus(normNewStatus);

    if (wasClosed != willBeClosed) {
        task.setStatus(normNewStatus);
        TaskStoreModel* destStore = willBeClosed ? &m_doneStore : &m_todoStore;
        const QString targetListId = !task.listId().isEmpty() ? task.listId() : m_listId;
        int newOrder = 0;
        for (int i = 0; i < destStore->rowCount(); ++i) {
            const TxtTask t = destStore->taskAt(i);
            const QString lid = !t.listId().isEmpty() ? t.listId() : m_listId;
            if (lid == targetListId) {
                newOrder++;
            }
        }

        QMap<QString, QVector<TxtTask>> remainingPerList;
        for (int i = 0; i < store->rowCount(); ++i) {
            if (i == sourceRow) continue;
            const TxtTask t = store->taskAt(i);
            const QString lid = !t.listId().isEmpty() ? t.listId() : m_listId;
            if (!lid.isEmpty()) {
                remainingPerList[lid].append(t);
            }
        }

        if (!DatabaseManager::instance().moveTaskDoneAndReorder(task, newOrder, remainingPerList, wasClosed)) {
            emit errorOccurred(tr("Failed to update task status in database."));
            return false;
        }

        m_movingBetweenStores = true;
        store->removeAt(sourceRow);
        destStore->appendTask(task);
        m_movingBetweenStores = false;
        return true;
    } else {
        task.setStatus(normNewStatus);
        if (!DatabaseManager::instance().updateTask(task)) {
            emit errorOccurred(tr("Failed to update task in database."));
            return false;
        }
        m_suppressStoreTaskUpdated = true;
        store->setTaskStatus(sourceRow, normNewStatus);
        m_suppressStoreTaskUpdated = false;
        return true;
    }
}

void TaskManager::clearDoneList() {
    const QStringList targetLists = !m_listIds.isEmpty() ? m_listIds : (!m_listId.isEmpty() ? QStringList{m_listId} : QStringList{QStringLiteral("__ALL__")});
    if (!DatabaseManager::instance().deleteDoneTasks(targetLists)) {
        emit errorOccurred(tr("Failed to delete completed tasks from database."));
        loadFromDatabase(false);
        return;
    }
    m_movingBetweenStores = true;
    m_doneStore.clear();
    m_movingBetweenStores = false;
}

void TaskManager::sortTasks() {
    m_todoStore.sortTasks();
    m_doneStore.sortTasks();
}

void TaskManager::refresh() {
    loadFromDatabase(false);
    emit refreshed();
}

void TaskManager::onTodoTaskDoneToggled(int row, const TxtTask& task, bool* success) {
    if (m_movingBetweenStores) {
        if (success) *success = false;
        return;
    }
    const QString targetListId = !task.listId().isEmpty() ? task.listId() : m_listId;
    int newOrder = 0;
    for (int i = 0; i < m_doneStore.rowCount(); ++i) {
        const TxtTask t = m_doneStore.taskAt(i);
        const QString lid = !t.listId().isEmpty() ? t.listId() : m_listId;
        if (lid == targetListId) {
            newOrder++;
        }
    }

    QMap<QString, QVector<TxtTask>> remainingPerList;
    for (int i = 0; i < m_todoStore.rowCount(); ++i) {
        if (i == row) continue;
        const TxtTask t = m_todoStore.taskAt(i);
        const QString lid = !t.listId().isEmpty() ? t.listId() : m_listId;
        if (!lid.isEmpty()) {
            remainingPerList[lid].append(t);
        }
    }

    if (!DatabaseManager::instance().moveTaskDoneAndReorder(task, newOrder, remainingPerList, false)) {
        if (success) *success = false;
        emit errorOccurred(tr("Failed to update task status in database."));
        loadFromDatabase(false);
        return;
    }
    if (success) *success = true;
    m_movingBetweenStores = true;
    m_todoStore.removeAt(row);
    m_doneStore.appendTask(task);
    m_movingBetweenStores = false;
}

void TaskManager::onDoneTaskDoneToggled(int row, const TxtTask& task, bool* success) {
    if (m_movingBetweenStores) {
        if (success) *success = false;
        return;
    }
    const QString targetListId = !task.listId().isEmpty() ? task.listId() : m_listId;
    int newOrder = 0;
    for (int i = 0; i < m_todoStore.rowCount(); ++i) {
        const TxtTask t = m_todoStore.taskAt(i);
        const QString lid = !t.listId().isEmpty() ? t.listId() : m_listId;
        if (lid == targetListId) {
            newOrder++;
        }
    }

    QMap<QString, QVector<TxtTask>> remainingPerList;
    for (int i = 0; i < m_doneStore.rowCount(); ++i) {
        if (i == row) continue;
        const TxtTask t = m_doneStore.taskAt(i);
        const QString lid = !t.listId().isEmpty() ? t.listId() : m_listId;
        if (!lid.isEmpty()) {
            remainingPerList[lid].append(t);
        }
    }

    if (!DatabaseManager::instance().moveTaskDoneAndReorder(task, newOrder, remainingPerList, true)) {
        if (success) *success = false;
        emit errorOccurred(tr("Failed to update task status in database."));
        loadFromDatabase(false);
        return;
    }
    if (success) *success = true;
    m_movingBetweenStores = true;
    m_doneStore.removeAt(row);
    m_todoStore.appendTask(task);
    m_movingBetweenStores = false;
}

void TaskManager::onTaskUpdated(int row, const TxtTask& task, bool* success) {
    Q_UNUSED(row)
    if (m_suppressStoreTaskUpdated) {
        if (success) *success = true;
        return;
    }
    if (!DatabaseManager::instance().updateTask(task)) {
        if (success) *success = false;
        emit errorOccurred(tr("Failed to update task in database."));
        loadFromDatabase(false);
        return;
    }
    if (success) *success = true;
}

void TaskManager::onTaskRemoved(qint64 taskId) {
    if (!m_movingBetweenStores && taskId >= 0) {
        if (!DatabaseManager::instance().deleteTask(taskId)) {
            emit errorOccurred(tr("Failed to delete task from database."));
            loadFromDatabase(false);
        }
    }
}

void TaskManager::onTodoTasksReordered() {
    updateTasksOrders(false, m_todoStore.tasks());
}

void TaskManager::onDoneTasksReordered() {
    updateTasksOrders(true, m_doneStore.tasks());
}

bool TaskManager::updateTasksOrders(bool done, const QVector<TxtTask>& tasks) {
    if (tasks.isEmpty()) return true;
    QMap<QString, QVector<TxtTask>> tasksByList;
    for (const TxtTask& t : tasks) {
        const QString lid = !t.listId().isEmpty() ? t.listId() : m_listId;
        if (!lid.isEmpty()) {
            tasksByList[lid].append(t);
        }
    }
    bool allSucceeded = true;
    for (auto it = tasksByList.cbegin(); it != tasksByList.cend(); ++it) {
        if (!DatabaseManager::instance().replaceListTasksOrders(it.key(), done, it.value())) {
            allSucceeded = false;
        }
    }
    if (!allSucceeded) {
        emit errorOccurred(tr("Failed to update tasks order in database."));
        loadFromDatabase(false);
        return false;
    }
    return true;
}

void TaskManager::loadFromDatabase(bool addDefaultsIfEmpty) {
    if (m_listIds.isEmpty() && m_listId.isEmpty()) {
        m_todoStore.clear();
        m_doneStore.clear();
        return;
    }

    DatabaseManager& db = DatabaseManager::instance();
    if (m_listIds.size() == 1 && !m_listId.isEmpty() && addDefaultsIfEmpty && db.countAllTasksInList(m_listId) == 0) {
        const QStringList defaults = {
            QStringLiteral("Choose a task from the list or add a new one @GoForIt"),
            QStringLiteral("Switch to the Timer tab and start working +Productivity duration:25m"),
            QStringLiteral("(A) Mark completed tasks as done @GoForIt")
        };
        if (!db.beginTransaction()) {
            emit errorOccurred(tr("Failed to start database transaction for initial tasks."));
        } else {
            bool allOk = true;
            for (int i = 0; i < defaults.size(); ++i) {
                TxtTask t = TxtTask::fromSimpleTxt(defaults.at(i), false, m_addCreationDates);
                if (db.insertTask(m_listId, t, i) < 0) {
                    allOk = false;
                    break;
                }
            }
            if (allOk) {
                if (!db.commitTransaction()) {
                    db.rollbackTransaction();
                    emit errorOccurred(tr("Failed to commit initial tasks transaction."));
                }
            } else {
                db.rollbackTransaction();
                emit errorOccurred(tr("Failed to save initial tasks to database."));
            }
        }
    }

    const QStringList queryIds = m_listIds.isEmpty() ? QStringList{m_listId} : m_listIds;
    m_todoStore.setTasks(db.loadTasks(queryIds, false));
    m_doneStore.setTasks(db.loadTasks(queryIds, true));
}

} // namespace gofi
