#include "DatabaseManager.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QVariant>

namespace gofi {

namespace {

QString expandUserPath(const QString& path) {
    const QString p = path.trimmed();
    if (p == QLatin1String("~")) {
        return QDir::homePath();
    }
    if (p.startsWith(QStringLiteral("~/")) || p.startsWith(QStringLiteral("~\\"))) {
        return QDir::homePath() + p.mid(1);
    }
    return p;
}

bool areSameFiles(const QString& pathA, const QString& pathB) {
    if (pathA.isEmpty() || pathB.isEmpty()) return false;
    const QFileInfo fiA(pathA);
    const QFileInfo fiB(pathB);
    if (fiA.exists() && fiB.exists()) {
        const QString cA = fiA.canonicalFilePath();
        const QString cB = fiB.canonicalFilePath();
        if (!cA.isEmpty() && !cB.isEmpty()) {
            return cA == cB;
        }
    }
    return QDir::cleanPath(fiA.absoluteFilePath()) == QDir::cleanPath(fiB.absoluteFilePath());
}

bool isValidGofiDatabase(const QString& filePath) {
    const QFileInfo fi(filePath);
    if (!fi.exists() || !fi.isFile() || fi.size() < 100) {
        return false;
    }

    // Verify SQLite 3 header: 16 bytes starting with "SQLite format 3\0"
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray header = f.read(16);
    f.close();
    if (header.size() < 16 || !header.startsWith("SQLite format 3") || header.at(15) != '\0') {
        return false;
    }

    // Verify schema and integrity using an isolated temporary connection
    const QString tempConnName = QStringLiteral("gofi_verify_%1_%2")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch());

    bool valid = false;
    {
        QSqlDatabase tempDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), tempConnName);
        tempDb.setDatabaseName(filePath);
        if (tempDb.open()) {
            QSqlQuery q(tempDb);
            bool integrityOk = false;
            if (q.exec(QStringLiteral("PRAGMA integrity_check;")) && q.next()) {
                if (q.value(0).toString().compare(QStringLiteral("ok"), Qt::CaseInsensitive) == 0) {
                    integrityOk = true;
                }
            }

            if (integrityOk) {
                // Must contain 'lists' and 'tasks' tables
                if (q.exec(QStringLiteral("SELECT count(*) FROM sqlite_master WHERE type='table' AND name IN ('lists', 'tasks');")) && q.next()) {
                    if (q.value(0).toInt() >= 2) {
                        valid = true;
                    }
                }
            }
            tempDb.close();
        }
    }
    QSqlDatabase::removeDatabase(tempConnName);
    return valid;
}

QString nonNullString(const QString& s) {
    return s.isNull() ? QStringLiteral("") : s;
}

QString serializeSchedule(const Schedule& schedule) {
    QJsonArray arr;
    for (const QVariant& v : schedule.toVariantList()) {
        arr.append(v.toInt());
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

Schedule deserializeSchedule(const QString& json) {
    Schedule sched;
    if (json.isEmpty()) {
        return sched;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isArray()) {
        sched.fromVariantList(doc.array().toVariantList());
    }
    return sched;
}

QString serializeStatusMapping(const QVariantMap& map) {
    if (map.isEmpty()) return QStringLiteral("");
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

QVariantMap deserializeStatusMapping(const QString& json) {
    if (json.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isObject()) {
        return doc.object().toVariantMap();
    }
    return {};
}

QString serializePriorityMapping(const QVariantMap& map) {
    if (map.isEmpty()) return QStringLiteral("");
    return QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(map)).toJson(QJsonDocument::Compact));
}

QVariantMap deserializePriorityMapping(const QString& json) {
    if (json.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isObject()) {
        return doc.object().toVariantMap();
    }
    return {};
}

QString serializeCommentsList(const QVariantList& comments) {
    if (comments.isEmpty()) return QStringLiteral("");
    return QString::fromUtf8(QJsonDocument(QJsonArray::fromVariantList(comments)).toJson(QJsonDocument::Compact));
}

QVariantList deserializeCommentsList(const QString& json) {
    if (json.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isArray()) {
        return doc.array().toVariantList();
    }
    return {};
}

} // namespace

DatabaseManager& DatabaseManager::instance() {
    static DatabaseManager inst;
    return inst;
}

DatabaseManager::~DatabaseManager() {
    close();
}

void DatabaseManager::close() {
    if (!QCoreApplication::instance()) {
        return;
    }
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase database = QSqlDatabase::database(m_connectionName, false);
            if (database.isOpen()) {
                database.close();
            }
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    m_dbPath.clear();
}

bool DatabaseManager::initialize(const QString& customDbPath) {
    QString targetPath = customDbPath;
    if (targetPath.isEmpty()) {
        QSettings boot(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("settings"));
        const QString configuredPath = boot.value(QStringLiteral("Storage/databasePath")).toString().trimmed();
        if (!configuredPath.isEmpty()) {
            targetPath = configuredPath;
            QFileInfo(targetPath).dir().mkpath(QStringLiteral("."));
        } else {
            const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QDir().mkpath(dataDir);
            QFile::setPermissions(dataDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            targetPath = dataDir + QStringLiteral("/goforit.db");
        }
    } else if (targetPath != QStringLiteral(":memory:")) {
        QFileInfo(targetPath).dir().mkpath(QStringLiteral("."));
    }

    if (m_dbPath == targetPath && isOpen()) {
        return true;
    }

    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase oldDb = QSqlDatabase::database(m_connectionName, false);
            if (oldDb.isOpen()) {
                oldDb.close();
            }
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }

    m_dbPath = targetPath;
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    database.setDatabaseName(m_dbPath);
    if (!database.open()) {
        return false;
    }

    if (m_dbPath != QStringLiteral(":memory:") && QFile::exists(m_dbPath)) {
        QFile::setPermissions(m_dbPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    return createSchema();
}

bool DatabaseManager::isOpen() const {
    if (!QSqlDatabase::contains(m_connectionName)) {
        return false;
    }
    return QSqlDatabase::database(m_connectionName, false).isOpen();
}

QSqlDatabase DatabaseManager::db() const {
    if (!isOpen()) {
        const_cast<DatabaseManager*>(this)->initialize();
    }
    return QSqlDatabase::database(m_connectionName);
}

bool DatabaseManager::createSchema() {
    QSqlDatabase database = QSqlDatabase::database(m_connectionName);
    QSqlQuery q(database);

    q.exec(QStringLiteral("PRAGMA foreign_keys = ON;"));
    q.exec(QStringLiteral("PRAGMA journal_mode = WAL;"));
    q.exec(QStringLiteral("PRAGMA synchronous = NORMAL;"));

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
            "  status_mapping TEXT NOT NULL DEFAULT '',"
            "  priority_mapping TEXT NOT NULL DEFAULT ''"
            ");"))) {
        return false;
    }
    q.exec(QStringLiteral("ALTER TABLE lists ADD COLUMN api_token TEXT NOT NULL DEFAULT '';"));
    q.exec(QStringLiteral("ALTER TABLE lists ADD COLUMN auto_worklog INTEGER NOT NULL DEFAULT 1;"));
    q.exec(QStringLiteral("ALTER TABLE lists ADD COLUMN status_mapping TEXT NOT NULL DEFAULT '';"));
    q.exec(QStringLiteral("ALTER TABLE lists ADD COLUMN priority_mapping TEXT NOT NULL DEFAULT '';"));

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
    q.exec(QStringLiteral("ALTER TABLE tasks ADD COLUMN status TEXT NOT NULL DEFAULT 'todo';"));
    q.exec(QStringLiteral("ALTER TABLE tasks ADD COLUMN details TEXT NOT NULL DEFAULT '';"));
    q.exec(QStringLiteral("ALTER TABLE tasks ADD COLUMN comments_json TEXT NOT NULL DEFAULT '';"));
    q.exec(QStringLiteral("ALTER TABLE tasks ADD COLUMN is_viewed INTEGER NOT NULL DEFAULT 1;"));
    q.exec(QStringLiteral("UPDATE tasks SET status = 'done' WHERE done = 1 AND status = 'todo';"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tasks_list_done_order ON tasks(list_id, done, sort_order);"));
    // Consolidate tracked timer and duration from duplicates before removing duplicate rows
    database.transaction();
    q.exec(QStringLiteral(
        "UPDATE tasks "
        "SET timer_seconds = (SELECT SUM(t2.timer_seconds) FROM tasks t2 WHERE t2.list_id = tasks.list_id AND t2.jira_key = tasks.jira_key AND t2.jira_key != ''), "
        "    duration_seconds = (SELECT MAX(t2.duration_seconds) FROM tasks t2 WHERE t2.list_id = tasks.list_id AND t2.jira_key = tasks.jira_key AND t2.jira_key != ''), "
        "    jira_synced_seconds = (SELECT SUM(t2.jira_synced_seconds) FROM tasks t2 WHERE t2.list_id = tasks.list_id AND t2.jira_key = tasks.jira_key AND t2.jira_key != '') "
        "WHERE id IN (SELECT MIN(id) FROM tasks WHERE jira_key != '' GROUP BY list_id, jira_key HAVING COUNT(*) > 1);"
    ));
    q.exec(QStringLiteral("DELETE FROM tasks WHERE jira_key != '' AND id NOT IN (SELECT MIN(id) FROM tasks WHERE jira_key != '' GROUP BY list_id, jira_key);"));
    q.exec(QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_tasks_list_jira_key ON tasks(list_id, jira_key) WHERE jira_key != '';"));
    database.commit();

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

    q.exec(QStringLiteral("ALTER TABLE activity_log ADD COLUMN is_pomodoro_completed INTEGER NOT NULL DEFAULT 0;"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_activity_log_list ON activity_log(list_id, start_time_utc);"));
    q.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_activity_log_time ON activity_log(start_time_utc);"));

    if (!q.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS app_settings ("
            "  key TEXT PRIMARY KEY,"
            "  value TEXT NOT NULL DEFAULT ''"
            ");"))) {
        return false;
    }
    return true;
}

bool DatabaseManager::relocateDatabase(const QString& newDbPath, bool copyExisting) {
    QString cleanTarget = expandUserPath(newDbPath);
    if (cleanTarget.isEmpty()) return false;
    if (QFileInfo(cleanTarget).isDir()) {
        cleanTarget = QDir(cleanTarget).filePath(QStringLiteral("goforit.db"));
    }
    cleanTarget = QFileInfo(cleanTarget).absoluteFilePath();
    if (areSameFiles(cleanTarget, m_dbPath)) return true;

    QFileInfo(cleanTarget).dir().mkpath(QStringLiteral("."));
    const QString oldPath = m_dbPath;

    if (copyExisting && !oldPath.isEmpty() && QFile::exists(oldPath)) {
        if (isOpen()) {
            QSqlQuery checkpointQ(db());
            checkpointQ.exec(QStringLiteral("PRAGMA wal_checkpoint(FULL);"));
        }

        const QString tempTarget = QFileInfo(cleanTarget).dir().filePath(
            QStringLiteral(".%1.tmp.%2").arg(QFileInfo(cleanTarget).fileName()).arg(QCoreApplication::applicationPid())
        );
        if (QFile::exists(tempTarget)) {
            QFile::remove(tempTarget);
        }
        if (!QFile::copy(oldPath, tempTarget)) {
            return false;
        }
        QFile::setPermissions(tempTarget, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (QFile::exists(cleanTarget)) {
            QFile::remove(cleanTarget);
        }
        if (!QFile::rename(tempTarget, cleanTarget)) {
            if (!QFile::copy(tempTarget, cleanTarget)) {
                QFile::remove(tempTarget);
                return false;
            }
            QFile::remove(tempTarget);
        }
    }

    QSettings boot(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("settings"));
    boot.setValue(QStringLiteral("Storage/databasePath"), cleanTarget);
    boot.sync();

    return initialize(cleanTarget);
}

bool DatabaseManager::exportDatabaseBackup(const QString& destinationFilePath) const {
    const QString cleanDest = expandUserPath(destinationFilePath);
    if (cleanDest.isEmpty() || m_dbPath.isEmpty()) return false;
    if (areSameFiles(cleanDest, m_dbPath)) return false;

    QFileInfo destInfo(cleanDest);
    destInfo.dir().mkpath(QStringLiteral("."));

    if (isOpen()) {
        QSqlQuery checkpointQ(db());
        checkpointQ.exec(QStringLiteral("PRAGMA wal_checkpoint(FULL);"));
    }

    const QString tempDest = destInfo.dir().filePath(
        QStringLiteral(".%1.tmp.%2").arg(destInfo.fileName()).arg(QCoreApplication::applicationPid())
    );
    if (QFile::exists(tempDest)) {
        QFile::remove(tempDest);
    }
    if (!QFile::copy(m_dbPath, tempDest)) {
        return false;
    }
    QFile::setPermissions(tempDest, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    if (QFile::exists(cleanDest)) {
        if (!QFile::rename(tempDest, cleanDest)) {
            QFile::remove(cleanDest);
            if (!QFile::rename(tempDest, cleanDest)) {
                if (!QFile::copy(tempDest, cleanDest)) {
                    QFile::remove(tempDest);
                    return false;
                }
                QFile::remove(tempDest);
            }
        }
    } else {
        if (!QFile::rename(tempDest, cleanDest)) {
            if (!QFile::copy(tempDest, cleanDest)) {
                QFile::remove(tempDest);
                return false;
            }
            QFile::remove(tempDest);
        }
    }
    QFile::setPermissions(cleanDest, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return QFile::exists(cleanDest);
}

bool DatabaseManager::importDatabaseBackup(const QString& sourceFilePath) {
    const QString cleanSrc = expandUserPath(sourceFilePath);
    if (cleanSrc.isEmpty() || !QFile::exists(cleanSrc)) return false;
    if (areSameFiles(cleanSrc, m_dbPath)) return false;

    if (!isValidGofiDatabase(cleanSrc)) {
        return false;
    }

    const QString targetPath = m_dbPath;
    if (targetPath.isEmpty()) return false;

    if (isOpen()) {
        QSqlQuery checkpointQ(db());
        checkpointQ.exec(QStringLiteral("PRAGMA wal_checkpoint(FULL);"));
    }

    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase oldDb = QSqlDatabase::database(m_connectionName, false);
            if (oldDb.isOpen()) oldDb.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    m_dbPath.clear();

    const QString rollbackDb = targetPath + QStringLiteral(".bak.%1").arg(QCoreApplication::applicationPid());
    const QString rollbackWal = targetPath + QStringLiteral("-wal.bak.%1").arg(QCoreApplication::applicationPid());
    const QString rollbackShm = targetPath + QStringLiteral("-shm.bak.%1").arg(QCoreApplication::applicationPid());

    QFile::remove(rollbackDb);
    QFile::remove(rollbackWal);
    QFile::remove(rollbackShm);

    const bool hasExisting = QFile::exists(targetPath);
    bool hasBackup = false;
    if (hasExisting) {
        hasBackup = QFile::rename(targetPath, rollbackDb);
        if (!hasBackup) {
            // Atomic rename failed (e.g. permission issue), abort to protect live DB
            initialize(targetPath);
            return false;
        }
        if (QFile::exists(targetPath + QStringLiteral("-wal"))) {
            QFile::rename(targetPath + QStringLiteral("-wal"), rollbackWal);
        }
        if (QFile::exists(targetPath + QStringLiteral("-shm"))) {
            QFile::rename(targetPath + QStringLiteral("-shm"), rollbackShm);
        }
    }

    bool copyOk = QFile::copy(cleanSrc, targetPath);
    bool initOk = false;
    if (copyOk) {
        QFile::setPermissions(targetPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        initOk = initialize(targetPath);
    }

    if (!copyOk || !initOk) {
        if (QSqlDatabase::contains(m_connectionName)) {
            {
                QSqlDatabase cur = QSqlDatabase::database(m_connectionName, false);
                if (cur.isOpen()) cur.close();
            }
            QSqlDatabase::removeDatabase(m_connectionName);
        }
        QFile::remove(targetPath);
        QFile::remove(targetPath + QStringLiteral("-wal"));
        QFile::remove(targetPath + QStringLiteral("-shm"));
        if (hasBackup) {
            QFile::rename(rollbackDb, targetPath);
            if (QFile::exists(rollbackWal)) QFile::rename(rollbackWal, targetPath + QStringLiteral("-wal"));
            if (QFile::exists(rollbackShm)) QFile::rename(rollbackShm, targetPath + QStringLiteral("-shm"));
            QFile::setPermissions(targetPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
        initialize(targetPath);

        QFile::remove(rollbackDb);
        QFile::remove(rollbackWal);
        QFile::remove(rollbackShm);
        return false;
    }

    QFile::remove(rollbackDb);
    QFile::remove(rollbackWal);
    QFile::remove(rollbackShm);
    return true;
}

QVariant DatabaseManager::getSetting(const QString& key, const QVariant& defaultValue) const {
    if (!isOpen() || key.isEmpty()) return defaultValue;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("SELECT value FROM app_settings WHERE key = :key LIMIT 1"));
    q.bindValue(QStringLiteral(":key"), nonNullString(key));
    if (q.exec() && q.next()) {
        return q.value(0);
    }
    return defaultValue;
}

bool DatabaseManager::setSetting(const QString& key, const QVariant& value) {
    if (!isOpen() || key.isEmpty()) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO app_settings (key, value) VALUES (:key, :value) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value"
    ));
    q.bindValue(QStringLiteral(":key"), nonNullString(key));
    q.bindValue(QStringLiteral(":value"), nonNullString(value.toString()));
    return q.exec();
}

QVector<TodoListEntry> DatabaseManager::loadLists() const {
    QVector<TodoListEntry> result;
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "SELECT id, name, provider, activity_log_path, add_creation_dates, "
        "reminder_time, custom_schedule, jira_url, jira_user, jira_jql, api_token, auto_worklog, status_mapping, priority_mapping "
        "FROM lists ORDER BY sort_order ASC, rowid ASC"
    ));
    if (!q.exec()) {
        return result;
    }
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
        e.apiToken = q.value(10).toString();
        e.autoWorklog = q.value(11).toInt() != 0;
        e.statusMapping = deserializeStatusMapping(q.value(12).toString());
        e.priorityMapping = deserializePriorityMapping(q.value(13).toString());
        e.directoryPath = m_dbPath;
        result.append(std::move(e));
    }
    return result;
}

bool DatabaseManager::saveList(const TodoListEntry& entry, int sortOrder) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO lists (id, name, provider, sort_order, activity_log_path, "
        "add_creation_dates, reminder_time, custom_schedule, jira_url, jira_user, jira_jql, api_token, auto_worklog, status_mapping, priority_mapping) "
        "VALUES (:id, :name, :provider, :sort_order, :activity_log_path, "
        ":add_creation_dates, :reminder_time, :custom_schedule, :jira_url, :jira_user, :jira_jql, :api_token, :auto_worklog, :status_mapping, :priority_mapping) "
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
        "  priority_mapping = excluded.priority_mapping"
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
    q.bindValue(QStringLiteral(":api_token"), nonNullString(entry.apiToken));
    q.bindValue(QStringLiteral(":auto_worklog"), entry.autoWorklog ? 1 : 0);
    q.bindValue(QStringLiteral(":status_mapping"), nonNullString(serializeStatusMapping(entry.statusMapping)));
    q.bindValue(QStringLiteral(":priority_mapping"), nonNullString(serializePriorityMapping(entry.priorityMapping)));
    return q.exec();
}

bool DatabaseManager::removeList(const QString& listId) {
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM lists WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), listId);
    return q.exec();
}

bool DatabaseManager::updateListsOrder(const QVector<TodoListEntry>& entries) {
    QSqlDatabase database = db();
    database.transaction();
    QSqlQuery q(database);
    q.prepare(QStringLiteral("UPDATE lists SET sort_order = :order WHERE id = :id"));
    for (int i = 0; i < entries.size(); ++i) {
        q.bindValue(QStringLiteral(":order"), i);
        q.bindValue(QStringLiteral(":id"), entries.at(i).id);
        if (!q.exec()) {
            database.rollback();
            return false;
        }
    }
    return database.commit();
}

QVector<TxtTask> DatabaseManager::loadTasks(const QStringList& listIds, bool done) const {
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

QVector<TxtTask> DatabaseManager::loadTasks(const QString& listId, bool done) const {
    if (listId.isEmpty()) return {};
    return loadTasks(QStringList{listId}, done);
}

int DatabaseManager::countAllTasksInLists(const QStringList& listIds) const {
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

int DatabaseManager::countAllTasksInList(const QString& listId) const {
    if (listId.isEmpty()) return 0;
    return countAllTasksInLists(QStringList{listId});
}

qint64 DatabaseManager::insertTask(const QString& listId, const TxtTask& task, int sortOrder) {
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

bool DatabaseManager::updateTask(const TxtTask& task) {
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
    return q.exec();
}

bool DatabaseManager::markTaskViewed(qint64 taskId, bool viewed) {
    if (taskId < 0) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE tasks SET is_viewed = :viewed WHERE id = :id"));
    q.bindValue(QStringLiteral(":viewed"), viewed ? 1 : 0);
    q.bindValue(QStringLiteral(":id"), taskId);
    return q.exec();
}

bool DatabaseManager::updateTaskDoneAndOrder(const TxtTask& task, int sortOrder) {
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
    return q.exec();
}

bool DatabaseManager::updateTaskTimer(qint64 taskId, quint32 timerSeconds, quint32 jiraSyncedSeconds) {
    if (taskId < 0) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "UPDATE tasks SET timer_seconds = :timer_seconds, jira_synced_seconds = :jira_synced_seconds "
        "WHERE id = :id"
    ));
    q.bindValue(QStringLiteral(":timer_seconds"), timerSeconds);
    q.bindValue(QStringLiteral(":jira_synced_seconds"), jiraSyncedSeconds);
    q.bindValue(QStringLiteral(":id"), taskId);
    return q.exec();
}

bool DatabaseManager::deleteTask(qint64 taskId) {
    if (taskId < 0) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM tasks WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), taskId);
    return q.exec();
}

bool DatabaseManager::deleteDoneTasks(const QString& listId) {
    if (listId.isEmpty()) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM tasks WHERE list_id = :list_id AND done = 1"));
    q.bindValue(QStringLiteral(":list_id"), listId);
    return q.exec();
}

bool DatabaseManager::replaceListTasksOrders(const QString& listId, bool done, const QVector<TxtTask>& tasks) {
    if (listId.isEmpty()) return false;
    QSqlDatabase database = db();
    database.transaction();
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
        if (!q.exec()) {
            database.rollback();
            return false;
        }
    }
    return database.commit();
}

bool DatabaseManager::upsertRemoteIssueTask(const QString& listId,
                                            const QString& issueKey,
                                            const QString& description,
                                            const QString& localStatus,
                                            quint8 priority,
                                            quint32 durationSeconds,
                                            const QString& details,
                                            const QVariantList& comments,
                                            bool* outIsNew,
                                            QString* outOldStatus)
{
    if (outIsNew) *outIsNew = false;
    if (outOldStatus) outOldStatus->clear();
    if (listId.isEmpty() || issueKey.isEmpty()) return false;
    const QString normStatus = localStatus.isEmpty() ? QStringLiteral("todo") : localStatus;
    const bool isDone = TxtTask::isClosedStatus(normStatus);

    QSqlDatabase database = db();
    QSqlQuery check(database);
    check.prepare(QStringLiteral(
        "SELECT id, duration_seconds, details, status, comments_json FROM tasks WHERE list_id = :list_id AND jira_key = :jira_key LIMIT 1"
    ));
    check.bindValue(QStringLiteral(":list_id"), nonNullString(listId));
    check.bindValue(QStringLiteral(":jira_key"), nonNullString(issueKey));
    if (check.exec() && check.next()) {
        const qint64 existingId = check.value(0).toLongLong();
        const quint32 existingDur = check.value(1).toUInt();
        const QString existingDetails = check.value(2).toString();
        const QString prevStatus = check.value(3).toString();
        const QString existingCommentsJson = check.value(4).toString();
        if (outOldStatus) *outOldStatus = prevStatus;
        QSqlQuery upd(database);
        upd.prepare(QStringLiteral(
            "UPDATE tasks SET description = :description, done = :done, status = :status, "
            "priority = :priority, duration_seconds = :duration, details = :details, comments_json = :comments_json "
            "WHERE id = :id"
        ));
        upd.bindValue(QStringLiteral(":description"), nonNullString(description));
        upd.bindValue(QStringLiteral(":done"), isDone ? 1 : 0);
        upd.bindValue(QStringLiteral(":status"), nonNullString(normStatus));
        upd.bindValue(QStringLiteral(":priority"), static_cast<int>(priority));
        upd.bindValue(QStringLiteral(":duration"), durationSeconds > 0 ? durationSeconds : existingDur);
        upd.bindValue(QStringLiteral(":details"), !details.trimmed().isEmpty() ? nonNullString(details) : nonNullString(existingDetails));
        const QString commentsJson = !comments.isEmpty() ? serializeCommentsList(comments) : existingCommentsJson;
        upd.bindValue(QStringLiteral(":comments_json"), nonNullString(commentsJson));
        upd.bindValue(QStringLiteral(":id"), existingId);
        return upd.exec();
    }

    if (outIsNew) *outIsNew = true;

    QSqlQuery maxOrderQ(database);
    maxOrderQ.prepare(QStringLiteral("SELECT COALESCE(MAX(sort_order), -1) + 1 FROM tasks WHERE list_id = :list_id AND done = :done"));
    maxOrderQ.bindValue(QStringLiteral(":list_id"), nonNullString(listId));
    maxOrderQ.bindValue(QStringLiteral(":done"), isDone ? 1 : 0);
    int nextOrder = 0;
    if (maxOrderQ.exec() && maxOrderQ.next()) {
        nextOrder = maxOrderQ.value(0).toInt();
    }

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

qint64 DatabaseManager::insertActivityLog(const QString& listId,
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

bool DatabaseManager::markActivityWorklogSynced(qint64 logId) {
    if (logId < 0) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE activity_log SET worklog_synced = 1 WHERE id = :id"));
    q.bindValue(QStringLiteral(":id"), logId);
    return q.exec();
}

QVector<ActivityRecord> DatabaseManager::loadActivityLogs(const QDateTime& startUtc,
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
    if (!listId.trimmed().isEmpty() && listId != QStringLiteral("__ALL__")) {
        sql += QStringLiteral(" AND list_id = :list_id");
    }
    sql += QStringLiteral(" ORDER BY start_time_utc DESC");

    q.prepare(sql);
    if (startUtc.isValid()) {
        q.bindValue(QStringLiteral(":start_time"), startUtc.toSecsSinceEpoch());
    }
    if (endUtc.isValid()) {
        q.bindValue(QStringLiteral(":end_time"), endUtc.toSecsSinceEpoch());
    }
    if (!listId.trimmed().isEmpty() && listId != QStringLiteral("__ALL__")) {
        q.bindValue(QStringLiteral(":list_id"), listId);
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
