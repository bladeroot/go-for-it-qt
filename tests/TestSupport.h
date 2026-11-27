#pragma once

#include "core/CredentialStore.h"
#include "core/IssueTrackerService.h"
#include "core/NotificationService.h"
#include "core/SystemHookService.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QMap>
#include <QNetworkProxy>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

// Test sandbox RAII setup: creates disposable sandbox dirs, sets HOME and XDG vars,
// and disables notification audio and system hooks during testing.
class TestSandbox {
public:
    TestSandbox() = default;

    bool init() {
        gofi::NotificationService::setAudioPlaybackEnabled(false);
        gofi::SystemHookService::setSystemIntegrationEnabled(false);
        if (!m_sandbox.isValid()) {
            return false;
        }
        const QString configDir = m_sandbox.filePath(QStringLiteral("config"));
        const QString dataDir = m_sandbox.filePath(QStringLiteral("data"));
        QDir().mkpath(configDir);
        QDir().mkpath(dataDir);

        qputenv("HOME", m_sandbox.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", configDir.toUtf8());
        qputenv("XDG_DATA_HOME", dataDir.toUtf8());
        QStandardPaths::setTestModeEnabled(true);
        return true;
    }

    QString path() const { return m_sandbox.path(); }
    QString filePath(const QString& fileName) const { return m_sandbox.filePath(fileName); }
    bool isValid() const { return m_sandbox.isValid(); }
    void remove() { m_sandbox.remove(); }

private:
    QTemporaryDir m_sandbox;
};

// Minimal HTTP/1.1 responder for tracker tests: buffers each request until its
// Content-Length body is complete, then answers once via the handler.
using HttpHandler = std::function<QByteArray(const QByteArray& method, const QByteArray& target, const QByteArray& body)>;
using HttpFullHandler = std::function<QByteArray(const QByteArray& method, const QByteArray& target, const QByteArray& head, const QByteArray& body)>;

void serveHttpFull(QTcpServer& server, const HttpFullHandler& handler);
void serveHttp(QTcpServer& server, const HttpHandler& handler);
QByteArray httpResponse(const QByteArray& status, const QByteArray& contentType, const QByteArray& body,
                        const QByteArray& extraHeaders = QByteArray());

// Captures URLs that would be handed to the desktop, so tests never open anything.
class UrlOpenSpy : public QObject {
    Q_OBJECT
public:
    QList<QUrl> opened;
public slots:
    void handle(const QUrl& url) { opened.append(url); }
};

class ApplicationProxyGuard {
public:
    ApplicationProxyGuard();
    ~ApplicationProxyGuard();

private:
    QNetworkProxy m_previousProxy;
};

// Remote writes are no-ops in test mode, so the tests switch it off for their
// duration and always restore it, even when a QVERIFY returns early.
struct RemoteWritesEnabled {
    bool previous = QStandardPaths::isTestModeEnabled();
    RemoteWritesEnabled() { QStandardPaths::setTestModeEnabled(false); }
    ~RemoteWritesEnabled() { QStandardPaths::setTestModeEnabled(previous); }
};

inline gofi::TodoListEntry makeStatusPushEntry(const QString& provider, const QTcpServer& server) {
    gofi::TodoListEntry entry;
    entry.id = QStringLiteral("kb86_%1").arg(provider.toLower());
    entry.provider = provider;
    entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    entry.jiraUser = QStringLiteral("user@example.com");
    entry.apiToken = QStringLiteral("token");
    entry.syncStatusToRemote = true;
    return entry;
}

// Copies the built QML module (*.qml only, without qmldir files) into dstRoot, so Main.qml can
// be loaded by file URL with a test AppController exposed as the `app` context property.
inline bool copyBuiltQmlTree(const QString& dstRoot) {
    const QString srcRoot = QCoreApplication::applicationDirPath() + QStringLiteral("/GoForIt/qml");
    QDir(dstRoot).removeRecursively();
    QDirIterator it(srcRoot, {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
    bool copied = false;
    while (it.hasNext()) {
        const QString f = it.next();
        const QString dst = dstRoot + f.mid(srcRoot.size());
        QDir().mkpath(QFileInfo(dst).absolutePath());
        if (!QFile::copy(f, dst)) return false;
        copied = true;
    }
    return copied;
}

class FakeKWinScripting;

class FakeKWinScript : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Script")
public:
    FakeKWinScript(FakeKWinScripting* owner, const QString& path, const QString& name);
    QString pluginName() const { return m_name; }
    bool unloaded = false;
    bool started = false;
    // Reads and "evaluates" the file (as Scripting.start() does); false if it was unreadable.
    bool evaluate();
public slots:
    Q_SCRIPTABLE void run();
private:
    FakeKWinScripting* m_owner;
    QString m_path;
    QString m_name;
};

class FakeKWinScripting : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.kwin.Scripting")
public:
    explicit FakeKWinScripting(const QDBusConnection& conn);
    ~FakeKWinScripting() override;

    QStringList executed;   // contents of the scripts KWin actually evaluated
    int unreadable = 0;     // runs that found the script file missing
    int unloads = 0;
public slots:
    // Like KWin 6.8, the returned id is the number of loaded scripts, so ids repeat after an unload
    // and the /Scripting/Script<id> path may still belong to an older, running script.
    Q_SCRIPTABLE int loadScript(const QString& filePath, const QString& pluginName);
    Q_SCRIPTABLE bool unloadScript(const QString& pluginName);
    // Runs every loaded script that has not been started yet.
    Q_SCRIPTABLE void start();
private:
    QDBusConnection m_conn;
    QList<FakeKWinScript*> m_scripts;
};

class FakeSecretService : public QDBusVirtualObject {
public:
    struct ItemData {
        QString path;
        StringStringMap attributes;
        QByteArray secretValue;
    };

    QMap<QString, ItemData> items;
    QSet<QString> lockedPaths;
    int unlockCalls = 0;
    bool unlockNeedsPrompt = false;  // real KWallet / GNOME Keyring: Unlock of a locked collection returns a prompt
    int promptDelayMs = 0;           // time the user needs to type the wallet password
    bool promptDismisses = false;    // the user closes the wallet password dialog
    bool createNeedsPrompt = false;  // CreateItem into a locked collection answers with a prompt
    bool promptAnswered = false;     // the (possibly delayed) unlock prompt has sent Completed
    QList<QDBusObjectPath> pendingUnlock;
    int nextId = 0;

    QString introspect(const QString&) const override { return {}; }
    bool handleMessage(const QDBusMessage& msg, const QDBusConnection& conn) override;
};

// Private dbus-daemon so D-Bus tests never touch the user's session bus.
struct PrivateDBus {
    QProcess daemon;
    QString address;
    PrivateDBus();
    ~PrivateDBus();
};

// Runs a TuiApp on a pseudo-terminal: stdin/stdout are redirected to the pty slave and everything
// written to the terminal is collected from the master by a reader thread. Restores fds on exit.
struct PtyTerminal {
    int master = -1;
    int slave = -1;
    int savedStdin = -1;
    int savedStdout = -1;
    QByteArray captured;
    std::mutex mutex;
    std::atomic<bool> stop{false};
    std::thread reader;

    bool open(int rows, int cols);
    void type(const QByteArray& keys);
    QByteArray takeOutput();
    ~PtyTerminal();
};
