#include "DatabaseSchema.h"
#include <QSqlQuery>
#include <QString>

namespace gofi {

bool DatabaseSchema::createSchema(QSqlDatabase database) {
    QSqlQuery q(database);

    q.exec(QStringLiteral("PRAGMA foreign_keys = ON;"));
    q.exec(QStringLiteral("PRAGMA journal_mode = WAL;"));
    q.exec(QStringLiteral("PRAGMA synchronous = NORMAL;"));

    auto ensureColumn = [&q](const QString& table, const QString& colDef) -> bool {
        const QString colName = colDef.section(QLatin1Char(' '), 0, 0);
        if (!q.exec(QStringLiteral("PRAGMA table_info(%1);").arg(table))) {
            return false;
        }
        bool found = false;
        while (q.next()) {
            if (q.value(1).toString().compare(colName, Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return q.exec(QStringLiteral("ALTER TABLE %1 ADD COLUMN %2;").arg(table, colDef));
        }
        return true;
    };

    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS lists ("
            "  id TEXT PRIMARY KEY,"
            "  name TEXT NOT NULL,"
            "  provider TEXT NOT NULL DEFAULT 'SQLite',"
            "  sort_order INTEGER NOT NULL DEFAULT 0,"
            "  activity_log_path TEXT NOT NULL DEFAULT '',"
            "  add_creation_dates INTEGER NOT NULL DEFAULT 1,"
            "  reminder_time INTEGER NOT NULL DEFAULT -1,"
            "  custom_schedule TEXT NOT NULL DEFAULT '',"
            "  jira_url TEXT NOT NULL DEFAULT '',"
            "  jira_user TEXT NOT NULL DEFAULT '',"
            "  jira_jql TEXT NOT NULL DEFAULT '',"
            "  api_token TEXT NOT NULL DEFAULT '',"
            "  auto_worklog INTEGER NOT NULL DEFAULT 1,"
            "  sync_status_to_remote INTEGER NOT NULL DEFAULT 0,"
            "  status_mapping TEXT NOT NULL DEFAULT '',"
            "  priority_mapping TEXT NOT NULL DEFAULT ''"
            ");"))) {
        return false;
    }
    if (!ensureColumn(QStringLiteral("lists"), QStringLiteral("api_token TEXT NOT NULL DEFAULT ''"))) return false;
    if (!ensureColumn(QStringLiteral("lists"), QStringLiteral("auto_worklog INTEGER NOT NULL DEFAULT 1"))) return false;
    if (!ensureColumn(QStringLiteral("lists"), QStringLiteral("sync_status_to_remote INTEGER NOT NULL DEFAULT 0"))) return false;
    if (!ensureColumn(QStringLiteral("lists"), QStringLiteral("status_mapping TEXT NOT NULL DEFAULT ''"))) return false;
    if (!ensureColumn(QStringLiteral("lists"), QStringLiteral("priority_mapping TEXT NOT NULL DEFAULT ''"))) return false;
    if (!ensureColumn(QStringLiteral("lists"), QStringLiteral("onboarding_completed INTEGER NOT NULL DEFAULT 0"))) return false;

    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS tasks ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  list_id TEXT NOT NULL,"
            "  description TEXT NOT NULL,"
            "  done INTEGER NOT NULL DEFAULT 0,"
            "  status TEXT NOT NULL DEFAULT 'todo',"
            "  priority INTEGER NOT NULL DEFAULT 127,"
            "  creation_date TEXT NOT NULL DEFAULT '',"
            "  completion_date TEXT NOT NULL DEFAULT '',"
            "  duration_seconds INTEGER NOT NULL DEFAULT 0,"
            "  timer_seconds INTEGER NOT NULL DEFAULT 0,"
            "  sort_order INTEGER NOT NULL DEFAULT 0,"
            "  jira_key TEXT NOT NULL DEFAULT '',"
            "  jira_synced_seconds INTEGER NOT NULL DEFAULT 0,"
            "  details TEXT NOT NULL DEFAULT '',"
            "  comments_json TEXT NOT NULL DEFAULT '',"
            "  FOREIGN KEY (list_id) REFERENCES lists(id) ON DELETE CASCADE"
            ");"))) {
        return false;
    }
    if (!ensureColumn(QStringLiteral("tasks"), QStringLiteral("status TEXT NOT NULL DEFAULT 'todo'"))) return false;
    if (!ensureColumn(QStringLiteral("tasks"), QStringLiteral("details TEXT NOT NULL DEFAULT ''"))) return false;
    if (!ensureColumn(QStringLiteral("tasks"), QStringLiteral("comments_json TEXT NOT NULL DEFAULT ''"))) return false;
    if (!ensureColumn(QStringLiteral("tasks"), QStringLiteral("is_viewed INTEGER NOT NULL DEFAULT 1"))) return false;
    if (!q.exec(QStringLiteral("UPDATE tasks SET status = 'done' WHERE done = 1 AND status = 'todo';"))) return false;
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tasks_list_done_order ON tasks(list_id, done, sort_order);"))) return false;

    // Consolidate tracked timer and duration from duplicates before removing duplicate rows
    if (!database.transaction()) {
        return false;
    }
    if (!q.exec(QStringLiteral(
        "UPDATE tasks "
        "SET timer_seconds = (SELECT SUM(t2.timer_seconds) FROM tasks t2 WHERE t2.list_id = tasks.list_id AND t2.jira_key = tasks.jira_key AND t2.jira_key != ''), "
        "    duration_seconds = (SELECT MAX(t2.duration_seconds) FROM tasks t2 WHERE t2.list_id = tasks.list_id AND t2.jira_key = tasks.jira_key AND t2.jira_key != ''), "
        "    jira_synced_seconds = (SELECT SUM(t2.jira_synced_seconds) FROM tasks t2 WHERE t2.list_id = tasks.list_id AND t2.jira_key = tasks.jira_key AND t2.jira_key != '') "
        "WHERE id IN (SELECT MIN(id) FROM tasks WHERE jira_key != '' GROUP BY list_id, jira_key HAVING COUNT(*) > 1);"
    ))) {
        database.rollback();
        return false;
    }
    if (!q.exec(QStringLiteral("DELETE FROM tasks WHERE jira_key != '' AND id NOT IN (SELECT MIN(id) FROM tasks WHERE jira_key != '' GROUP BY list_id, jira_key);"))) {
        database.rollback();
        return false;
    }
    if (!q.exec(QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_list_jira_key ON tasks(list_id, jira_key) WHERE jira_key != '';"))) {
        database.rollback();
        return false;
    }
    if (!database.commit()) {
        database.rollback();
        return false;
    }

    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS activity_log ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  list_id TEXT NOT NULL,"
            "  list_name TEXT NOT NULL,"
            "  task_id INTEGER NOT NULL DEFAULT -1,"
            "  jira_key TEXT NOT NULL DEFAULT '',"
            "  task_description TEXT NOT NULL,"
            "  start_time_utc INTEGER NOT NULL,"
            "  runtime_seconds INTEGER NOT NULL,"
            "  stop_time_utc INTEGER NOT NULL,"
            "  worklog_synced INTEGER NOT NULL DEFAULT 0,"
            "  is_pomodoro_completed INTEGER NOT NULL DEFAULT 0"
            ");"))) {
        return false;
    }

    if (!ensureColumn(QStringLiteral("activity_log"), QStringLiteral("is_pomodoro_completed INTEGER NOT NULL DEFAULT 0"))) return false;
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_activity_log_list ON activity_log(list_id, start_time_utc);"))) return false;
    if (!q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_activity_log_time ON activity_log(start_time_utc);"))) return false;

    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS app_settings ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT NOT NULL DEFAULT ''"
            ");"))) {
        return false;
    }
    return true;
}

} // namespace gofi
