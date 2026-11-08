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

void TaskManager::clearDoneList() {
    if (!m_listIds.isEmpty()) {
        for (const QString& lid : m_listIds) {
            DatabaseManager::instance().deleteDoneTasks(lid);
        }
    } else if (!m_listId.isEmpty()) {
        DatabaseManager::instance().deleteDoneTasks(m_listId);
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

void TaskManager::onTodoTaskDoneToggled(int row, const TxtTask& task) {
    m_movingBetweenStores = true;
    m_todoStore.removeAt(row);
    m_movingBetweenStores = false;

    const int newOrder = m_doneStore.rowCount();
    DatabaseManager::instance().updateTaskDoneAndOrder(task, newOrder);
    updateTasksOrders(false, m_todoStore.tasks());
    m_doneStore.appendTask(task);
}

void TaskManager::onDoneTaskDoneToggled(int row, const TxtTask& task) {
    m_movingBetweenStores = true;
    m_doneStore.removeAt(row);
    m_movingBetweenStores = false;

    const int newOrder = m_todoStore.rowCount();
    DatabaseManager::instance().updateTaskDoneAndOrder(task, newOrder);
    updateTasksOrders(true, m_doneStore.tasks());
    m_todoStore.appendTask(task);
}

void TaskManager::onTaskUpdated(int row, const TxtTask& task) {
    Q_UNUSED(row)
    DatabaseManager::instance().updateTask(task);
}

void TaskManager::onTaskRemoved(qint64 taskId) {
    if (!m_movingBetweenStores && taskId >= 0) {
        DatabaseManager::instance().deleteTask(taskId);
    }
}

void TaskManager::onTodoTasksReordered() {
    updateTasksOrders(false, m_todoStore.tasks());
}

void TaskManager::onDoneTasksReordered() {
    updateTasksOrders(true, m_doneStore.tasks());
}

void TaskManager::updateTasksOrders(bool done, const QVector<TxtTask>& tasks) {
    if (tasks.isEmpty()) return;
    QMap<QString, QVector<TxtTask>> tasksByList;
    for (const TxtTask& t : tasks) {
        const QString lid = !t.listId().isEmpty() ? t.listId() : m_listId;
        if (!lid.isEmpty()) {
            tasksByList[lid].append(t);
        }
    }
    for (auto it = tasksByList.cbegin(); it != tasksByList.cend(); ++it) {
        DatabaseManager::instance().replaceListTasksOrders(it.key(), done, it.value());
    }
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
        for (int i = 0; i < defaults.size(); ++i) {
            TxtTask t = TxtTask::fromSimpleTxt(defaults.at(i), false, m_addCreationDates);
            db.insertTask(m_listId, t, i);
        }
    }

    const QStringList queryIds = m_listIds.isEmpty() ? QStringList{m_listId} : m_listIds;
    m_todoStore.setTasks(db.loadTasks(queryIds, false));
    m_doneStore.setTasks(db.loadTasks(queryIds, true));
}

} // namespace gofi
