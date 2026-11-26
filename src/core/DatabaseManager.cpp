#include "DatabaseManager.h"
#include "CredentialStore.h"
#include "DatabaseBackup.h"
#include "DatabaseSchema.h"
#include "DatabaseSerialization.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QVariant>

namespace gofi {

DatabaseManager::DatabaseManager()
    : m_listRepo(this)
    , m_taskRepo(this)
    , m_activityRepo(this)
    , m_backup(this)
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
    return DatabaseSchema::createSchema(db());
}

bool DatabaseManager::createSchemaForDb(QSqlDatabase database) {
    return DatabaseSchema::createSchema(database);
}

bool DatabaseManager::relocateDatabase(const QString& newDbPath, bool copyExisting) {
    return m_backup.relocateDatabase(newDbPath, copyExisting);
}

bool DatabaseManager::exportDatabaseBackup(const QString& destinationFilePath) const {
    return m_backup.exportDatabaseBackup(destinationFilePath);
}

bool DatabaseManager::importDatabaseBackup(const QString& sourceFilePath) {
    return m_backup.importDatabaseBackup(sourceFilePath);
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
    return m_listRepo.loadLists();
}

bool DatabaseManager::saveList(const TodoListEntry& entry, int sortOrder) {
    return m_listRepo.saveList(entry, sortOrder);
}

bool DatabaseManager::updateListStatusMapping(const QString& listId, const QVariantMap& mapping) {
    return m_listRepo.updateListStatusMapping(listId, mapping);
}

bool DatabaseManager::removeList(const QString& listId, bool* notFound) {
    return m_listRepo.removeList(listId, notFound);
}

bool DatabaseManager::updateListsOrder(const QVector<TodoListEntry>& entries) {
    return m_listRepo.updateListsOrder(entries);
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

bool DatabaseManager::renameRemoteIssueKey(const QString& listId, const QString& oldKey, const QString& newKey) {
    return m_taskRepo.renameRemoteIssueKey(listId, oldKey, newKey);
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
                                            QString* outOldStatus,
                                            bool replaceDetails,
                                            bool replaceComments)
{
    return m_taskRepo.upsertRemoteIssueTask(listId, issueKey, description, localStatus, priority, durationSeconds, details, comments,
                                            outIsNew, outOldStatus, replaceDetails, replaceComments);
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
    return m_activityRepo.insertActivityLog(listId, listName, taskId, jiraKey, taskDescription, startTimeUtc, runtimeSeconds, stopTimeUtc, worklogSynced, isPomodoroCompleted);
}

bool DatabaseManager::markActivityWorklogSynced(qint64 logId) {
    return m_activityRepo.markActivityWorklogSynced(logId);
}

QVector<ActivityRecord> DatabaseManager::loadActivityLogs(const QDateTime& startUtc,
                                                         const QDateTime& endUtc,
                                                         const QString& listId) const
{
    return m_activityRepo.loadActivityLogs(startUtc, endUtc, listId);
}

} // namespace gofi
