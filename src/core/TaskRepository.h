#pragma once

#include "../todotxt/TxtTask.h"
#include <QMap>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>
#include <optional>

namespace gofi {

class DatabaseManager;

class TaskRepository {
public:
    explicit TaskRepository(DatabaseManager* db = nullptr);

    // Tasks CRUD
    [[nodiscard]] std::optional<TxtTask> loadTask(qint64 taskId, bool* queryError = nullptr) const;
    [[nodiscard]] QVector<TxtTask> loadTasks(const QStringList& listIds, bool done) const;
    [[nodiscard]] QVector<TxtTask> loadTasks(const QString& listId, bool done) const;
    [[nodiscard]] int countAllTasksInLists(const QStringList& listIds) const;
    [[nodiscard]] int countAllTasksInList(const QString& listId) const;
    [[nodiscard]] bool isOnboardingCompleted(const QString& listId) const;
    bool setOnboardingCompleted(const QString& listId, bool completed = true);
    qint64 insertTask(const QString& listId, const TxtTask& task, int sortOrder);
    qint64 insertTaskOnTopAndReorder(const QString& listId, const TxtTask& task, const QVector<TxtTask>& existingTasks);
    bool updateTask(const TxtTask& task);
    bool markTaskViewed(qint64 taskId, bool viewed = true);
    bool updateTaskDoneAndOrder(const TxtTask& task, int sortOrder);
    bool moveTaskDoneAndReorder(const TxtTask& task, int newOrder, const QMap<QString, QVector<TxtTask>>& remainingPerList, bool remainingDone);
    bool updateTaskTimer(qint64 taskId, quint32 timerSeconds, quint32 jiraSyncedSeconds = 0);
    bool deleteTask(qint64 taskId);
    bool deleteTaskAndReorder(qint64 taskId, const QString& listId, bool done, const QVector<TxtTask>& remainingTasks);
    bool deleteDoneTasks(const QString& listId);
    bool deleteDoneTasks(const QStringList& listIds);
    bool replaceListTasksOrders(const QString& listId, bool done, const QVector<TxtTask>& tasks);
    bool upsertRemoteIssueTask(const QString& listId,
                               const QString& issueKey,
                               const QString& description,
                               const QString& localStatus,
                               quint8 priority,
                               quint32 durationSeconds,
                               const QString& details = {},
                               const QVariantList& comments = {},
                               bool* outIsNew = nullptr,
                               QString* outOldStatus = nullptr,
                               // true: `details` / `comments` are the tracker's complete current value, so an
                               // empty one clears the stored value; false: empty means "not fetched", keep it.
                               bool replaceDetails = false,
                               bool replaceComments = false);

private:
    [[nodiscard]] QSqlDatabase db() const;

    DatabaseManager* m_dbManager = nullptr;
};

} // namespace gofi
