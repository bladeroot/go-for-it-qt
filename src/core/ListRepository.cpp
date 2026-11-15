#include "ListRepository.h"
#include "CredentialStore.h"
#include "DatabaseManager.h"
#include "DatabaseSerialization.h"
#include <QSqlDatabase>
#include <QSqlQuery>

namespace gofi {

ListRepository::ListRepository(DatabaseManager* db)
    : m_dbManager(db)
{
}

QSqlDatabase ListRepository::db() const {
    if (m_dbManager) {
        return m_dbManager->db();
    }
    return DatabaseManager::instance().db();
}

QString ListRepository::databasePath() const {
    if (m_dbManager) {
        return m_dbManager->databasePath();
    }
    return DatabaseManager::instance().databasePath();
}

QVector<TodoListEntry> ListRepository::loadLists() const {
    QVector<TodoListEntry> result;
    CredentialStore::instance().unlockKeyringForCurrentDatabase();
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "SELECT id, name, provider, activity_log_path, add_creation_dates, "
        "reminder_time, custom_schedule, jira_url, jira_user, jira_jql, api_token, auto_worklog, status_mapping, priority_mapping, sync_status_to_remote "
        "FROM lists ORDER BY sort_order ASC, rowid ASC"
    ));
    if (!q.exec()) {
        return result;
    }
    const QString currentDbPath = databasePath();
    while (q.next()) {
        TodoListEntry e;
        e.id = q.value(0).toString();
        e.name = q.value(1).toString();
        e.provider = q.value(2).toString();
        e.activityLogPath = q.value(3).toString();
        e.addCreationDates = q.value(4).toInt() != 0;
        e.reminderTime = q.value(5).toInt();
        e.customSchedule = deserializeSchedule(q.value(6).toString());
        e.jiraUrl = q.value(7).toString();
        e.jiraUser = q.value(8).toString();
        e.jiraJql = q.value(9).toString();
        e.apiToken = CredentialStore::instance().loadToken(e.id);
        if (e.apiToken.isEmpty()) {
            const QString legacyToken = q.value(10).toString();
            if (!legacyToken.isEmpty()) {
                e.apiToken = legacyToken;
                CredentialStore::instance().storeToken(e.id, legacyToken);
            }
        }
        e.autoWorklog = q.value(11).toInt() != 0;
        e.statusMapping = deserializeStatusMapping(q.value(12).toString());
        e.priorityMapping = deserializePriorityMapping(q.value(13).toString());
        e.syncStatusToRemote = q.value(14).toInt() != 0;
        e.directoryPath = currentDbPath;
        result.append(std::move(e));
    }
    return result;
}

bool ListRepository::saveList(const TodoListEntry& entry, int sortOrder) {
    if (!entry.id.isEmpty()) {
        if (entry.apiToken.isEmpty()) {
            CredentialStore::instance().deleteToken(entry.id);
        } else {
            CredentialStore::instance().storeToken(entry.id, entry.apiToken);
        }
    }
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO lists (id, name, provider, sort_order, activity_log_path, "
        "add_creation_dates, reminder_time, custom_schedule, jira_url, jira_user, jira_jql, api_token, auto_worklog, status_mapping, priority_mapping, sync_status_to_remote) "
        "VALUES (:id, :name, :provider, :sort_order, :activity_log_path, "
        ":add_creation_dates, :reminder_time, :custom_schedule, :jira_url, :jira_user, :jira_jql, :api_token, :auto_worklog, :status_mapping, :priority_mapping, :sync_status_to_remote) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  name = excluded.name,"
        "  provider = excluded.provider,"
        "  sort_order = excluded.sort_order,"
        "  activity_log_path = excluded.activity_log_path,"
        "  add_creation_dates = excluded.add_creation_dates,"
        "  reminder_time = excluded.reminder_time,"
        "  custom_schedule = excluded.custom_schedule,"
        "  jira_url = excluded.jira_url,"
        "  jira_user = excluded.jira_user,"
        "  jira_jql = excluded.jira_jql,"
        "  api_token = excluded.api_token,"
        "  auto_worklog = excluded.auto_worklog,"
        "  status_mapping = excluded.status_mapping,"
        "  priority_mapping = excluded.priority_mapping,"
        "  sync_status_to_remote = excluded.sync_status_to_remote"
    ));
    q.bindValue(QStringLiteral(":id"), nonNullString(entry.id));
    q.bindValue(QStringLiteral(":name"), nonNullString(entry.name));
    q.bindValue(QStringLiteral(":provider"), entry.provider.isEmpty() ? QStringLiteral("SQLite") : entry.provider);
    q.bindValue(QStringLiteral(":sort_order"), sortOrder);
    q.bindValue(QStringLiteral(":activity_log_path"), nonNullString(entry.activityLogPath));
    q.bindValue(QStringLiteral(":add_creation_dates"), entry.addCreationDates ? 1 : 0);
    q.bindValue(QStringLiteral(":reminder_time"), entry.reminderTime);
    q.bindValue(QStringLiteral(":custom_schedule"), nonNullString(serializeSchedule(entry.customSchedule)));
    q.bindValue(QStringLiteral(":jira_url"), nonNullString(entry.jiraUrl));
    q.bindValue(QStringLiteral(":jira_user"), nonNullString(entry.jiraUser));
    q.bindValue(QStringLiteral(":jira_jql"), nonNullString(entry.jiraJql));
    q.bindValue(QStringLiteral(":api_token"), QStringLiteral(""));
    q.bindValue(QStringLiteral(":auto_worklog"), entry.autoWorklog ? 1 : 0);
    q.bindValue(QStringLiteral(":status_mapping"), nonNullString(serializeStatusMapping(entry.statusMapping)));
    q.bindValue(QStringLiteral(":priority_mapping"), nonNullString(serializePriorityMapping(entry.priorityMapping)));
    q.bindValue(QStringLiteral(":sync_status_to_remote"), entry.syncStatusToRemote ? 1 : 0);
    return q.exec();
}

bool ListRepository::updateListStatusMapping(const QString& listId, const QVariantMap& mapping) {
    if (listId.isEmpty()) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE lists SET status_mapping = :status_mapping WHERE id = :id"));
    q.bindValue(QStringLiteral(":status_mapping"), nonNullString(serializeStatusMapping(mapping)));
    q.bindValue(QStringLiteral(":id"), listId);
    return q.exec() && q.numRowsAffected() > 0;
}

bool ListRepository::removeList(const QString& listId, bool* notFound) {
    if (notFound) {
        *notFound = false;
    }
    if (listId.isEmpty()) {
        if (notFound) {
            *notFound = true;
        }
        return false;
    }
    CredentialStore::instance().deleteToken(listId);
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM lists WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), listId);
    if (!q.exec()) {
        return false;
    }
    if (q.numRowsAffected() <= 0) {
        if (notFound) {
            *notFound = true;
        }
        return false;
    }
    return true;
}

bool ListRepository::updateListsOrder(const QVector<TodoListEntry>& entries) {
    QSqlDatabase database = db();
    if (!database.transaction()) {
        return false;
    }
    QSqlQuery q(database);
    q.prepare(QStringLiteral("UPDATE lists SET sort_order = :order WHERE id = :id"));
    for (int i = 0; i < entries.size(); ++i) {
        q.bindValue(QStringLiteral(":order"), i);
        q.bindValue(QStringLiteral(":id"), entries.at(i).id);
        if (!q.exec() || q.numRowsAffected() <= 0) {
            database.rollback();
            return false;
        }
    }
    return database.commit();
}

} // namespace gofi
