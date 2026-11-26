#include "TaskRepository.h"
#include "DatabaseManager.h"
#include "DatabaseSerialization.h"
#include <QDate>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace gofi {

TaskRepository::TaskRepository(DatabaseManager* db)
    : m_dbManager(db)
{
}

QSqlDatabase TaskRepository::db() const {
    if (m_dbManager) {
        return m_dbManager->db();
    }
    return DatabaseManager::instance().db();
}

QVector<TxtTask> TaskRepository::loadTasks(const QStringList& listIds, bool done) const {
    QVector<TxtTask> result;
    const bool matchAll = listIds.isEmpty() || listIds.contains(QStringLiteral("__ALL__")) || listIds.contains(QStringLiteral("*"));

    QString sql = QStringLiteral(
        "SELECT t.id, t.description, t.done, t.status, t.priority, t.creation_date, t.completion_date, "
        "t.duration_seconds, t.timer_seconds, t.jira_key, t.jira_synced_seconds, t.details, t.comments_json, t.is_viewed, "
        "t.list_id, COALESCE(l.name, ''), COALESCE(l.provider, 'SQLite') "
        "FROM tasks t LEFT JOIN lists l ON t.list_id = l.id "
        "WHERE t.done = :done "
    );

    if (!matchAll) {
        QStringList placeholders;
        for (int i = 0; i < listIds.size(); ++i) {
            placeholders.append(QStringLiteral(":lid_%1").arg(i));
        }
        sql += QStringLiteral("AND t.list_id IN (%1) ").arg(placeholders.join(QLatin1Char(',')));
    }
    sql += QStringLiteral("ORDER BY t.sort_order ASC, t.id ASC");

    QSqlQuery q(db());
    q.prepare(sql);
    q.bindValue(QStringLiteral(":done"), done ? 1 : 0);
    if (!matchAll) {
        for (int i = 0; i < listIds.size(); ++i) {
            q.bindValue(QStringLiteral(":lid_%1").arg(i), listIds.at(i));
        }
    }
    if (!q.exec()) {
        return result;
    }
    while (q.next()) {
        const qint64 id = q.value(0).toLongLong();
        const QString desc = q.value(1).toString();
        const bool isDone = q.value(2).toInt() != 0;
        const QString status = q.value(3).toString();
        const auto prio = static_cast<quint8>(q.value(4).toUInt());
        const QDate cDate = QDate::fromString(q.value(5).toString(), Qt::ISODate);
        const QDate compDate = QDate::fromString(q.value(6).toString(), Qt::ISODate);
        const quint32 duration = q.value(7).toUInt();
        const quint32 timerVal = q.value(8).toUInt();
        const QString jiraKey = q.value(9).toString();
        const quint32 jiraSynced = q.value(10).toUInt();
        const QString details = q.value(11).toString();
        const QVariantList comments = deserializeCommentsList(q.value(12).toString());
        const bool isViewed = q.value(13).isNull() ? true : (q.value(13).toInt() != 0);
        const QString lId = q.value(14).toString();
        const QString lName = q.value(15).toString();
        const QString lProvider = q.value(16).toString();

        result.append(TxtTask::fromDatabase(
            id, desc, isDone, status, prio, cDate, compDate, duration, timerVal,
            jiraKey, jiraSynced, details, comments, isViewed, lId, lName, lProvider
        ));
    }
    return result;
}

std::optional<TxtTask> TaskRepository::loadTask(qint64 taskId, bool* queryError) const {
    if (queryError) {
        *queryError = false;
    }
    if (taskId < 0) return std::nullopt;
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "SELECT t.id, t.description, t.done, t.status, t.priority, t.creation_date, t.completion_date, "
        "t.duration_seconds, t.timer_seconds, t.jira_key, t.jira_synced_seconds, t.details, t.comments_json, t.is_viewed, "
        "t.list_id, COALESCE(l.name, ''), COALESCE(l.provider, 'SQLite') "
        "FROM tasks t LEFT JOIN lists l ON t.list_id = l.id "
        "WHERE t.id = :id"
    ));
    q.bindValue(QStringLiteral(":id"), taskId);
    if (!q.exec()) {
        if (queryError) {
            *queryError = true;
        }
        return std::nullopt;
    }
    if (!q.next()) {
        return std::nullopt;
    }
    const qint64 id = q.value(0).toLongLong();
    const QString desc = q.value(1).toString();
    const bool isDone = q.value(2).toInt() != 0;
    const QString status = q.value(3).toString();
    const auto prio = static_cast<quint8>(q.value(4).toUInt());
    const QDate cDate = QDate::fromString(q.value(5).toString(), Qt::ISODate);
    const QDate compDate = QDate::fromString(q.value(6).toString(), Qt::ISODate);
    const quint32 duration = q.value(7).toUInt();
    const quint32 timerVal = q.value(8).toUInt();
    const QString jiraKey = q.value(9).toString();
    const quint32 jiraSynced = q.value(10).toUInt();
    const QString details = q.value(11).toString();
    const QVariantList comments = deserializeCommentsList(q.value(12).toString());
    const bool isViewed = q.value(13).isNull() ? true : (q.value(13).toInt() != 0);
    const QString lId = q.value(14).toString();
    const QString lName = q.value(15).toString();
    const QString lProvider = q.value(16).toString();

    return TxtTask::fromDatabase(
        id, desc, isDone, status, prio, cDate, compDate, duration, timerVal,
        jiraKey, jiraSynced, details, comments, isViewed, lId, lName, lProvider
    );
}

QVector<TxtTask> TaskRepository::loadTasks(const QString& listId, bool done) const {
    if (listId.isEmpty()) return {};
    return loadTasks(QStringList{listId}, done);
}

int TaskRepository::countAllTasksInLists(const QStringList& listIds) const {
    const bool matchAll = listIds.isEmpty() || listIds.contains(QStringLiteral("__ALL__")) || listIds.contains(QStringLiteral("*"));
    QString sql = QStringLiteral("SELECT COUNT(*) FROM tasks ");
    if (!matchAll) {
        QStringList placeholders;
        for (int i = 0; i < listIds.size(); ++i) {
            placeholders.append(QStringLiteral(":lid_%1").arg(i));
        }
        sql += QStringLiteral("WHERE list_id IN (%1)").arg(placeholders.join(QLatin1Char(',')));
    }
    QSqlQuery q(db());
    q.prepare(sql);
    if (!matchAll) {
        for (int i = 0; i < listIds.size(); ++i) {
            q.bindValue(QStringLiteral(":lid_%1").arg(i), listIds.at(i));
        }
    }
    if (q.exec() && q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

int TaskRepository::countAllTasksInList(const QString& listId) const {
    if (listId.isEmpty()) return 0;
    return countAllTasksInLists(QStringList{listId});
}

bool TaskRepository::isOnboardingCompleted(const QString& listId) const {
    if (listId.isEmpty()) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT onboarding_completed FROM lists WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), listId);
    if (q.exec() && q.next()) {
        return q.value(0).toInt() != 0;
    }
    return false;
}

bool TaskRepository::setOnboardingCompleted(const QString& listId, bool completed) {
    if (listId.isEmpty()) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE lists SET onboarding_completed = :val WHERE id = :id"));
    q.bindValue(QStringLiteral(":val"), completed ? 1 : 0);
    q.bindValue(QStringLiteral(":id"), listId);
    return q.exec() && q.numRowsAffected() > 0;
}

qint64 TaskRepository::insertTask(const QString& listId, const TxtTask& task, int sortOrder) {
    if (listId.isEmpty()) return -1;
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO tasks (list_id, description, done, status, priority, creation_date, "
        "completion_date, duration_seconds, timer_seconds, sort_order, jira_key, jira_synced_seconds, details, comments_json, is_viewed) "
        "VALUES (:list_id, :description, :done, :status, :priority, :creation_date, "
        ":completion_date, :duration_seconds, :timer_seconds, :sort_order, :jira_key, :jira_synced_seconds, :details, :comments_json, :is_viewed)"
    ));
    q.bindValue(QStringLiteral(":list_id"), nonNullString(listId));
    q.bindValue(QStringLiteral(":description"), nonNullString(task.description()));
    q.bindValue(QStringLiteral(":done"), task.isDone() ? 1 : 0);
    q.bindValue(QStringLiteral(":status"), nonNullString(task.status()));
    q.bindValue(QStringLiteral(":priority"), static_cast<int>(task.priority()));
    q.bindValue(QStringLiteral(":creation_date"), task.creationDate().isValid() ? task.creationDate().toString(Qt::ISODate) : QStringLiteral(""));
    q.bindValue(QStringLiteral(":completion_date"), task.completionDate().isValid() ? task.completionDate().toString(Qt::ISODate) : QStringLiteral(""));
    q.bindValue(QStringLiteral(":duration_seconds"), task.duration());
    q.bindValue(QStringLiteral(":timer_seconds"), task.timerValue());
    q.bindValue(QStringLiteral(":sort_order"), sortOrder);
    q.bindValue(QStringLiteral(":jira_key"), nonNullString(task.jiraKey()));
    q.bindValue(QStringLiteral(":jira_synced_seconds"), task.jiraSyncedSeconds());
    q.bindValue(QStringLiteral(":details"), nonNullString(task.details()));
    q.bindValue(QStringLiteral(":comments_json"), nonNullString(serializeCommentsList(task.comments())));
    q.bindValue(QStringLiteral(":is_viewed"), task.isViewed() ? 1 : 0);

    if (!q.exec()) {
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

qint64 TaskRepository::insertTaskOnTopAndReorder(const QString& listId, const TxtTask& task, const QVector<TxtTask>& existingTasks) {
    if (listId.isEmpty()) return -1;
    QSqlDatabase database = db();
    if (!database.transaction()) {
        return -1;
    }

    QSqlQuery q(database);
    q.prepare(QStringLiteral(
        "INSERT INTO tasks (list_id, description, done, status, priority, creation_date, "
        "completion_date, duration_seconds, timer_seconds, sort_order, jira_key, jira_synced_seconds, details, comments_json, is_viewed) "
        "VALUES (:list_id, :description, :done, :status, :priority, :creation_date, "
        ":completion_date, :duration_seconds, :timer_seconds, :sort_order, :jira_key, :jira_synced_seconds, :details, :comments_json, :is_viewed)"
    ));
    q.bindValue(QStringLiteral(":list_id"), nonNullString(listId));
    q.bindValue(QStringLiteral(":description"), nonNullString(task.description()));
    q.bindValue(QStringLiteral(":done"), task.isDone() ? 1 : 0);
    q.bindValue(QStringLiteral(":status"), nonNullString(task.status()));
    q.bindValue(QStringLiteral(":priority"), static_cast<int>(task.priority()));
    q.bindValue(QStringLiteral(":creation_date"), task.creationDate().isValid() ? task.creationDate().toString(Qt::ISODate) : QStringLiteral(""));
    q.bindValue(QStringLiteral(":completion_date"), task.completionDate().isValid() ? task.completionDate().toString(Qt::ISODate) : QStringLiteral(""));
    q.bindValue(QStringLiteral(":duration_seconds"), task.duration());
    q.bindValue(QStringLiteral(":timer_seconds"), task.timerValue());
    q.bindValue(QStringLiteral(":sort_order"), 0);
    q.bindValue(QStringLiteral(":jira_key"), nonNullString(task.jiraKey()));
    q.bindValue(QStringLiteral(":jira_synced_seconds"), task.jiraSyncedSeconds());
    q.bindValue(QStringLiteral(":details"), nonNullString(task.details()));
    q.bindValue(QStringLiteral(":comments_json"), nonNullString(serializeCommentsList(task.comments())));
    q.bindValue(QStringLiteral(":is_viewed"), task.isViewed() ? 1 : 0);

    if (!q.exec()) {
        database.rollback();
        return -1;
    }
    const qint64 newId = q.lastInsertId().toLongLong();

    QSqlQuery reorderQ(database);
    reorderQ.prepare(QStringLiteral(
        "UPDATE tasks SET sort_order = :order WHERE id = :id AND list_id = :list_id AND done = 0"
    ));

    int currentOrder = 1;
    for (const TxtTask& t : existingTasks) {
        const QString lid = !t.listId().isEmpty() ? t.listId() : listId;
        if (lid == listId && t.id() >= 0 && !t.isDone()) {
            reorderQ.bindValue(QStringLiteral(":order"), currentOrder);
            reorderQ.bindValue(QStringLiteral(":id"), t.id());
            reorderQ.bindValue(QStringLiteral(":list_id"), lid);
            if (!reorderQ.exec() || reorderQ.numRowsAffected() <= 0) {
                database.rollback();
                return -1;
            }
            ++currentOrder;
        }
    }

    if (!database.commit()) {
        database.rollback();
        return -1;
    }
    return newId;
}

bool TaskRepository::updateTask(const TxtTask& task) {
    if (task.id() < 0) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "UPDATE tasks SET description = :description, done = :done, status = :status, priority = :priority, "
        "creation_date = :creation_date, completion_date = :completion_date, "
        "duration_seconds = :duration_seconds, timer_seconds = :timer_seconds, "
        "jira_key = :jira_key, jira_synced_seconds = :jira_synced_seconds, "
        "details = :details, comments_json = :comments_json, is_viewed = :is_viewed "
        "WHERE id = :id"
    ));
    q.bindValue(QStringLiteral(":description"), nonNullString(task.description()));
    q.bindValue(QStringLiteral(":done"), task.isDone() ? 1 : 0);
    q.bindValue(QStringLiteral(":status"), nonNullString(task.status()));
    q.bindValue(QStringLiteral(":priority"), static_cast<int>(task.priority()));
    q.bindValue(QStringLiteral(":creation_date"), task.creationDate().isValid() ? task.creationDate().toString(Qt::ISODate) : QStringLiteral(""));
    q.bindValue(QStringLiteral(":completion_date"), task.completionDate().isValid() ? task.completionDate().toString(Qt::ISODate) : QStringLiteral(""));
    q.bindValue(QStringLiteral(":duration_seconds"), task.duration());
    q.bindValue(QStringLiteral(":timer_seconds"), task.timerValue());
    q.bindValue(QStringLiteral(":jira_key"), nonNullString(task.jiraKey()));
    q.bindValue(QStringLiteral(":jira_synced_seconds"), task.jiraSyncedSeconds());
    q.bindValue(QStringLiteral(":details"), nonNullString(task.details()));
    q.bindValue(QStringLiteral(":comments_json"), nonNullString(serializeCommentsList(task.comments())));
    q.bindValue(QStringLiteral(":is_viewed"), task.isViewed() ? 1 : 0);
    q.bindValue(QStringLiteral(":id"), task.id());
    return q.exec() && q.numRowsAffected() > 0;
}

bool TaskRepository::markTaskViewed(qint64 taskId, bool viewed) {
    if (taskId < 0) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE tasks SET is_viewed = :viewed WHERE id = :id"));
    q.bindValue(QStringLiteral(":viewed"), viewed ? 1 : 0);
    q.bindValue(QStringLiteral(":id"), taskId);
    return q.exec() && q.numRowsAffected() > 0;
}

bool TaskRepository::updateTaskDoneAndOrder(const TxtTask& task, int sortOrder) {
    if (task.id() < 0) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "UPDATE tasks SET done = :done, status = :status, completion_date = :completion_date, sort_order = :sort_order "
        "WHERE id = :id"
    ));
    q.bindValue(QStringLiteral(":done"), task.isDone() ? 1 : 0);
    q.bindValue(QStringLiteral(":status"), nonNullString(task.status()));
    q.bindValue(QStringLiteral(":completion_date"), task.completionDate().isValid() ? task.completionDate().toString(Qt::ISODate) : QStringLiteral(""));
    q.bindValue(QStringLiteral(":sort_order"), sortOrder);
    q.bindValue(QStringLiteral(":id"), task.id());
    return q.exec() && q.numRowsAffected() > 0;
}

bool TaskRepository::moveTaskDoneAndReorder(const TxtTask& task, int newOrder, const QMap<QString, QVector<TxtTask>>& remainingPerList, bool remainingDone) {
    if (task.id() < 0) return false;
    QSqlDatabase database = db();
    if (!database.transaction()) {
        return false;
    }

    QSqlQuery q(database);
    q.prepare(QStringLiteral(
        "UPDATE tasks SET done = :done, status = :status, completion_date = :completion_date, sort_order = :sort_order "
        "WHERE id = :id"
    ));
    q.bindValue(QStringLiteral(":done"), task.isDone() ? 1 : 0);
    q.bindValue(QStringLiteral(":status"), nonNullString(task.status()));
    q.bindValue(QStringLiteral(":completion_date"), task.completionDate().isValid() ? task.completionDate().toString(Qt::ISODate) : QStringLiteral(""));
    q.bindValue(QStringLiteral(":sort_order"), newOrder);
    q.bindValue(QStringLiteral(":id"), task.id());
    if (!q.exec() || q.numRowsAffected() <= 0) {
        database.rollback();
        return false;
    }

    QSqlQuery reorderQ(database);
    reorderQ.prepare(QStringLiteral(
        "UPDATE tasks SET sort_order = :order WHERE id = :id AND list_id = :list_id AND done = :done"
    ));
    for (auto it = remainingPerList.cbegin(); it != remainingPerList.cend(); ++it) {
        const QString& listId = it.key();
        const QVector<TxtTask>& tasks = it.value();
        for (int i = 0; i < tasks.size(); ++i) {
            if (tasks.at(i).id() < 0) continue;
            reorderQ.bindValue(QStringLiteral(":order"), i);
            reorderQ.bindValue(QStringLiteral(":id"), tasks.at(i).id());
            reorderQ.bindValue(QStringLiteral(":list_id"), listId);
            reorderQ.bindValue(QStringLiteral(":done"), remainingDone ? 1 : 0);
            if (!reorderQ.exec() || reorderQ.numRowsAffected() <= 0) {
                database.rollback();
                return false;
            }
        }
    }

    return database.commit();
}

bool TaskRepository::updateTaskTimer(qint64 taskId, quint32 timerSeconds, quint32 jiraSyncedSeconds) {
    if (taskId < 0) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "UPDATE tasks SET timer_seconds = :timer_seconds, jira_synced_seconds = :jira_synced_seconds "
        "WHERE id = :id"
    ));
    q.bindValue(QStringLiteral(":timer_seconds"), timerSeconds);
    q.bindValue(QStringLiteral(":jira_synced_seconds"), jiraSyncedSeconds);
    q.bindValue(QStringLiteral(":id"), taskId);
    return q.exec() && q.numRowsAffected() > 0;
}

bool TaskRepository::deleteTask(qint64 taskId) {
    if (taskId < 0) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM tasks WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), taskId);
    return q.exec() && q.numRowsAffected() > 0;
}

bool TaskRepository::deleteTaskAndReorder(qint64 taskId, const QString& listId, bool done, const QVector<TxtTask>& remainingTasks) {
    if (taskId < 0) return false;
    QSqlDatabase database = db();
    if (!database.transaction()) {
        return false;
    }

    QSqlQuery q(database);
    q.prepare(QStringLiteral("DELETE FROM tasks WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), taskId);
    if (!q.exec() || q.numRowsAffected() <= 0) {
        database.rollback();
        return false;
    }

    QSqlQuery reorderQ(database);
    reorderQ.prepare(QStringLiteral(
        "UPDATE tasks SET sort_order = :order WHERE id = :id AND list_id = :list_id AND done = :done"
    ));
    for (int i = 0; i < remainingTasks.size(); ++i) {
        const TxtTask& t = remainingTasks.at(i);
        if (t.id() < 0) continue;
        reorderQ.bindValue(QStringLiteral(":order"), i);
        reorderQ.bindValue(QStringLiteral(":id"), t.id());
        reorderQ.bindValue(QStringLiteral(":list_id"), listId);
        reorderQ.bindValue(QStringLiteral(":done"), done ? 1 : 0);
        if (!reorderQ.exec() || reorderQ.numRowsAffected() <= 0) {
            database.rollback();
            return false;
        }
    }

    if (!database.commit()) {
        database.rollback();
        return false;
    }
    return true;
}

bool TaskRepository::deleteDoneTasks(const QStringList& listIds) {
    QSqlDatabase database = db();
    if (!database.transaction()) {
        return false;
    }
    const bool matchAll = listIds.isEmpty() || listIds.contains(QStringLiteral("__ALL__")) || listIds.contains(QStringLiteral("*"));
    QSqlQuery q(database);
    if (matchAll) {
        q.prepare(QStringLiteral("DELETE FROM tasks WHERE done = 1"));
    } else {
        QStringList placeholders;
        for (int i = 0; i < listIds.size(); ++i) {
            placeholders.append(QStringLiteral(":lid_%1").arg(i));
        }
        q.prepare(QStringLiteral("DELETE FROM tasks WHERE done = 1 AND list_id IN (%1)").arg(placeholders.join(QLatin1Char(','))));
        for (int i = 0; i < listIds.size(); ++i) {
            q.bindValue(QStringLiteral(":lid_%1").arg(i), listIds.at(i));
        }
    }
    if (!q.exec()) {
        database.rollback();
        return false;
    }
    return database.commit();
}

bool TaskRepository::deleteDoneTasks(const QString& listId) {
    if (listId.isEmpty()) return false;
    return deleteDoneTasks(QStringList{listId});
}

bool TaskRepository::replaceListTasksOrders(const QString& listId, bool done, const QVector<TxtTask>& tasks) {
    if (listId.isEmpty()) return false;
    QSqlDatabase database = db();
    if (!database.transaction()) {
        return false;
    }
    QSqlQuery q(database);
    q.prepare(QStringLiteral(
        "UPDATE tasks SET sort_order = :order WHERE id = :id AND list_id = :list_id AND done = :done"
    ));
    for (int i = 0; i < tasks.size(); ++i) {
        if (tasks.at(i).id() < 0) continue;
        q.bindValue(QStringLiteral(":order"), i);
        q.bindValue(QStringLiteral(":id"), tasks.at(i).id());
        q.bindValue(QStringLiteral(":list_id"), listId);
        q.bindValue(QStringLiteral(":done"), done ? 1 : 0);
        if (!q.exec() || q.numRowsAffected() <= 0) {
            database.rollback();
            return false;
        }
    }
    return database.commit();
}

bool TaskRepository::upsertRemoteIssueTask(const QString& listId,
                                           const QString& issueKey,
                                           const QString& description,
                                           const QString& localStatus,
                                           quint8 priority,
                                           quint32 durationSeconds,
                                           const QString& details,
                                           const QVariantList& comments,
                                           bool* outIsNew,
                                           QString* outOldStatus,
                                           bool replaceDetails,
                                           bool replaceComments)
{
    if (outIsNew) *outIsNew = false;
    if (outOldStatus) outOldStatus->clear();
    if (listId.isEmpty() || issueKey.isEmpty()) return false;
    const QString normStatus = localStatus.isEmpty() ? QStringLiteral("todo") : localStatus;
    const bool isDone = TxtTask::isClosedStatus(normStatus);

    QSqlDatabase database = db();
    QSqlQuery check(database);
    check.prepare(QStringLiteral(
        "SELECT id, duration_seconds, details, status, comments_json, completion_date, done, sort_order FROM tasks WHERE list_id = :list_id AND jira_key = :jira_key LIMIT 1"
    ));
    check.bindValue(QStringLiteral(":list_id"), nonNullString(listId));
    check.bindValue(QStringLiteral(":jira_key"), nonNullString(issueKey));
    if (!check.exec()) {
        qWarning() << "TaskRepository::upsertRemoteIssueTask check query failed:" << check.lastError().text();
        return false;
    }
    if (check.next()) {
        const qint64 existingId = check.value(0).toLongLong();
        const quint32 existingDur = check.value(1).toUInt();
        const QString existingDetails = check.value(2).toString();
        const QString prevStatus = check.value(3).toString();
        const QString existingCommentsJson = check.value(4).toString();
        const QString existingCompletionDate = check.value(5).toString();
        const bool wasDone = check.value(6).toInt() == 1 || TxtTask::isClosedStatus(prevStatus);
        const int oldOrder = check.value(7).toInt();
        if (outOldStatus) *outOldStatus = prevStatus;

        QString completionDateToSet;
        if (isDone) {
            if (!wasDone || existingCompletionDate.trimmed().isEmpty()) {
                completionDateToSet = QDate::currentDate().toString(Qt::ISODate);
            } else {
                completionDateToSet = existingCompletionDate.trimmed();
            }
        } else {
            completionDateToSet = QStringLiteral("");
        }

        int targetOrder = oldOrder;
        if (wasDone != isDone) {
            QSqlQuery maxOrderQ(database);
            maxOrderQ.prepare(QStringLiteral("SELECT COALESCE(MAX(sort_order), -1) + 1 FROM tasks WHERE list_id = :list_id AND done = :done"));
            maxOrderQ.bindValue(QStringLiteral(":list_id"), nonNullString(listId));
            maxOrderQ.bindValue(QStringLiteral(":done"), isDone ? 1 : 0);
            if (!maxOrderQ.exec() || !maxOrderQ.next()) {
                qWarning() << "TaskRepository::upsertRemoteIssueTask targetOrder maxOrderQ failed:" << maxOrderQ.lastError().text();
                return false;
            }
            targetOrder = maxOrderQ.value(0).toInt();
        }

        QSqlQuery sp(database);
        if (!sp.exec(QStringLiteral("SAVEPOINT upsert_remote_task"))) {
            qWarning() << "TaskRepository::upsertRemoteIssueTask failed to create savepoint:" << sp.lastError().text();
            return false;
        }

        auto rollbackSavepoint = [&database]() {
            QSqlQuery rb(database);
            if (!rb.exec(QStringLiteral("ROLLBACK TO SAVEPOINT upsert_remote_task"))) {
                qWarning() << "TaskRepository::upsertRemoteIssueTask ROLLBACK TO SAVEPOINT failed:" << rb.lastError().text();
            }
            QSqlQuery rel(database);
            if (!rel.exec(QStringLiteral("RELEASE SAVEPOINT upsert_remote_task"))) {
                qWarning() << "TaskRepository::upsertRemoteIssueTask RELEASE SAVEPOINT after rollback failed:" << rel.lastError().text();
            }
        };

        QSqlQuery upd(database);
        upd.prepare(QStringLiteral(
            "UPDATE tasks SET description = :description, done = :done, status = :status, "
            "completion_date = :completion_date, sort_order = :sort_order, "
            "priority = :priority, duration_seconds = :duration, details = :details, comments_json = :comments_json "
            "WHERE id = :id"
        ));
        upd.bindValue(QStringLiteral(":description"), nonNullString(description));
        upd.bindValue(QStringLiteral(":done"), isDone ? 1 : 0);
        upd.bindValue(QStringLiteral(":status"), nonNullString(normStatus));
        upd.bindValue(QStringLiteral(":completion_date"), completionDateToSet);
        upd.bindValue(QStringLiteral(":sort_order"), targetOrder);
        upd.bindValue(QStringLiteral(":priority"), static_cast<int>(priority));
        upd.bindValue(QStringLiteral(":duration"), durationSeconds > 0 ? durationSeconds : existingDur);
        const bool keepDetails = !replaceDetails && details.trimmed().isEmpty();
        upd.bindValue(QStringLiteral(":details"), nonNullString(keepDetails ? existingDetails : details));
        const bool keepComments = !replaceComments && comments.isEmpty();
        const QString commentsJson = keepComments ? existingCommentsJson : serializeCommentsList(comments);
        upd.bindValue(QStringLiteral(":comments_json"), nonNullString(commentsJson));
        upd.bindValue(QStringLiteral(":id"), existingId);
        if (!upd.exec() || upd.numRowsAffected() <= 0) {
            rollbackSavepoint();
            return false;
        }

        if (wasDone != isDone) {
            QSqlQuery compactQ(database);
            compactQ.prepare(QStringLiteral(
                "UPDATE tasks SET sort_order = sort_order - 1 WHERE list_id = :list_id AND done = :wasDone AND sort_order > :oldOrder"
            ));
            compactQ.bindValue(QStringLiteral(":list_id"), nonNullString(listId));
            compactQ.bindValue(QStringLiteral(":wasDone"), wasDone ? 1 : 0);
            compactQ.bindValue(QStringLiteral(":oldOrder"), oldOrder);
            if (!compactQ.exec()) {
                rollbackSavepoint();
                return false;
            }
        }

        QSqlQuery rel(database);
        if (!rel.exec(QStringLiteral("RELEASE SAVEPOINT upsert_remote_task"))) {
            qWarning() << "TaskRepository::upsertRemoteIssueTask final RELEASE SAVEPOINT failed:" << rel.lastError().text();
            rollbackSavepoint();
            return false;
        }

        return true;
    }

    if (outIsNew) *outIsNew = true;

    QSqlQuery maxOrderQ(database);
    maxOrderQ.prepare(QStringLiteral("SELECT COALESCE(MAX(sort_order), -1) + 1 FROM tasks WHERE list_id = :list_id AND done = :done"));
    maxOrderQ.bindValue(QStringLiteral(":list_id"), nonNullString(listId));
    maxOrderQ.bindValue(QStringLiteral(":done"), isDone ? 1 : 0);
    if (!maxOrderQ.exec() || !maxOrderQ.next()) {
        qWarning() << "TaskRepository::upsertRemoteIssueTask new task maxOrderQ failed:" << maxOrderQ.lastError().text();
        return false;
    }
    const int nextOrder = maxOrderQ.value(0).toInt();

    TxtTask newTask = TxtTask::fromDatabase(
        -1,
        description,
        isDone,
        normStatus,
        priority,
        QDate::currentDate(),
        isDone ? QDate::currentDate() : QDate(),
        durationSeconds,
        0,
        issueKey,
        0,
        details,
        comments,
        false // new synced tasks start as unviewed
    );
    return insertTask(listId, newTask, nextOrder) >= 0;
}

} // namespace gofi
