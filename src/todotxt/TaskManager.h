#pragma once

#include "TaskStoreModel.h"
#include <QObject>

namespace gofi {

class TaskManager : public QObject {
    Q_OBJECT

public:
    explicit TaskManager(QObject* parent = nullptr);

    [[nodiscard]] TaskStoreModel* todoModel() noexcept { return &m_todoStore; }
    [[nodiscard]] const TaskStoreModel* todoModel() const noexcept { return &m_todoStore; }
    [[nodiscard]] TaskStoreModel* doneModel() noexcept { return &m_doneStore; }
    [[nodiscard]] const TaskStoreModel* doneModel() const noexcept { return &m_doneStore; }

    void setActiveList(const QString& listId,
                       bool addDefaultTodosIfNew = false,
                       bool addCreationDates = true);

    void setActiveLists(const QStringList& listIds,
                        const QString& primaryListId = {},
                        bool addCreationDates = true);

    void setAddCreationDates(bool value) noexcept { m_addCreationDates = value; }
    [[nodiscard]] bool addCreationDates() const noexcept { return m_addCreationDates; }
    [[nodiscard]] QString activeListId() const { return m_listId; }
    [[nodiscard]] QStringList activeListIds() const { return m_listIds; }
    [[nodiscard]] bool isMultiList() const { return m_listIds.size() != 1 || m_listIds.contains(QStringLiteral("__ALL__")); }

    void addNewTask(const QString& simpleText, bool onTop = false, bool addCreationDate = true);
    bool toggleTaskDone(bool fromDoneList, int sourceRow);
    bool setTaskStatus(bool fromDoneList, int sourceRow, const QString& newStatus);
    void clearDoneList();
    void sortTasks();
    void refresh();

signals:
    void errorOccurred(const QString& message);
    void refreshed();

private slots:
    void onTodoTasksReordered();
    void onDoneTasksReordered();

private:
    bool toggleTodoTaskDone(int row, const gofi::TxtTask& task);
    bool toggleDoneTaskDone(int row, const gofi::TxtTask& task);
    bool updateTaskInDatabase(int row, const gofi::TxtTask& task);
    bool updateTaskTimerInDatabase(int row, const gofi::TxtTask& task);
    bool deleteTaskFromDatabase(int row, qint64 taskId);
    void loadFromDatabase(bool addDefaultsIfEmpty);
    bool updateTasksOrders(bool done, const QVector<TxtTask>& tasks);

    TaskStoreModel m_todoStore;
    TaskStoreModel m_doneStore;
    QString m_listId;
    QStringList m_listIds;
    bool m_addCreationDates = true;
    bool m_movingBetweenStores = false;
    bool m_suppressStoreTaskUpdated = false;
};

} // namespace gofi
