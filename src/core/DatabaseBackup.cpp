#include "DatabaseBackup.h"
#include "DatabaseManager.h"
#include "CredentialStore.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <sys/stat.h>

namespace gofi {

namespace {

struct UmaskGuard {
    mode_t oldUmask;
    explicit UmaskGuard(mode_t mask) : oldUmask(::umask(mask)) {}
    ~UmaskGuard() { ::umask(oldUmask); }
};

} // namespace

DatabaseBackup::DatabaseBackup(DatabaseManager* db)
    : m_dbManager(db)
{
}

QString DatabaseBackup::expandUserPath(const QString& path) {
    const QString p = path.trimmed();
    if (p == QLatin1String("~")) {
        return QDir::homePath();
    }
    if (p.startsWith(QStringLiteral("~/")) || p.startsWith(QStringLiteral("~\\"))) {
        return QDir::homePath() + p.mid(1);
    }
    return p;
}

bool DatabaseBackup::areSameFiles(const QString& pathA, const QString& pathB) {
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

bool DatabaseBackup::isValidGofiDatabase(const QString& filePath) {
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

bool DatabaseBackup::relocateDatabase(const QString& newDbPath, bool copyExisting) {
    auto& dbMgr = m_dbManager ? *m_dbManager : DatabaseManager::instance();
    QString cleanTarget = expandUserPath(newDbPath);
    if (cleanTarget.isEmpty()) return false;
    if (QFileInfo(cleanTarget).isDir()) {
        cleanTarget = QDir(cleanTarget).filePath(QStringLiteral("goforit.db"));
    }
    cleanTarget = QFileInfo(cleanTarget).absoluteFilePath();
    if (areSameFiles(cleanTarget, dbMgr.m_dbPath)) return true;

    QFileInfo(cleanTarget).dir().mkpath(QStringLiteral("."));
    const QString oldPath = dbMgr.m_dbPath;
    QVector<TodoListEntry> existingEntries;

    if (copyExisting && !oldPath.isEmpty() && QFile::exists(oldPath)) {
        if (dbMgr.isOpen()) {
            existingEntries = dbMgr.loadLists();
            QSqlQuery checkpointQ(dbMgr.db());
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

    if (!dbMgr.initialize(cleanTarget)) {
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

bool DatabaseBackup::exportDatabaseBackup(const QString& destinationFilePath) const {
    const auto& dbMgr = m_dbManager ? *m_dbManager : DatabaseManager::instance();
    const QString cleanDest = expandUserPath(destinationFilePath);
    if (cleanDest.isEmpty() || dbMgr.m_dbPath.isEmpty()) return false;
    if (areSameFiles(cleanDest, dbMgr.m_dbPath)) return false;

    QFileInfo destInfo(cleanDest);
    destInfo.dir().mkpath(QStringLiteral("."));

    if (dbMgr.isOpen()) {
        QSqlQuery checkpointQ(dbMgr.db());
        checkpointQ.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE);"));
    }

    const QString tempDest = destInfo.dir().filePath(
        QStringLiteral(".%1.tmp.%2").arg(destInfo.fileName()).arg(QCoreApplication::applicationPid())
    );
    if (QFile::exists(tempDest)) {
        QFile::remove(tempDest);
    }
    if (!QFile::copy(dbMgr.m_dbPath, tempDest)) {
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

bool DatabaseBackup::importDatabaseBackup(const QString& sourceFilePath) {
    auto& dbMgr = m_dbManager ? *m_dbManager : DatabaseManager::instance();
    const QString cleanSrc = expandUserPath(sourceFilePath);
    if (cleanSrc.isEmpty() || !QFile::exists(cleanSrc)) return false;
    if (areSameFiles(cleanSrc, dbMgr.m_dbPath)) return false;

    if (!isValidGofiDatabase(cleanSrc)) {
        return false;
    }

    const QString targetPath = dbMgr.m_dbPath;
    if (targetPath.isEmpty()) return false;

    if (dbMgr.isOpen()) {
        QSqlQuery checkpointQ(dbMgr.db());
        checkpointQ.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE);"));
    }

    if (QSqlDatabase::contains(dbMgr.m_connectionName)) {
        {
            QSqlDatabase oldDb = QSqlDatabase::database(dbMgr.m_connectionName, false);
            if (oldDb.isOpen()) oldDb.close();
        }
        QSqlDatabase::removeDatabase(dbMgr.m_connectionName);
    }
    dbMgr.m_dbPath.clear();

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
            dbMgr.initialize(targetPath);
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
        initOk = dbMgr.initialize(targetPath);
    }

    if (!copyOk || !initOk) {
        if (QSqlDatabase::contains(dbMgr.m_connectionName)) {
            {
                QSqlDatabase cur = QSqlDatabase::database(dbMgr.m_connectionName, false);
                if (cur.isOpen()) cur.close();
            }
            QSqlDatabase::removeDatabase(dbMgr.m_connectionName);
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
        dbMgr.initialize(targetPath);

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

} // namespace gofi
