#include "DatabaseManager.h"
#include "CredentialStore.h"
#include "DatabaseSerialization.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QVariant>
#include <sys/stat.h>

namespace gofi {

namespace {

struct UmaskGuard {
    mode_t oldUmask;
    explicit UmaskGuard(mode_t mask) : oldUmask(::umask(mask)) {}
    ~UmaskGuard() { ::umask(oldUmask); }
};

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

} // namespace

DatabaseManager::DatabaseManager()
    : m_taskRepo(this)
{
}

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
    m_initFailed = false;
    CredentialStore::instance().clearInMemoryTokens();
    CredentialStore::instance().resetPromptState();
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
        m_initFailed = false;
        return true;
    }

    if (targetPath != QStringLiteral(":memory:")) {
        static qint64 s_probeId = 0;
        const QString probeConnName = QStringLiteral("gofi_probe_%1_%2").arg(QCoreApplication::applicationPid()).arg(++s_probeId);
        bool probeOk = false;
        {
            QSqlDatabase probeDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), probeConnName);
            probeDb.setDatabaseName(targetPath);
            if (probeDb.open()) {
                probeOk = createSchemaForDb(probeDb);
                probeDb.close();
            }
        }
        QSqlDatabase::removeDatabase(probeConnName);

        if (!probeOk) {
            m_initFailed = true;
            return false;
        }
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
        m_initFailed = true;
        return false;
    }

    if (m_dbPath != QStringLiteral(":memory:") && QFile::exists(m_dbPath)) {
        QFile::setPermissions(m_dbPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    const bool schemaOk = createSchemaForDb(database);
    if (!schemaOk) {
        m_initFailed = true;
        return false;
    }
    m_initFailed = false;

    if (m_dbPath == QStringLiteral(":memory:")) {
        CredentialStore::instance().setInMemoryMode(true);
    } else {
        CredentialStore::instance().setInMemoryMode(false);
        CredentialStore::instance().setFallbackFilePath(
            QFileInfo(m_dbPath).dir().filePath(QStringLiteral("credentials.json"))
        );
    }
    CredentialStore::instance().migratePlaintextDatabaseTokens();

    return true;
}

bool DatabaseManager::isOpen() const {
    if (!QSqlDatabase::contains(m_connectionName)) {
        return false;
    }
    return QSqlDatabase::database(m_connectionName, false).isOpen();
}

bool DatabaseManager::beginTransaction() {
    return db().transaction();
}

bool DatabaseManager::commitTransaction() {
    return db().commit();
}

bool DatabaseManager::rollbackTransaction() {
    return db().rollback();
}

QSqlDatabase DatabaseManager::db() const {
    if (!isOpen() && !m_initFailed) {
        const_cast<DatabaseManager*>(this)->initialize();
    }
    return QSqlDatabase::database(m_connectionName);
}

bool DatabaseManager::createSchema() {
    return createSchemaForDb(db());
}

bool DatabaseManager::createSchemaForDb(QSqlDatabase database) {
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
    QVector<TodoListEntry> existingEntries;

    if (copyExisting && !oldPath.isEmpty() && QFile::exists(oldPath)) {
        if (isOpen()) {
            existingEntries = loadLists();
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
        if (QFile::exists(cleanTarget + QStringLiteral("-wal"))) {
            QFile::remove(cleanTarget + QStringLiteral("-wal"));
        }
        if (QFile::exists(cleanTarget + QStringLiteral("-shm"))) {
            QFile::remove(cleanTarget + QStringLiteral("-shm"));
        }
        if (!QFile::rename(tempTarget, cleanTarget)) {
            if (!QFile::copy(tempTarget, cleanTarget)) {
                QFile::remove(tempTarget);
                return false;
            }
            QFile::remove(tempTarget);
        }
    }

    if (!initialize(cleanTarget)) {
        return false;
    }

    if (copyExisting && !oldPath.isEmpty()) {
        const QString oldCreds = QFileInfo(oldPath).dir().filePath(QStringLiteral("credentials.json"));
        const QString newCreds = QFileInfo(cleanTarget).dir().filePath(QStringLiteral("credentials.json"));

        if (QFile::exists(oldCreds)) {
            UmaskGuard umaskGuard(0077);
            QFile oldF(oldCreds);
            if (oldF.open(QIODevice::ReadOnly)) {
                const QJsonDocument oldDoc = QJsonDocument::fromJson(oldF.readAll());
                oldF.close();
                if (oldDoc.isObject()) {
                    QJsonObject oldRoot = oldDoc.object();
                    QJsonObject oldTokens = oldRoot.value(QStringLiteral("tokens")).toObject();

                    QJsonObject newRoot;
                    QJsonObject newTokens;
                    if (QFile::exists(newCreds)) {
                        QFile newF(newCreds);
                        if (newF.open(QIODevice::ReadOnly)) {
                            const QJsonDocument newDoc = QJsonDocument::fromJson(newF.readAll());
                            newF.close();
                            if (newDoc.isObject()) {
                                newRoot = newDoc.object();
                                newTokens = newRoot.value(QStringLiteral("tokens")).toObject();
                            }
                        }
                    }

                    const QString oldCanonical = QFileInfo(oldPath).canonicalFilePath();
                    const QString oldAbs = QFileInfo(oldPath).absoluteFilePath();
                    const QString newCanonical = QFileInfo(cleanTarget).canonicalFilePath();
                    const QString targetScope = newCanonical.isEmpty() ? cleanTarget : newCanonical;

                    QVector<QString> keysToRemoveFromOld;
                    for (auto it = oldTokens.begin(); it != oldTokens.end(); ++it) {
                        const QString key = it.key();
                        QString listIdPart;
                        bool belongsToOldDb = false;
                        if (!oldCanonical.isEmpty() && key.startsWith(oldCanonical + QStringLiteral(":"))) {
                            listIdPart = key.mid(oldCanonical.length() + 1);
                            belongsToOldDb = true;
                        } else if (key.startsWith(oldAbs + QStringLiteral(":"))) {
                            listIdPart = key.mid(oldAbs.length() + 1);
                            belongsToOldDb = true;
                        } else if (!key.contains(QStringLiteral(":"))) {
                            listIdPart = key;
                            belongsToOldDb = true;
                        }

                        if (belongsToOldDb) {
                            const QString newKey = QStringLiteral("%1:%2").arg(targetScope, listIdPart);
                            newTokens[newKey] = it.value();
                            keysToRemoveFromOld.append(key);
                        }
                    }

                    if (oldCreds == newCreds) {
                        for (const QString& k : keysToRemoveFromOld) {
                            oldTokens.remove(k);
                        }
                        for (auto it = newTokens.begin(); it != newTokens.end(); ++it) {
                            oldTokens[it.key()] = it.value();
                        }
                        oldRoot[QStringLiteral("version")] = 1;
                        oldRoot[QStringLiteral("tokens")] = oldTokens;

                        QSaveFile saveFile(oldCreds);
                        if (saveFile.open(QIODevice::WriteOnly)) {
                            QFile::setPermissions(saveFile.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                            saveFile.write(QJsonDocument(oldRoot).toJson(QJsonDocument::Indented));
                            saveFile.commit();
                            QFile::setPermissions(oldCreds, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                        }
                    } else {
                        newRoot[QStringLiteral("version")] = 1;
                        newRoot[QStringLiteral("tokens")] = newTokens;

                        QSaveFile saveNew(newCreds);
                        if (saveNew.open(QIODevice::WriteOnly)) {
                            QFile::setPermissions(saveNew.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                            saveNew.write(QJsonDocument(newRoot).toJson(QJsonDocument::Indented));
                            if (saveNew.commit()) {
                                QFile::setPermissions(newCreds, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                                for (const QString& k : keysToRemoveFromOld) {
                                    oldTokens.remove(k);
                                }
                                if (oldTokens.isEmpty()) {
                                    QFile::remove(oldCreds);
                                } else {
                                    oldRoot[QStringLiteral("tokens")] = oldTokens;
                                    QSaveFile saveOld(oldCreds);
                                    if (saveOld.open(QIODevice::WriteOnly)) {
                                        QFile::setPermissions(saveOld.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                                        saveOld.write(QJsonDocument(oldRoot).toJson(QJsonDocument::Indented));
                                        saveOld.commit();
                                        QFile::setPermissions(oldCreds, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        auto& store = CredentialStore::instance();
        for (const auto& entry : existingEntries) {
            if (!entry.apiToken.isEmpty()) {
                store.storeToken(entry.id, entry.apiToken);
            }
        }
    }

    QSettings boot(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("settings"));
    boot.setValue(QStringLiteral("Storage/databasePath"), cleanTarget);
    boot.sync();

    return true;
}

bool DatabaseManager::exportDatabaseBackup(const QString& destinationFilePath) const {
    const QString cleanDest = expandUserPath(destinationFilePath);
    if (cleanDest.isEmpty() || m_dbPath.isEmpty()) return false;
    if (areSameFiles(cleanDest, m_dbPath)) return false;

    QFileInfo destInfo(cleanDest);
    destInfo.dir().mkpath(QStringLiteral("."));

    if (isOpen()) {
        QSqlQuery checkpointQ(db());
        checkpointQ.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE);"));
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
        checkpointQ.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE);"));
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
        e.directoryPath = m_dbPath;
        result.append(std::move(e));
    }
    return result;
}

bool DatabaseManager::saveList(const TodoListEntry& entry, int sortOrder) {
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

bool DatabaseManager::updateListStatusMapping(const QString& listId, const QVariantMap& mapping) {
    if (listId.isEmpty()) return false;
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE lists SET status_mapping = :status_mapping WHERE id = :id"));
    q.bindValue(QStringLiteral(":status_mapping"), nonNullString(serializeStatusMapping(mapping)));
    q.bindValue(QStringLiteral(":id"), listId);
    return q.exec() && q.numRowsAffected() > 0;
}

bool DatabaseManager::removeList(const QString& listId, bool* notFound) {
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

bool DatabaseManager::updateListsOrder(const QVector<TodoListEntry>& entries) {
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

std::optional<TxtTask> DatabaseManager::loadTask(qint64 taskId, bool* queryError) const {
    return m_taskRepo.loadTask(taskId, queryError);
}

QVector<TxtTask> DatabaseManager::loadTasks(const QStringList& listIds, bool done) const {
    return m_taskRepo.loadTasks(listIds, done);
}

QVector<TxtTask> DatabaseManager::loadTasks(const QString& listId, bool done) const {
    return m_taskRepo.loadTasks(listId, done);
}

int DatabaseManager::countAllTasksInLists(const QStringList& listIds) const {
    return m_taskRepo.countAllTasksInLists(listIds);
}

int DatabaseManager::countAllTasksInList(const QString& listId) const {
    return m_taskRepo.countAllTasksInList(listId);
}

bool DatabaseManager::isOnboardingCompleted(const QString& listId) const {
    return m_taskRepo.isOnboardingCompleted(listId);
}

bool DatabaseManager::setOnboardingCompleted(const QString& listId, bool completed) {
    return m_taskRepo.setOnboardingCompleted(listId, completed);
}

qint64 DatabaseManager::insertTask(const QString& listId, const TxtTask& task, int sortOrder) {
    return m_taskRepo.insertTask(listId, task, sortOrder);
}

qint64 DatabaseManager::insertTaskOnTopAndReorder(const QString& listId, const TxtTask& task, const QVector<TxtTask>& existingTasks) {
    return m_taskRepo.insertTaskOnTopAndReorder(listId, task, existingTasks);
}

bool DatabaseManager::updateTask(const TxtTask& task) {
    return m_taskRepo.updateTask(task);
}

bool DatabaseManager::markTaskViewed(qint64 taskId, bool viewed) {
    return m_taskRepo.markTaskViewed(taskId, viewed);
}

bool DatabaseManager::updateTaskDoneAndOrder(const TxtTask& task, int sortOrder) {
    return m_taskRepo.updateTaskDoneAndOrder(task, sortOrder);
}

bool DatabaseManager::moveTaskDoneAndReorder(const TxtTask& task, int newOrder, const QMap<QString, QVector<TxtTask>>& remainingPerList, bool remainingDone) {
    return m_taskRepo.moveTaskDoneAndReorder(task, newOrder, remainingPerList, remainingDone);
}

bool DatabaseManager::updateTaskTimer(qint64 taskId, quint32 timerSeconds, quint32 jiraSyncedSeconds) {
    return m_taskRepo.updateTaskTimer(taskId, timerSeconds, jiraSyncedSeconds);
}

bool DatabaseManager::deleteTask(qint64 taskId) {
    return m_taskRepo.deleteTask(taskId);
}

bool DatabaseManager::deleteTaskAndReorder(qint64 taskId, const QString& listId, bool done, const QVector<TxtTask>& remainingTasks) {
    return m_taskRepo.deleteTaskAndReorder(taskId, listId, done, remainingTasks);
}

bool DatabaseManager::deleteDoneTasks(const QString& listId) {
    return m_taskRepo.deleteDoneTasks(listId);
}

bool DatabaseManager::deleteDoneTasks(const QStringList& listIds) {
    return m_taskRepo.deleteDoneTasks(listIds);
}

bool DatabaseManager::replaceListTasksOrders(const QString& listId, bool done, const QVector<TxtTask>& tasks) {
    return m_taskRepo.replaceListTasksOrders(listId, done, tasks);
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
    return m_taskRepo.upsertRemoteIssueTask(listId, issueKey, description, localStatus, priority, durationSeconds, details, comments, outIsNew, outOldStatus);
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
    return q.exec() && q.numRowsAffected() > 0;
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
