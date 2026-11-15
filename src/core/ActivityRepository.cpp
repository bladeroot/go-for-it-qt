#include "ActivityRepository.h"
#include "DatabaseManager.h"
#include "DatabaseSerialization.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTimeZone>

namespace gofi {

ActivityRepository::ActivityRepository(DatabaseManager* db)
    : m_dbManager(db)
{
}

QSqlDatabase ActivityRepository::db() const {
    if (m_dbManager) {
        return m_dbManager->db();
    }
    return DatabaseManager::instance().db();
}

qint64 ActivityRepository::insertActivityLog(const QString& listId,
                                            const QString& listName,
                                            qint64 taskId,
                                            const QString& jiraKey,
                                            const QString& taskDescription,
                                            const QDateTime& startTimeUtc,
                                            quint32 runtimeSeconds,
                                            const QDateTime& stopTimeUtc,
                                            bool worklogSynced,
                                            bool isPomodoroCompleted)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO activity_log (list_id, list_name, task_id, jira_key, task_description, "
        "start_time_utc, runtime_seconds, stop_time_utc, worklog_synced, is_pomodoro_completed) "
        "VALUES (:list_id, :list_name, :task_id, :jira_key, :task_description, "
        ":start_time_utc, :runtime_seconds, :stop_time_utc, :worklog_synced, :is_pomodoro_completed)"
    ));
    q.bindValue(QStringLiteral(":list_id"), nonNullString(listId));
    q.bindValue(QStringLiteral(":list_name"), nonNullString(listName));
    q.bindValue(QStringLiteral(":task_id"), taskId);
    q.bindValue(QStringLiteral(":jira_key"), nonNullString(jiraKey));
    q.bindValue(QStringLiteral(":task_description"), nonNullString(taskDescription));
    q.bindValue(QStringLiteral(":start_time_utc"), startTimeUtc.toSecsSinceEpoch());
    q.bindValue(QStringLiteral(":runtime_seconds"), runtimeSeconds);
    q.bindValue(QStringLiteral(":stop_time_utc"), stopTimeUtc.toSecsSinceEpoch());
    q.bindValue(QStringLiteral(":worklog_synced"), worklogSynced ? 1 : 0);
    q.bindValue(QStringLiteral(":is_pomodoro_completed"), isPomodoroCompleted ? 1 : 0);
    if (!q.exec()) {
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

bool ActivityRepository::markActivityWorklogSynced(qint64 logId) {
    if (logId < 0) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE activity_log SET worklog_synced = 1 WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), logId);
    return q.exec() && q.numRowsAffected() > 0;
}

QVector<ActivityRecord> ActivityRepository::loadActivityLogs(const QDateTime& startUtc,
                                                             const QDateTime& endUtc,
                                                             const QString& listId) const
{
    QVector<ActivityRecord> records;
    QSqlQuery q(db());

    QString sql = QStringLiteral(
        "SELECT id, list_id, list_name, task_id, jira_key, task_description, "
        "start_time_utc, runtime_seconds, stop_time_utc, worklog_synced, is_pomodoro_completed "
        "FROM activity_log WHERE 1=1"
    );

    if (startUtc.isValid()) {
        sql += QStringLiteral(" AND start_time_utc >= :start_time");
    }
    if (endUtc.isValid()) {
        sql += QStringLiteral(" AND start_time_utc <= :end_time");
    }
    QStringList targetIds;
    if (!listId.trimmed().isEmpty() && listId != QStringLiteral("__ALL__")) {
        const QStringList rawIds = listId.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& rid : rawIds) {
            const QString trimmedId = rid.trimmed();
            if (!trimmedId.isEmpty()) {
                targetIds.append(trimmedId);
            }
        }
    }
    if (targetIds.size() == 1) {
        sql += QStringLiteral(" AND list_id = :list_id");
    } else if (targetIds.size() > 1) {
        QStringList placeholders;
        placeholders.reserve(targetIds.size());
        for (int i = 0; i < targetIds.size(); ++i) {
            placeholders.append(QStringLiteral(":lid_%1").arg(i));
        }
        sql += QStringLiteral(" AND list_id IN (%1)").arg(placeholders.join(QStringLiteral(", ")));
    }
    sql += QStringLiteral(" ORDER BY start_time_utc DESC");

    q.prepare(sql);
    if (startUtc.isValid()) {
        q.bindValue(QStringLiteral(":start_time"), startUtc.toSecsSinceEpoch());
    }
    if (endUtc.isValid()) {
        q.bindValue(QStringLiteral(":end_time"), endUtc.toSecsSinceEpoch());
    }
    if (targetIds.size() == 1) {
        q.bindValue(QStringLiteral(":list_id"), targetIds.first());
    } else if (targetIds.size() > 1) {
        for (int i = 0; i < targetIds.size(); ++i) {
            q.bindValue(QStringLiteral(":lid_%1").arg(i), targetIds.at(i));
        }
    }

    if (!q.exec()) {
        return records;
    }

    while (q.next()) {
        ActivityRecord r;
        r.id = q.value(0).toLongLong();
        r.listId = q.value(1).toString();
        r.listName = q.value(2).toString();
        r.taskId = q.value(3).toLongLong();
        r.jiraKey = q.value(4).toString();
        r.taskDescription = q.value(5).toString();
        r.startTimeUtc = QDateTime::fromSecsSinceEpoch(q.value(6).toLongLong(), QTimeZone::UTC);
        r.runtimeSeconds = q.value(7).toUInt();
        r.stopTimeUtc = QDateTime::fromSecsSinceEpoch(q.value(8).toLongLong(), QTimeZone::UTC);
        r.worklogSynced = q.value(9).toInt() != 0;
        r.isPomodoroCompleted = q.value(10).toInt() != 0;
        records.append(std::move(r));
    }
    return records;
}

} // namespace gofi
