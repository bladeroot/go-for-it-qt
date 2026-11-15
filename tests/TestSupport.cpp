#include "TestSupport.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

void serveHttpFull(QTcpServer& server, const HttpFullHandler& handler) {
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, handler]() {
        while (server.hasPendingConnections()) {
            QTcpSocket* socket = server.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, buffer, handler]() {
                buffer->append(socket->readAll());
                while (true) {
                    const int headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    const QByteArray head = buffer->left(headerEnd);
                    qsizetype contentLength = 0;
                    for (const QByteArray& line : head.split('\n')) {
                        if (line.trimmed().toLower().startsWith("content-length:")) {
                            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                        }
                    }
                    if (buffer->size() < headerEnd + 4 + contentLength) return;
                    const QByteArray body = buffer->mid(headerEnd + 4, contentLength);
                    buffer->remove(0, headerEnd + 4 + contentLength);
                    const QList<QByteArray> requestLine = head.left(head.indexOf("\r\n")).split(' ');
                    const QByteArray method = requestLine.value(0);
                    const QByteArray target = requestLine.value(1);
                    socket->write(handler(method, target, head, body));
                }
            });
        }
    });
}

void serveHttp(QTcpServer& server, const HttpHandler& handler) {
    serveHttpFull(server, [handler](const QByteArray& method, const QByteArray& target, const QByteArray&, const QByteArray& body) {
        return handler(method, target, body);
    });
}

QByteArray httpResponse(const QByteArray& status, const QByteArray& contentType, const QByteArray& body,
                        const QByteArray& extraHeaders) {
    return "HTTP/1.1 " + status + "\r\n"
           "Content-Type: " + contentType + "\r\n" + extraHeaders +
           "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}

ApplicationProxyGuard::ApplicationProxyGuard()
    : m_previousProxy(QNetworkProxy::applicationProxy()) {
    QNetworkProxy::setApplicationProxy(
        QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 1));
}

ApplicationProxyGuard::~ApplicationProxyGuard() {
    QNetworkProxy::setApplicationProxy(m_previousProxy);
}

FakeKWinScript::FakeKWinScript(FakeKWinScripting* owner, const QString& path, const QString& name)
    : QObject(nullptr), m_owner(owner), m_path(path), m_name(name) {}

void FakeKWinScript::run() {
    setDelayedReply(true);
    const QDBusMessage msg = message();
    QDBusConnection conn = connection();
    QPointer<FakeKWinScript> self(this);
    // KWin reads the file on a worker thread; model the latency so the order is deterministic.
    QTimer::singleShot(30, this, [self, msg, conn]() mutable {
        if (!self || self->unloaded) return; // unloaded while loading: never evaluated, never answered
        QFile f(self->m_path);
        if (!f.open(QIODevice::ReadOnly)) {
            ++self->m_owner->unreadable;
            conn.send(msg.createErrorReply(QStringLiteral("org.kde.kwin.Scripting.FileError"), QStringLiteral("unreadable")));
            return;
        }
        self->m_owner->executed.append(QString::fromUtf8(f.readAll()));
        conn.send(msg.createReply());
    });
}

FakeKWinScripting::FakeKWinScripting(const QDBusConnection& conn) : m_conn(conn) {}

FakeKWinScripting::~FakeKWinScripting() {
    for (int i = 0; i < m_scripts.size(); ++i) {
        m_conn.unregisterObject(QStringLiteral("/Scripting/Script%1").arg(i));
    }
    qDeleteAll(m_scripts);
}

int FakeKWinScripting::loadScript(const QString& filePath, const QString& pluginName) {
    for (FakeKWinScript* sc : std::as_const(m_scripts)) {
        if (!sc->unloaded && sc->pluginName() == pluginName) return -1;
    }
    const int id = static_cast<int>(m_scripts.size());
    auto* sc = new FakeKWinScript(this, filePath, pluginName);
    m_scripts.append(sc);
    m_conn.registerObject(QStringLiteral("/Scripting/Script%1").arg(id), sc, QDBusConnection::ExportScriptableSlots);
    return id;
}

bool FakeKWinScripting::unloadScript(const QString& pluginName) {
    ++unloads;
    for (FakeKWinScript* sc : std::as_const(m_scripts)) {
        if (!sc->unloaded && sc->pluginName() == pluginName) {
            sc->unloaded = true;
            return true;
        }
    }
    return false;
}

bool FakeSecretService::handleMessage(const QDBusMessage& msg, const QDBusConnection& conn) {
    if (msg.type() != QDBusMessage::MethodCallMessage) {
        return false;
    }
    const QString member = msg.member();
    if (member == QStringLiteral("OpenSession")) {
        QDBusMessage reply = msg.createReply();
        reply << QVariant::fromValue(QDBusVariant(QString()))
              << QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/freedesktop/secrets/session/fake")));
        conn.send(reply);
        return true;
    }
    if (member == QStringLiteral("CreateItem") && createNeedsPrompt) {
        QDBusMessage reply = msg.createReply();
        reply << QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")))
              << QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/freedesktop/secrets/prompt/c1")));
        conn.send(reply);
        return true;
    }
    if (member == QStringLiteral("CreateItem")) {
        const QString itemPath = QStringLiteral("/org/freedesktop/secrets/item/%1").arg(++nextId);
        ItemData data;
        data.path = itemPath;
        if (!msg.arguments().isEmpty()) {
            const QVariantMap props = qdbus_cast<QVariantMap>(msg.arguments().at(0));
            QVariant attrVar = props.value(QStringLiteral("org.freedesktop.Secret.Item.Attributes"));
            if (attrVar.userType() == qMetaTypeId<QDBusVariant>()) {
                attrVar = attrVar.value<QDBusVariant>().variant();
            }
            if (attrVar.userType() == qMetaTypeId<QDBusArgument>()) {
                const QDBusArgument arg = attrVar.value<QDBusArgument>();
                arg >> data.attributes;
            } else {
                data.attributes = attrVar.value<StringStringMap>();
            }
        }
        if (msg.arguments().size() > 1) {
            const QVariant secVar = msg.arguments().at(1);
            if (secVar.canConvert<gofi::SecretServiceSecret>()) {
                data.secretValue = secVar.value<gofi::SecretServiceSecret>().value;
            } else if (secVar.canConvert<QDBusArgument>()) {
                gofi::SecretServiceSecret sec;
                const QDBusArgument arg = secVar.value<QDBusArgument>();
                arg >> sec;
                data.secretValue = sec.value;
            }
        }
        items.insert(itemPath, data);
        QDBusMessage reply = msg.createReply();
        reply << QVariant::fromValue(QDBusObjectPath(itemPath))
              << QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")));
        conn.send(reply);
        return true;
    }
    if (member == QStringLiteral("SearchItems")) {
        StringStringMap searchAttrs;
        if (!msg.arguments().isEmpty()) {
            const QVariant aVar = msg.arguments().at(0);
            if (aVar.userType() == qMetaTypeId<QDBusArgument>()) {
                const QDBusArgument arg = aVar.value<QDBusArgument>();
                arg >> searchAttrs;
            } else {
                searchAttrs = aVar.value<StringStringMap>();
            }
        }
        QList<QDBusObjectPath> unlocked;
        QList<QDBusObjectPath> locked;
        for (auto it = items.cbegin(); it != items.cend(); ++it) {
            bool matches = !searchAttrs.isEmpty();
            if (matches) {
                for (auto a = searchAttrs.cbegin(); a != searchAttrs.cend(); ++a) {
                    if (it.value().attributes.value(a.key()) != a.value()) {
                        matches = false;
                        break;
                    }
                }
            }
            if (matches) {
                if (lockedPaths.contains(it.value().path)) {
                    locked.append(QDBusObjectPath(it.value().path));
                } else {
                    unlocked.append(QDBusObjectPath(it.value().path));
                }
            }
        }
        QDBusMessage reply = msg.createReply();
        reply << QVariant::fromValue(unlocked)
              << QVariant::fromValue(locked);
        conn.send(reply);
        return true;
    }
    if (member == QStringLiteral("Unlock")) {
        ++unlockCalls;
        QList<QDBusObjectPath> requested;
        if (!msg.arguments().isEmpty()) {
            const QVariant v = msg.arguments().at(0);
            if (v.canConvert<QDBusArgument>()) {
                v.value<QDBusArgument>() >> requested;
            } else {
                requested = qdbus_cast<QList<QDBusObjectPath>>(v);
            }
        }
        if (unlockNeedsPrompt) {
            pendingUnlock = requested;
            QDBusMessage reply = msg.createReply();
            reply << QVariant::fromValue(QList<QDBusObjectPath>())
                  << QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/freedesktop/secrets/prompt/u1")));
            conn.send(reply);
            return true;
        }
        for (const auto& p : requested) {
            lockedPaths.remove(p.path());
        }
        QDBusMessage reply = msg.createReply();
        reply << QVariant::fromValue(requested)
              << QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")));
        conn.send(reply);
        return true;
    }
    if (member == QStringLiteral("Prompt") && msg.path() == QStringLiteral("/org/freedesktop/secrets/prompt/u1")) {
        conn.send(msg.createReply());
        // The user entered the wallet password: unlock and report completion.
        const QString promptPath = msg.path();
        QDBusConnection c = conn;
        auto complete = [this, c, promptPath]() mutable {
            if (promptDismisses) {
                QDBusMessage dismissed = QDBusMessage::createSignal(promptPath, QStringLiteral("org.freedesktop.Secret.Prompt"), QStringLiteral("Completed"));
                dismissed << true << QVariant::fromValue(QDBusVariant(QVariant::fromValue(QList<QDBusObjectPath>())));
                c.send(dismissed);
                return;
            }
            for (const auto& p : pendingUnlock) {
                lockedPaths.remove(p.path());
            }
            QDBusMessage completed = QDBusMessage::createSignal(promptPath, QStringLiteral("org.freedesktop.Secret.Prompt"), QStringLiteral("Completed"));
            completed << false << QVariant::fromValue(QDBusVariant(QVariant::fromValue(pendingUnlock)));
            c.send(completed);
        };
        if (promptDelayMs > 0) {
            QTimer::singleShot(promptDelayMs, complete);
        } else {
            complete();
        }
        return true;
    }
    if (member == QStringLiteral("GetSecret")) {
        const QString path = msg.path();
        if (lockedPaths.contains(path)) {
            conn.send(msg.createErrorReply(QStringLiteral("org.freedesktop.Secret.Error.IsLocked"), QStringLiteral("locked")));
            return true;
        }
        ItemData data = items.value(path);
        gofi::SecretServiceSecret sec;
        if (!msg.arguments().isEmpty()) {
            sec.session = msg.arguments().at(0).value<QDBusObjectPath>();
        }
        sec.value = data.secretValue;
        sec.contentType = QStringLiteral("text/plain");
        QDBusMessage reply = msg.createReply();
        reply << QVariant::fromValue(sec);
        conn.send(reply);
        return true;
    }
    if (member == QStringLiteral("Delete")) {
        const QString path = msg.path();
        items.remove(path);
        QDBusMessage reply = msg.createReply();
        reply << QVariant::fromValue(QDBusObjectPath(QStringLiteral("/")));
        conn.send(reply);
        return true;
    }
    return false;
}

PrivateDBus::PrivateDBus() {
    daemon.start(QStringLiteral("dbus-daemon"),
                 {QStringLiteral("--session"), QStringLiteral("--nofork"), QStringLiteral("--print-address=1")});
    if (daemon.waitForStarted(3000) && daemon.waitForReadyRead(3000)) {
        address = QString::fromUtf8(daemon.readLine()).trimmed();
    }
}

PrivateDBus::~PrivateDBus() {
    daemon.terminate();
    if (!daemon.waitForFinished(3000)) {
        daemon.kill();
        daemon.waitForFinished(3000);
    }
}

bool PtyTerminal::open(int rows, int cols) {
    master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) return false;
    slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY);
    if (slave < 0) return false;
    struct winsize ws {};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ::ioctl(slave, TIOCSWINSZ, &ws);
    reader = std::thread([this]() {
        char buf[65536];
        while (!stop.load()) {
            struct pollfd pfd { master, POLLIN, 0 };
            if (::poll(&pfd, 1, 20) > 0) {
                const ssize_t n = ::read(master, buf, sizeof buf);
                if (n > 0) {
                    std::lock_guard<std::mutex> lock(mutex);
                    captured.append(buf, n);
                }
            }
        }
    });
    std::fflush(stdout);
    savedStdin = ::dup(STDIN_FILENO);
    savedStdout = ::dup(STDOUT_FILENO);
    ::dup2(slave, STDIN_FILENO);
    ::dup2(slave, STDOUT_FILENO);
    return true;
}

void PtyTerminal::type(const QByteArray& keys) {
    (void)::write(master, keys.constData(), static_cast<size_t>(keys.size()));
}

QByteArray PtyTerminal::takeOutput() {
    std::lock_guard<std::mutex> lock(mutex);
    QByteArray out = captured;
    captured.clear();
    return out;
}

PtyTerminal::~PtyTerminal() {
    std::fflush(stdout);
    if (savedStdin >= 0) { ::dup2(savedStdin, STDIN_FILENO); ::close(savedStdin); }
    if (savedStdout >= 0) { ::dup2(savedStdout, STDOUT_FILENO); ::close(savedStdout); }
    stop = true;
    if (reader.joinable()) reader.join();
    if (slave >= 0) ::close(slave);
    if (master >= 0) ::close(master);
}
