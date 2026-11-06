#pragma once

#include "Schedule.h"
#include "../todotxt/TxtTask.h"
#include <QDateTime>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>
#include <QVector>

namespace gofi {

struct TodoListEntry {
    QString id;
    QString name;
    QString provider = QStringLiteral("SQLite");
    QString directoryPath;
    QString todoFilePath;
    QString doneFilePath;
    QString activityLogPath;
    bool logTimerInTxt = true;
    bool addCreationDates = true;
    int reminderTime = -1;
    Schedule customSchedule;
    QString jiraUrl;
    QString jiraUser;
    QString jiraJql;
    QString apiToken;
    bool autoWorklog = true;
    QVariantMap statusMapping; // remoteStatusName -> "todo" | "in_progress" | "paused" | "done" | "canceled"
};

class DatabaseManager {
public:
    static DatabaseManager& instance();

    bool initialize(const QString& customDbPath = {});
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] QString databasePath() const { return m_dbPath; }
    bool relocateDatabase(const QString& newDbPath, bool copyExisting = true);
    bool exportDatabaseBackup(const QString& destinationFilePath) const;
    bool importDatabaseBackup(const QString& sourceFilePath);

    // Key-Value App Settings inside SQLite
    [[nodiscard]] QVariant getSetting(const QString& key, const QVariant& defaultValue = {}) const;
    bool setSetting(const QString& key, const QVariant& value);

    // Lists CRUD
    [[nodiscard]] QVector<TodoListEntry> loadLists() const;
    bool saveList(const TodoListEntry& entry, int sortOrder);
    bool removeList(const QString& listId);
    bool updateListsOrder(const QVector<TodoListEntry>& entries);

    // Tasks CRUD
    [[nodiscard]] QVector<TxtTask> loadTasks(const QString& listId, bool done) const;
    [[nodiscard]] int countAllTasksInList(const QString& listId) const;
    qint64 insertTask(const QString& listId, const TxtTask& task, int sortOrder);
    bool updateTask(const TxtTask& task);
    bool updateTaskDoneAndOrder(const TxtTask& task, int sortOrder);
    bool updateTaskTimer(qint64 taskId, quint32 timerSeconds, quint32 jiraSyncedSeconds = 0);
    bool deleteTask(qint64 taskId);
    bool deleteDoneTasks(const QString& listId);
    bool replaceListTasksOrders(const QString& listId, bool done, const QVector<TxtTask>& tasks);
    bool upsertRemoteIssueTask(const QString& listId,
                               const QString& issueKey,
                               const QString& description,
                               const QString& localStatus,
                               quint8 priority,
                               quint32 durationSeconds,
                               const QString& details = {},
                               const QVariantList& comments = {});

    // Activity Log
    qint64 insertActivityLog(const QString& listId,
                             const QString& listName,
                             qint64 taskId,
                             const QString& jiraKey,
                             const QString& taskDescription,
                             const QDateTime& startTimeUtc,
                             quint32 runtimeSeconds,
                             const QDateTime& stopTimeUtc,
                             bool worklogSynced = false);
    bool markActivityWorklogSynced(qint64 logId);

private:
    DatabaseManager() = default;
    ~DatabaseManager();

    bool createSchema();
    [[nodiscard]] QSqlDatabase db() const;

    QString m_connectionName = QStringLiteral("gofi_sqlite_conn");
    QString m_dbPath;
};

} // namespace gofi
