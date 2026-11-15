#pragma once

#include <QDateTime>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

namespace gofi {

class DatabaseManager;

struct ActivityRecord {
    qint64 id = -1;
    QString listId;
    QString listName;
    qint64 taskId = -1;
    QString jiraKey;
    QString taskDescription;
    QDateTime startTimeUtc;
    quint32 runtimeSeconds = 0;
    QDateTime stopTimeUtc;
    bool worklogSynced = false;
    bool isPomodoroCompleted = false;
};

class ActivityRepository {
public:
    explicit ActivityRepository(DatabaseManager* db = nullptr);

    // Activity Log CRUD
    qint64 insertActivityLog(const QString& listId,
                             const QString& listName,
                             qint64 taskId,
                             const QString& jiraKey,
                             const QString& taskDescription,
                             const QDateTime& startTimeUtc,
                             quint32 runtimeSeconds,
                             const QDateTime& stopTimeUtc,
                             bool worklogSynced = false,
                             bool isPomodoroCompleted = false);
    bool markActivityWorklogSynced(qint64 logId);
    [[nodiscard]] QVector<ActivityRecord> loadActivityLogs(const QDateTime& startUtc = {},
                                                           const QDateTime& endUtc = {},
                                                           const QString& listId = {}) const;

private:
    [[nodiscard]] QSqlDatabase db() const;

    DatabaseManager* m_dbManager = nullptr;
};

} // namespace gofi
