#include "TestSupport.h"

#include "core/AppController.h"
#include "core/CredentialStore.h"
#include "core/DatabaseManager.h"
#include "core/ListManagerModel.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QtTest>

using namespace gofi;

class TestCredentials : public QObject {
    Q_OBJECT

private:
    TestSandbox m_testSandbox;

private slots:
    void initTestCase() {
        QVERIFY(m_testSandbox.init());
        QSettings probe(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("go-for-it"));
        QVERIFY(probe.fileName().startsWith(m_testSandbox.path()));
    }

    void cleanupTestCase() {
        if (m_testSandbox.isValid()) {
            m_testSandbox.remove();
        }
    }

    void cleanup() {
        DatabaseManager::instance().close();
        SystemHookService::setSystemIntegrationEnabled(false);
    }

    void testCredentialStoreFallbackFileOperationsAndPermissions() {
        const QString credsPath = m_testSandbox.filePath(QStringLiteral("test_creds.json"));
        QFile::remove(credsPath);

        auto& store = gofi::CredentialStore::instance();
        store.setInMemoryMode(false);
        store.setFallbackFilePath(credsPath);

        // Store tokens
        QVERIFY(store.storeToken(QStringLiteral("list_gh"), QStringLiteral("ghp_test123456789")));
        QVERIFY(store.storeToken(QStringLiteral("list_jira"), QStringLiteral("jira_secret_token")));

        // Verify file exists
        QVERIFY(QFile::exists(credsPath));

        // Verify strict 0600 file permissions (read/write owner only, no group or other access)
        const QFile::Permissions perms = QFile::permissions(credsPath);
        QVERIFY(perms & QFileDevice::ReadOwner);
        QVERIFY(perms & QFileDevice::WriteOwner);
        QVERIFY(!(perms & QFileDevice::ReadGroup));
        QVERIFY(!(perms & QFileDevice::WriteGroup));
        QVERIFY(!(perms & QFileDevice::ReadOther));
        QVERIFY(!(perms & QFileDevice::WriteOther));

        // Load tokens
        QCOMPARE(store.loadToken(QStringLiteral("list_gh")), QStringLiteral("ghp_test123456789"));
        QCOMPARE(store.loadToken(QStringLiteral("list_jira")), QStringLiteral("jira_secret_token"));

        // Verify JSON document structure
        {
            QFile f(credsPath);
            QVERIFY(f.open(QIODevice::ReadOnly));
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            f.close();
            QVERIFY(doc.isObject());
            QCOMPARE(doc.object().value(QStringLiteral("version")).toInt(), 1);
            const QJsonObject tokens = doc.object().value(QStringLiteral("tokens")).toObject();
            QCOMPARE(tokens.value(QStringLiteral("list_gh")).toString(), QStringLiteral("ghp_test123456789"));
            QCOMPARE(tokens.value(QStringLiteral("list_jira")).toString(), QStringLiteral("jira_secret_token"));
        }

        // Update token
        QVERIFY(store.storeToken(QStringLiteral("list_gh"), QStringLiteral("ghp_updated987654321")));
        QCOMPARE(store.loadToken(QStringLiteral("list_gh")), QStringLiteral("ghp_updated987654321"));

        // Delete token
        QVERIFY(store.deleteToken(QStringLiteral("list_jira")));
        QVERIFY(store.loadToken(QStringLiteral("list_jira")).isEmpty());
        QCOMPARE(store.loadToken(QStringLiteral("list_gh")), QStringLiteral("ghp_updated987654321"));

        QVERIFY(store.deleteToken(QStringLiteral("list_gh")));
        QVERIFY(store.loadToken(QStringLiteral("list_gh")).isEmpty());

        store.setFallbackFilePath(QString());
        QFile::remove(credsPath);
    }

    void testDatabasePlaintextTokenMigrationAndStorageSafety() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("legacy_token_migration.db"));
        const QString credsPath = m_testSandbox.filePath(QStringLiteral("credentials.json"));
        const QString backupPath = m_testSandbox.filePath(QStringLiteral("token_safety_backup.db"));
        QFile::remove(dbPath);
        QFile::remove(credsPath);
        QFile::remove(backupPath);

        // Step 1: Pre-populate database with an unmigrated row having plaintext api_token
        {
            QSqlDatabase rawDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy_pop"));
            rawDb.setDatabaseName(dbPath);
            QVERIFY(rawDb.open());
            QSqlQuery q(rawDb);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE lists (id TEXT PRIMARY KEY, name TEXT NOT NULL, provider TEXT NOT NULL, sort_order INTEGER NOT NULL DEFAULT 0, activity_log_path TEXT NOT NULL DEFAULT '', add_creation_dates INTEGER NOT NULL DEFAULT 1, reminder_time INTEGER NOT NULL DEFAULT -1, custom_schedule TEXT NOT NULL DEFAULT '', jira_url TEXT NOT NULL DEFAULT '', jira_user TEXT NOT NULL DEFAULT '', jira_jql TEXT NOT NULL DEFAULT '', api_token TEXT NOT NULL DEFAULT '', auto_worklog INTEGER NOT NULL DEFAULT 1, status_mapping TEXT NOT NULL DEFAULT '', priority_mapping TEXT NOT NULL DEFAULT '', sync_status_to_remote INTEGER NOT NULL DEFAULT 0);")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO lists (id, name, provider, api_token) VALUES ('legacy_list_1', 'Jira Work', 'Jira', 'super-secret-pat-12345');")));
            rawDb.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("legacy_pop"));

        // Step 2: Initialize AppController / DatabaseManager on this database.
        // It should automatically migrate the plaintext token into CredentialStore and clear it from SQLite.
        {
            AppController ctrl(QString(), dbPath);
            QCOMPARE(ctrl.listManager()->rowCount(), 1);
            QCOMPARE(ctrl.listManager()->getListConfig(QStringLiteral("legacy_list_1")).value(QStringLiteral("apiToken")).toString(), QStringLiteral("super-secret-pat-12345"));

            // Verify raw SQLite row now has EMPTY api_token!
            {
                const QSqlDatabase conn = QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn"));
                QSqlQuery checkQ(conn);
                QVERIFY(checkQ.exec(QStringLiteral("SELECT api_token FROM lists WHERE id = 'legacy_list_1'")));
                QVERIFY(checkQ.next());
                QCOMPARE(checkQ.value(0).toString(), QString());
            }

            // Save an updated configuration with a new token
            QVariantMap cfg = ctrl.listManager()->getListConfig(QStringLiteral("legacy_list_1"));
            cfg[QStringLiteral("apiToken")] = QStringLiteral("rotated-secret-67890");
            ctrl.listManager()->saveListConfig(cfg);

            // Verify it is updated in model
            QCOMPARE(ctrl.listManager()->getListConfig(QStringLiteral("legacy_list_1")).value(QStringLiteral("apiToken")).toString(), QStringLiteral("rotated-secret-67890"));

            // Verify raw SQLite row STILL has EMPTY api_token!
            {
                const QSqlDatabase conn = QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn"));
                QSqlQuery checkQ(conn);
                QVERIFY(checkQ.exec(QStringLiteral("SELECT api_token FROM lists WHERE id = 'legacy_list_1'")));
                QVERIFY(checkQ.next());
                QCOMPARE(checkQ.value(0).toString(), QString());
            }

            // Step 3: Export backup and verify the exported database has NO plain-text credentials
            QVERIFY(DatabaseManager::instance().exportDatabaseBackup(backupPath));
        }
        DatabaseManager::instance().close();

        // Check exported backup database directly
        {
            QSqlDatabase backupDb = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("check_backup"));
            backupDb.setDatabaseName(backupPath);
            QVERIFY(backupDb.open());
            QSqlQuery backupQ(backupDb);
            QVERIFY(backupQ.exec(QStringLiteral("SELECT api_token FROM lists WHERE id = 'legacy_list_1'")));
            QVERIFY(backupQ.next());
            QCOMPARE(backupQ.value(0).toString(), QString());
            backupDb.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("check_backup"));

        // Check credentials fallback file exists and has 0600 permissions
        QVERIFY(QFile::exists(credsPath));
        const QFile::Permissions perms = QFile::permissions(credsPath);
        QVERIFY(perms & QFileDevice::ReadOwner);
        QVERIFY(perms & QFileDevice::WriteOwner);
        QVERIFY(!(perms & QFileDevice::ReadOther));

        QFile::remove(dbPath);
        QFile::remove(credsPath);
        QFile::remove(backupPath);
        gofi::CredentialStore::instance().setFallbackFilePath(QString());
    }

    void testCredentialStoreKeyringAndMockSecretService() {
        PrivateDBus bus;
        if (bus.address.isEmpty()) {
            QSKIP("dbus-daemon is not available");
        }
        const QString fakeName = QStringLiteral("cs_fake_secrets");
        const QString clientName = QStringLiteral("cs_client");
        const QString credsPath = m_testSandbox.filePath(QStringLiteral("cs_keyring_fallback.json"));
        QFile::remove(credsPath);

        {
            QDBusConnection fakeConn = QDBusConnection::connectToBus(bus.address, fakeName);
            QDBusConnection clientConn = QDBusConnection::connectToBus(bus.address, clientName);
            QVERIFY(fakeConn.isConnected());
            QVERIFY(clientConn.isConnected());

            const QString service = QStringLiteral("org.freedesktop.secrets");
            FakeSecretService fakeSecrets;
            QVERIFY(fakeConn.registerService(service));
            QVERIFY(fakeConn.registerVirtualObject(QStringLiteral("/org/freedesktop/secrets"), &fakeSecrets, QDBusConnection::SubPath));

            auto& store = gofi::CredentialStore::instance();
            store.setInMemoryMode(false);
            store.setFallbackFilePath(credsPath);
            store.setDBusConnection(clientConn, service);

            // 1. Verify Keyring is reported as available
            QVERIFY(store.isKeyringAvailable());

            // 2. Store token into Keyring
            QVERIFY(store.storeToken(QStringLiteral("cs_keyring_list"), QStringLiteral("pat_secret_abc123")));
            QCOMPARE(fakeSecrets.items.size(), 1);

            // 3. Token is retrieved from Keyring
            QCOMPARE(store.loadToken(QStringLiteral("cs_keyring_list")), QStringLiteral("pat_secret_abc123"));

            // 4. Delete token from Keyring
            QVERIFY(store.deleteToken(QStringLiteral("cs_keyring_list")));
            QCOMPARE(fakeSecrets.items.size(), 0);
            QVERIFY(store.loadToken(QStringLiteral("cs_keyring_list")).isEmpty());

            store.resetDBusConnection();
            fakeConn.unregisterObject(QStringLiteral("/org/freedesktop/secrets"), QDBusConnection::UnregisterTree);
            fakeConn.unregisterService(service);
        }
        QDBusConnection::disconnectFromBus(fakeName);
        QDBusConnection::disconnectFromBus(clientName);
        gofi::CredentialStore::instance().setFallbackFilePath(QString());
        QFile::remove(credsPath);
    }

    void testCredentialStoreDoesNotUseSessionKeyringInTestMode() {
        // Tests must never read or write the user's real keyring (KWallet / GNOME Keyring) on the session bus.
        auto& store = gofi::CredentialStore::instance();
        store.resetDBusConnection();
        QVERIFY2(!store.isKeyringAvailable(),
                 "CredentialStore reports the session-bus Secret Service as available in test mode");
    }

    void testCredentialStoreUnlocksLockedKeyringItems() {
        PrivateDBus bus;
        if (bus.address.isEmpty()) {
            QSKIP("dbus-daemon is not available");
        }
        const QString fakeName = QStringLiteral("cs_locked_fake");
        const QString clientName = QStringLiteral("cs_locked_client");
        const QString credsPath = m_testSandbox.filePath(QStringLiteral("cs_locked_fallback.json"));
        QFile::remove(credsPath);
        {
            QDBusConnection fakeConn = QDBusConnection::connectToBus(bus.address, fakeName);
            QDBusConnection clientConn = QDBusConnection::connectToBus(bus.address, clientName);
            QVERIFY(fakeConn.isConnected());
            QVERIFY(clientConn.isConnected());
            const QString service = QStringLiteral("org.freedesktop.secrets");
            FakeSecretService fakeSecrets;
            QVERIFY(fakeConn.registerService(service));
            QVERIFY(fakeConn.registerVirtualObject(QStringLiteral("/org/freedesktop/secrets"), &fakeSecrets, QDBusConnection::SubPath));

            auto& store = gofi::CredentialStore::instance();
            store.setInMemoryMode(false);
            store.setFallbackFilePath(credsPath);
            store.setDBusConnection(clientConn, service);

            // A token stored in an earlier session; the collection is locked now (e.g. wallet not opened yet).
            QVERIFY(store.storeToken(QStringLiteral("cs_locked_list"), QStringLiteral("pat_locked_secret")));
            QCOMPARE(fakeSecrets.items.size(), 1);
            fakeSecrets.lockedPaths.insert(fakeSecrets.items.firstKey());

            const QString loaded = store.loadToken(QStringLiteral("cs_locked_list"));
            const int unlockCalls = fakeSecrets.unlockCalls;

            store.resetDBusConnection();
            fakeConn.unregisterObject(QStringLiteral("/org/freedesktop/secrets"), QDBusConnection::UnregisterTree);
            fakeConn.unregisterService(service);

            QCOMPARE(unlockCalls, 1);
            QCOMPARE(loaded, QStringLiteral("pat_locked_secret"));
        }
        QDBusConnection::disconnectFromBus(fakeName);
        QDBusConnection::disconnectFromBus(clientName);
        gofi::CredentialStore::instance().setFallbackFilePath(QString());
        QFile::remove(credsPath);
    }

    void testRemovingListInDatabaseCopyKeepsOriginalToken() {
        // A copy of the database (`--db` experiment, imported backup, relocated copy) has the same list ids.
        // Removing a list there must not delete the original database's token, neither in the keyring nor in
        // the per-directory credentials.json fallback.
        PrivateDBus bus;
        if (bus.address.isEmpty()) {
            QSKIP("dbus-daemon is not available");
        }
        const QString fakeName = QStringLiteral("cs_copy_fake");
        const QString clientName = QStringLiteral("cs_copy_client");
        QTemporaryDir dir(m_testSandbox.filePath(QStringLiteral("cs_copy_XXXXXX")));
        QVERIFY(dir.isValid());
        const QString originalDb = dir.filePath(QStringLiteral("original.db"));
        const QString copyDb = dir.filePath(QStringLiteral("copy.db"));
        const QString listId = QStringLiteral("cs_shared_list");

        auto run = [&](bool withKeyring, const QString& token) -> QString {
            QFile::remove(originalDb);
            QFile::remove(copyDb);
            QFile::remove(dir.filePath(QStringLiteral("credentials.json")));
            QDBusConnection fakeConn = QDBusConnection::connectToBus(bus.address, fakeName);
            QDBusConnection clientConn = QDBusConnection::connectToBus(bus.address, clientName);
            const QString service = QStringLiteral("org.freedesktop.secrets");
            FakeSecretService fakeSecrets;
            if (withKeyring) {
                fakeConn.registerService(service);
                fakeConn.registerVirtualObject(QStringLiteral("/org/freedesktop/secrets"), &fakeSecrets, QDBusConnection::SubPath);
            }
            auto& store = gofi::CredentialStore::instance();
            store.setDBusConnection(clientConn, service);

            auto& db = DatabaseManager::instance();
            QString loaded = QStringLiteral("<not loaded>");
            if (db.initialize(originalDb)) {
                gofi::TodoListEntry entry;
                entry.id = listId;
                entry.name = QStringLiteral("Tracker");
                entry.provider = QStringLiteral("GitHub");
                entry.jiraUrl = QStringLiteral("https://example.invalid");
                entry.apiToken = token;
                db.saveList(entry, 0);
                db.close();
                QFile::copy(originalDb, copyDb);

                if (db.initialize(copyDb)) {
                    db.removeList(listId);
                    db.close();
                }
                if (db.initialize(originalDb)) {
                    loaded.clear();
                    for (const auto& e : db.loadLists()) {
                        if (e.id == listId) {
                            loaded = e.apiToken;
                        }
                    }
                    db.close();
                }
            }
            store.resetDBusConnection();
            store.setFallbackFilePath(QString());
            if (withKeyring) {
                fakeConn.unregisterObject(QStringLiteral("/org/freedesktop/secrets"), QDBusConnection::UnregisterTree);
                fakeConn.unregisterService(service);
            }
            QDBusConnection::disconnectFromBus(fakeName);
            QDBusConnection::disconnectFromBus(clientName);
            return loaded;
        };

        const QString keyringToken = run(true, QStringLiteral("pat_keyring_original"));
        const QString fileToken = run(false, QStringLiteral("pat_file_original"));
        QCOMPARE(keyringToken, QStringLiteral("pat_keyring_original"));
        QCOMPARE(fileToken, QStringLiteral("pat_file_original"));
    }

    void testCredentialStoreCompletesUnlockPromptForLockedItems() {
        // A locked KWallet / GNOME Keyring collection is unlocked through a prompt: Unlock returns no paths and a
        // prompt object; the item becomes readable only after Prompt() completes.
        PrivateDBus bus;
        if (bus.address.isEmpty()) {
            QSKIP("dbus-daemon is not available");
        }
        const QString fakeName = QStringLiteral("cs_prompt_fake");
        const QString clientName = QStringLiteral("cs_prompt_client");
        const QString credsPath = m_testSandbox.filePath(QStringLiteral("cs_prompt_fallback.json"));
        QFile::remove(credsPath);
        QString loaded;
        int unlockCalls = 0;
        {
            QDBusConnection fakeConn = QDBusConnection::connectToBus(bus.address, fakeName);
            QDBusConnection clientConn = QDBusConnection::connectToBus(bus.address, clientName);
            QVERIFY(fakeConn.isConnected());
            QVERIFY(clientConn.isConnected());
            const QString service = QStringLiteral("org.freedesktop.secrets");
            FakeSecretService fakeSecrets;
            QVERIFY(fakeConn.registerService(service));
            QVERIFY(fakeConn.registerVirtualObject(QStringLiteral("/org/freedesktop/secrets"), &fakeSecrets, QDBusConnection::SubPath));

            auto& store = gofi::CredentialStore::instance();
            store.setInMemoryMode(false);
            store.setFallbackFilePath(credsPath);
            store.setDBusConnection(clientConn, service);

            QVERIFY(store.storeToken(QStringLiteral("cs_prompt_list"), QStringLiteral("pat_prompt_secret")));
            QCOMPARE(fakeSecrets.items.size(), 1);
            fakeSecrets.lockedPaths.insert(fakeSecrets.items.firstKey());
            fakeSecrets.unlockNeedsPrompt = true;

            loaded = store.loadToken(QStringLiteral("cs_prompt_list"));
            unlockCalls = fakeSecrets.unlockCalls;

            store.resetDBusConnection();
            fakeConn.unregisterObject(QStringLiteral("/org/freedesktop/secrets"), QDBusConnection::UnregisterTree);
            fakeConn.unregisterService(service);
        }
        QDBusConnection::disconnectFromBus(fakeName);
        QDBusConnection::disconnectFromBus(clientName);
        gofi::CredentialStore::instance().setFallbackFilePath(QString());
        QFile::remove(credsPath);

        QCOMPARE(unlockCalls, 1);
        QCOMPARE(loaded, QStringLiteral("pat_prompt_secret"));
    }

    void testClearedTokenStaysClearedDespiteUnscopedEntries() {
        // Clearing a list's token (saveList with an empty token) must stick. Unscoped entries -- written before
        // tokens were scoped to a database, or another database's scoped keyring item with the same list id --
        // must not bring it back on the next load.
        PrivateDBus bus;
        if (bus.address.isEmpty()) {
            QSKIP("dbus-daemon is not available");
        }
        const QString fakeName = QStringLiteral("cs_clear_fake");
        const QString clientName = QStringLiteral("cs_clear_client");
        QTemporaryDir dir(m_testSandbox.filePath(QStringLiteral("cs_clear_XXXXXX")));
        QVERIFY(dir.isValid());
        const QString listId = QStringLiteral("cs_clear_list");
        const QString service = QStringLiteral("org.freedesktop.secrets");
        auto& db = DatabaseManager::instance();
        auto& store = gofi::CredentialStore::instance();

        auto makeEntry = [&](const QString& token) {
            TodoListEntry entry;
            entry.id = listId;
            entry.name = QStringLiteral("Tracker");
            entry.provider = QStringLiteral("GitHub");
            entry.jiraUrl = QStringLiteral("https://example.invalid");
            entry.apiToken = token;
            return entry;
        };
        auto tokenIn = [&](const QString& dbPath) {
            QString t = QStringLiteral("<not loaded>");
            if (db.initialize(dbPath)) {
                for (const auto& e : db.loadLists()) {
                    if (e.id == listId) {
                        t = e.apiToken;
                    }
                }
                db.close();
            }
            return t;
        };

        // 1. Fallback file with an unscoped key (format written by 75db016..217dc9e), no keyring.
        QString fileAfterClear;
        {
            const QString dbPath = dir.filePath(QStringLiteral("file.db"));
            QDBusConnection clientConn = QDBusConnection::connectToBus(bus.address, clientName);
            store.setDBusConnection(clientConn, service);
            if (db.initialize(dbPath)) {
                db.saveList(makeEntry(QString()), 0);
                db.close();
            }
            {
                QFile f(dir.filePath(QStringLiteral("credentials.json")));
                QVERIFY(f.open(QIODevice::WriteOnly));
                f.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                    {QStringLiteral("tokens"), QJsonObject{{listId, QStringLiteral("pat_unscoped_file")}}}}).toJson());
            }
            QCOMPARE(tokenIn(dbPath), QStringLiteral("pat_unscoped_file"));
            if (db.initialize(dbPath)) {
                db.saveList(makeEntry(QString()), 0);  // user clears the token in the list settings
                db.close();
            }
            fileAfterClear = tokenIn(dbPath);
            store.resetDBusConnection();
            store.setFallbackFilePath(QString());
            QDBusConnection::disconnectFromBus(clientName);
        }

        // 2. Keyring: a copy of the database clears the token of the shared list id.
        QString copyAfterClear;
        QString originalAfterClear;
        {
            const QString originalDb = dir.filePath(QStringLiteral("original.db"));
            const QString copyDb = dir.filePath(QStringLiteral("copy.db"));
            QDBusConnection fakeConn = QDBusConnection::connectToBus(bus.address, fakeName);
            QDBusConnection clientConn = QDBusConnection::connectToBus(bus.address, clientName);
            FakeSecretService fakeSecrets;
            QVERIFY(fakeConn.registerService(service));
            QVERIFY(fakeConn.registerVirtualObject(QStringLiteral("/org/freedesktop/secrets"), &fakeSecrets, QDBusConnection::SubPath));
            store.setDBusConnection(clientConn, service);
            if (db.initialize(originalDb)) {
                db.saveList(makeEntry(QStringLiteral("pat_original_keyring")), 0);
                db.close();
            }
            QFile::copy(originalDb, copyDb);
            if (db.initialize(copyDb)) {
                db.saveList(makeEntry(QString()), 0);  // clear the token in the copy
                db.close();
            }
            copyAfterClear = tokenIn(copyDb);
            originalAfterClear = tokenIn(originalDb);
            store.resetDBusConnection();
            store.setFallbackFilePath(QString());
            fakeConn.unregisterObject(QStringLiteral("/org/freedesktop/secrets"), QDBusConnection::UnregisterTree);
            fakeConn.unregisterService(service);
            QDBusConnection::disconnectFromBus(fakeName);
            QDBusConnection::disconnectFromBus(clientName);
        }

        QCOMPARE(fileAfterClear, QString());
        QCOMPARE(originalAfterClear, QStringLiteral("pat_original_keyring"));
        QCOMPARE(copyAfterClear, QString());
    }

    void testUnscopedFallbackTokenLeavesFileAfterKeyringMigration() {
        // When the keyring becomes available, a token found in credentials.json is moved into the keyring.
        // The plaintext copy must then be gone from the file, also for an unscoped (pre-a67fbc6) key.
        PrivateDBus bus;
        if (bus.address.isEmpty()) {
            QSKIP("dbus-daemon is not available");
        }
        const QString fakeName = QStringLiteral("cs_mig_fake");
        const QString clientName = QStringLiteral("cs_mig_client");
        QTemporaryDir dir(m_testSandbox.filePath(QStringLiteral("cs_mig_XXXXXX")));
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath(QStringLiteral("mig.db"));
        const QString credsPath = dir.filePath(QStringLiteral("credentials.json"));
        const QString listId = QStringLiteral("cs_mig_list");
        const QString service = QStringLiteral("org.freedesktop.secrets");
        auto& db = DatabaseManager::instance();
        auto& store = gofi::CredentialStore::instance();

        QDBusConnection fakeConn = QDBusConnection::connectToBus(bus.address, fakeName);
        QDBusConnection clientConn = QDBusConnection::connectToBus(bus.address, clientName);
        FakeSecretService fakeSecrets;
        QVERIFY(fakeConn.registerService(service));
        QVERIFY(fakeConn.registerVirtualObject(QStringLiteral("/org/freedesktop/secrets"), &fakeSecrets, QDBusConnection::SubPath));
        store.setDBusConnection(clientConn, service);

        QString loaded;
        if (db.initialize(dbPath)) {
            TodoListEntry entry;
            entry.id = listId;
            entry.name = QStringLiteral("Tracker");
            entry.provider = QStringLiteral("GitHub");
            entry.jiraUrl = QStringLiteral("https://example.invalid");
            db.saveList(entry, 0);
            {
                QFile f(credsPath);
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                        {QStringLiteral("tokens"), QJsonObject{{listId, QStringLiteral("pat_unscoped_plain")}}}}).toJson());
                }
            }
            loaded = store.loadToken(listId);
            db.close();
        }
        const int keyringItems = fakeSecrets.items.size();
        store.resetDBusConnection();
        store.setFallbackFilePath(QString());
        fakeConn.unregisterObject(QStringLiteral("/org/freedesktop/secrets"), QDBusConnection::UnregisterTree);
        fakeConn.unregisterService(service);
        QDBusConnection::disconnectFromBus(fakeName);
        QDBusConnection::disconnectFromBus(clientName);

        QCOMPARE(loaded, QStringLiteral("pat_unscoped_plain"));
        QCOMPARE(keyringItems, 1);
        QFile f(credsPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY2(!f.readAll().contains("pat_unscoped_plain"), "token moved to the keyring is still in plaintext in credentials.json");
    }

    void testCredentialStoreWaitsForUserToAnswerUnlockPrompt() {
        // Typing a wallet password takes a human several seconds. A prompt answered after 8 s must still give
        // the token instead of an empty one (which also leads to deleting it on the next settings save).
        PrivateDBus bus;
        if (bus.address.isEmpty()) {
            QSKIP("dbus-daemon is not available");
        }
        const QString fakeName = QStringLiteral("cs_slowprompt_fake");
        const QString clientName = QStringLiteral("cs_slowprompt_client");
        const QString credsPath = m_testSandbox.filePath(QStringLiteral("cs_slowprompt_fallback.json"));
        QFile::remove(credsPath);
        QString loaded;
        {
            QDBusConnection fakeConn = QDBusConnection::connectToBus(bus.address, fakeName);
            QDBusConnection clientConn = QDBusConnection::connectToBus(bus.address, clientName);
            QVERIFY(fakeConn.isConnected());
            QVERIFY(clientConn.isConnected());
            const QString service = QStringLiteral("org.freedesktop.secrets");
            FakeSecretService fakeSecrets;
            QVERIFY(fakeConn.registerService(service));
            QVERIFY(fakeConn.registerVirtualObject(QStringLiteral("/org/freedesktop/secrets"), &fakeSecrets, QDBusConnection::SubPath));

            auto& store = gofi::CredentialStore::instance();
            store.setInMemoryMode(false);
            store.setFallbackFilePath(credsPath);
            store.setDBusConnection(clientConn, service);

            QVERIFY(store.storeToken(QStringLiteral("cs_slowprompt_list"), QStringLiteral("pat_slow_secret")));
            fakeSecrets.lockedPaths.insert(fakeSecrets.items.firstKey());
            fakeSecrets.unlockNeedsPrompt = true;
            fakeSecrets.promptDelayMs = 8000;

            loaded = store.loadToken(QStringLiteral("cs_slowprompt_list"));
            QTRY_VERIFY_WITH_TIMEOUT(fakeSecrets.lockedPaths.isEmpty(), 10000);  // let the delayed reply finish

            store.resetDBusConnection();
            fakeConn.unregisterObject(QStringLiteral("/org/freedesktop/secrets"), QDBusConnection::UnregisterTree);
            fakeConn.unregisterService(service);
        }
        QDBusConnection::disconnectFromBus(fakeName);
        QDBusConnection::disconnectFromBus(clientName);
        gofi::CredentialStore::instance().setFallbackFilePath(QString());
        QFile::remove(credsPath);

        QCOMPARE(loaded, QStringLiteral("pat_slow_secret"));
    }

    void testTokenChangedWhileKeyringLockedWinsAfterUnlock() {
        // The user dismisses the wallet password dialog, sees an empty token and enters a new one (it goes to
        // the fallback file because the locked collection refuses CreateItem). In a later session the wallet is
        // unlocked: the new token must be used, not the stale one still stored in the keyring.
        PrivateDBus bus;
        if (bus.address.isEmpty()) {
            QSKIP("dbus-daemon is not available");
        }
        const QString fakeName = QStringLiteral("cs_rotate_fake");
        const QString clientName = QStringLiteral("cs_rotate_client");
        const QString credsPath = m_testSandbox.filePath(QStringLiteral("cs_rotate_fallback.json"));
        const QString listId = QStringLiteral("cs_rotate_list");
        QFile::remove(credsPath);
        QString lockedLoad;
        bool storedWhileLocked = false;
        QString afterUnlock;
        {
            QDBusConnection fakeConn = QDBusConnection::connectToBus(bus.address, fakeName);
            QDBusConnection clientConn = QDBusConnection::connectToBus(bus.address, clientName);
            QVERIFY(fakeConn.isConnected());
            QVERIFY(clientConn.isConnected());
            const QString service = QStringLiteral("org.freedesktop.secrets");
            FakeSecretService fakeSecrets;
            QVERIFY(fakeConn.registerService(service));
            QVERIFY(fakeConn.registerVirtualObject(QStringLiteral("/org/freedesktop/secrets"), &fakeSecrets, QDBusConnection::SubPath));

            auto& store = gofi::CredentialStore::instance();
            store.setInMemoryMode(false);
            store.setFallbackFilePath(credsPath);
            store.setDBusConnection(clientConn, service);

            QVERIFY(store.storeToken(listId, QStringLiteral("pat_old_revoked")));
            QCOMPARE(fakeSecrets.items.size(), 1);

            // Session 1: wallet locked, the user dismisses the dialog and enters a new token.
            fakeSecrets.lockedPaths.insert(fakeSecrets.items.firstKey());
            fakeSecrets.unlockNeedsPrompt = true;
            fakeSecrets.promptDismisses = true;
            fakeSecrets.createNeedsPrompt = true;
            lockedLoad = store.loadToken(listId);
            storedWhileLocked = store.storeToken(listId, QStringLiteral("pat_new_current"));

            // Session 2: the wallet is open.
            store.resetPromptState();
            fakeSecrets.lockedPaths.clear();
            fakeSecrets.unlockNeedsPrompt = false;
            fakeSecrets.promptDismisses = false;
            fakeSecrets.createNeedsPrompt = false;
            afterUnlock = store.loadToken(listId);

            store.resetDBusConnection();
            fakeConn.unregisterObject(QStringLiteral("/org/freedesktop/secrets"), QDBusConnection::UnregisterTree);
            fakeConn.unregisterService(service);
        }
        QDBusConnection::disconnectFromBus(fakeName);
        QDBusConnection::disconnectFromBus(clientName);
        gofi::CredentialStore::instance().setFallbackFilePath(QString());
        QFile::remove(credsPath);

        QCOMPARE(lockedLoad, QString());
        QVERIFY(storedWhileLocked);
        QCOMPARE(afterUnlock, QStringLiteral("pat_new_current"));
    }

    // ListRepository::removeList removed the token before running DELETE FROM lists. When the DELETE
    // failed (locked or read-only database, constraint), the list stayed but its token was gone, so the
    // next sync failed with an authentication error and the user had to re-enter the token.
    void testFailedListRemovalKeepsToken() {
        QTemporaryDir dir(m_testSandbox.filePath(QStringLiteral("cs_failed_remove_XXXXXX")));
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath(QStringLiteral("failed_remove.db"));
        const QString listId = QStringLiteral("cs_failed_remove_list");

        auto& store = gofi::CredentialStore::instance();
        store.setInMemoryMode(false);
        store.setFallbackFilePath(dir.filePath(QStringLiteral("credentials.json")));

        auto& db = DatabaseManager::instance();
        QVERIFY(db.initialize(dbPath));
        gofi::TodoListEntry entry;
        entry.id = listId;
        entry.name = QStringLiteral("Failed Remove");
        entry.provider = QStringLiteral("GitHub");
        entry.jiraUrl = QStringLiteral("https://example.invalid");
        entry.apiToken = QStringLiteral("pat_keep_me");
        QVERIFY(db.saveList(entry, 0));
        QCOMPARE(store.loadToken(listId), QStringLiteral("pat_keep_me"));

        QSqlQuery trig(QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn")));
        QVERIFY(trig.exec(QStringLiteral(
            "CREATE TRIGGER fail_list_delete BEFORE DELETE ON lists "
            "BEGIN SELECT RAISE(ABORT, 'Simulated list delete failure'); END;")));

        QVERIFY(!db.removeList(listId));
        const auto lists = db.loadLists();
        QCOMPARE(lists.size(), 1);
        QCOMPARE(lists.first().apiToken, QStringLiteral("pat_keep_me"));
        db.close();
        store.setFallbackFilePath(QString());
    }

    void testRemovingListWhileKeyringLockedPurgesOrphanAfterUnlock() {
        // When a list is removed while the wallet/keyring is locked and the unlock prompt was dismissed,
        // deleteFromKeyring cannot unlock/delete the item immediately. When the keyring is unlocked
        // in a subsequent session or after unlock, the orphaned secret item must be cleanly purged.
        PrivateDBus bus;
        if (bus.address.isEmpty()) {
            QSKIP("dbus-daemon is not available");
        }
        const QString fakeName = QStringLiteral("cs_orphan_fake");
        const QString clientName = QStringLiteral("cs_orphan_client");
        QTemporaryDir dir(m_testSandbox.filePath(QStringLiteral("cs_orphan_XXXXXX")));
        QVERIFY(dir.isValid());
        const QString dbPath = dir.filePath(QStringLiteral("orphan_test.db"));
        const QString credsPath = dir.filePath(QStringLiteral("credentials.json"));
        const QString listId = QStringLiteral("cs_orphan_list");
        const QString service = QStringLiteral("org.freedesktop.secrets");

        QDBusConnection fakeConn = QDBusConnection::connectToBus(bus.address, fakeName);
        QDBusConnection clientConn = QDBusConnection::connectToBus(bus.address, clientName);
        QVERIFY(fakeConn.isConnected());
        QVERIFY(clientConn.isConnected());

        FakeSecretService fakeSecrets;
        QVERIFY(fakeConn.registerService(service));
        QVERIFY(fakeConn.registerVirtualObject(QStringLiteral("/org/freedesktop/secrets"), &fakeSecrets, QDBusConnection::SubPath));

        auto& store = gofi::CredentialStore::instance();
        store.setInMemoryMode(false);
        store.setFallbackFilePath(credsPath);
        store.setDBusConnection(clientConn, service);

        auto& db = DatabaseManager::instance();

        // Step 1: Initialize DB and save list with token into Keyring
        QVERIFY(db.initialize(dbPath));
        gofi::TodoListEntry entry;
        entry.id = listId;
        entry.name = QStringLiteral("Orphan Test List");
        entry.provider = QStringLiteral("GitHub");
        entry.jiraUrl = QStringLiteral("https://example.invalid");
        entry.apiToken = QStringLiteral("pat_orphan_secret");
        QVERIFY(db.saveList(entry, 0));
        db.close();

        QCOMPARE(fakeSecrets.items.size(), 1);

        // Step 2: Lock the keyring and simulate user dismissing the unlock prompt
        fakeSecrets.lockedPaths.insert(fakeSecrets.items.firstKey());
        fakeSecrets.unlockNeedsPrompt = true;
        fakeSecrets.promptDismisses = true;

        QVERIFY(db.initialize(dbPath));
        const auto lists = db.loadLists();
        QCOMPARE(lists.size(), 1);

        // User removes the list while the keyring is locked
        QVERIFY(db.removeList(listId));
        QCOMPARE(db.loadLists().size(), 0);
        db.close();

        // The list is removed from SQLite, but since the keyring was locked and prompt dismissed,
        // the item could not be removed from fakeSecrets.items yet.
        QCOMPARE(fakeSecrets.items.size(), 1);

        // Step 3: Unlock the keyring (in a new session or after prompt reset)
        store.resetPromptState();
        fakeSecrets.lockedPaths.clear();
        fakeSecrets.unlockNeedsPrompt = false;
        fakeSecrets.promptDismisses = false;

        // Open database again and load lists (which unlocks keyring for the current database)
        QVERIFY(db.initialize(dbPath));
        const auto listsAfterUnlock = db.loadLists();
        QCOMPARE(listsAfterUnlock.size(), 0);
        db.close();

        // The orphaned secret MUST have been purged from the keyring!
        QCOMPARE(fakeSecrets.items.size(), 0);

        store.resetDBusConnection();
        store.setFallbackFilePath(QString());
        fakeConn.unregisterObject(QStringLiteral("/org/freedesktop/secrets"), QDBusConnection::UnregisterTree);
        fakeConn.unregisterService(service);
        QDBusConnection::disconnectFromBus(fakeName);
        QDBusConnection::disconnectFromBus(clientName);
    }
};

QTEST_MAIN(TestCredentials)
#include "tst_credentials.moc"
