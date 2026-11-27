#include "CredentialStore.h"
#include "DatabaseManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <sys/stat.h>
#include <memory>
#include <utility>

namespace gofi {

namespace {

struct UmaskGuard {
    mode_t oldUmask;
    explicit UmaskGuard(mode_t mask) : oldUmask(::umask(mask)) {}
    ~UmaskGuard() { ::umask(oldUmask); }
};

} // namespace

static QList<QDBusObjectPath> parseObjectPathList(const QVariant& var) {
    if (var.userType() == qMetaTypeId<QDBusArgument>()) {
        const QDBusArgument arg = var.value<QDBusArgument>();
        QList<QDBusObjectPath> paths;
        arg >> paths;
        return paths;
    }
    if (var.canConvert<QList<QDBusObjectPath>>()) {
        return var.value<QList<QDBusObjectPath>>();
    }
    if (var.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = var.value<QDBusArgument>();
        QList<QDBusObjectPath> paths;
        arg >> paths;
        return paths;
    }
    return qdbus_cast<QList<QDBusObjectPath>>(var);
}

CredentialStore& CredentialStore::instance() {
    static CredentialStore store;
    return store;
}

CredentialStore::CredentialStore() {
    qDBusRegisterMetaType<SecretServiceSecret>();
    qDBusRegisterMetaType<StringStringMap>();
}

void CredentialStore::setFallbackFilePath(const QString& path) {
    m_fallbackPath = path;
}

QString CredentialStore::fallbackFilePath() const {
    return defaultFallbackFilePath();
}

void CredentialStore::setInMemoryMode(bool enabled) {
    m_inMemory = enabled;
}

bool CredentialStore::isInMemoryMode() const {
    return m_inMemory;
}

void CredentialStore::clearInMemoryTokens() {
    m_inMemoryTokens.clear();
}

void CredentialStore::setDBusConnection(const QDBusConnection& connection, const QString& serviceName) {
    m_customBus = connection;
    m_serviceName = serviceName;
    resetPromptState();
}

void CredentialStore::resetDBusConnection() {
    m_customBus.reset();
    m_serviceName = QStringLiteral("org.freedesktop.secrets");
    resetPromptState();
}

void CredentialStore::resetPromptState() {
    m_unlockPromptDismissed = false;
    // A prompt still open belongs to the previous bus/session: its answer is ignored.
    m_unlockInProgress = false;
    ++m_unlockGeneration;
    m_unlockResult.reset();
}

QDBusConnection CredentialStore::activeBus() const {
    if (m_customBus.has_value()) {
        return *m_customBus;
    }
    return QDBusConnection::sessionBus();
}

bool CredentialStore::isKeyringAvailable() const {
    if (!m_customBus.has_value() && QStandardPaths::isTestModeEnabled()) {
        return false;
    }
    QDBusConnection bus = activeBus();
    if (!bus.isConnected()) {
        return false;
    }
    auto* iface = bus.interface();
    if (!iface) {
        return false;
    }
    if (iface->isServiceRegistered(m_serviceName)) {
        return true;
    }
    // KWallet / GNOME Keyring are usually started on demand by D-Bus activation: start it now
    // instead of treating a not-yet-running service as missing (KB-154).
    const QDBusReply<QStringList> activatable = iface->activatableServiceNames();
    if (!activatable.isValid() || !activatable.value().contains(m_serviceName)) {
        return false;
    }
    const QDBusReply<void> started = iface->startService(m_serviceName);
    if (!started.isValid()) {
        qWarning() << "CredentialStore: cannot start" << m_serviceName << ":" << started.error().message();
        return false;
    }
    return iface->isServiceRegistered(m_serviceName);
}

QString CredentialStore::currentDbScope() const {
    const QString dbPath = DatabaseManager::instance().databasePath();
    if (dbPath.isEmpty() || dbPath == QStringLiteral(":memory:")) {
        return {};
    }
    const QFileInfo fi(dbPath);
    const QString canonical = fi.canonicalFilePath();
    if (!canonical.isEmpty()) {
        return canonical;
    }
    return fi.absoluteFilePath();
}

QString CredentialStore::defaultFallbackFilePath() const {
    if (!m_fallbackPath.isEmpty()) {
        return m_fallbackPath;
    }
    const QString dbPath = DatabaseManager::instance().databasePath();
    if (!dbPath.isEmpty() && dbPath != QStringLiteral(":memory:")) {
        return QFileInfo(dbPath).dir().filePath(QStringLiteral("credentials.json"));
    }
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dataDir + QStringLiteral("/credentials.json");
}

QList<QDBusObjectPath> CredentialStore::unlockObjects(const QList<QDBusObjectPath>& locked) {
    if (locked.isEmpty() || m_unlockPromptDismissed || m_unlockInProgress) {
        return {};
    }
    QDBusConnection bus = activeBus();
    QDBusMessage unlockMsg = QDBusMessage::createMethodCall(
        m_serviceName,
        QStringLiteral("/org/freedesktop/secrets"),
        QStringLiteral("org.freedesktop.Secret.Service"),
        QStringLiteral("Unlock")
    );
    unlockMsg << QVariant::fromValue(locked);
    const QDBusMessage unlockReply = bus.call(unlockMsg, QDBus::BlockWithGui, 3000);
    if (unlockReply.type() != QDBusMessage::ReplyMessage || unlockReply.arguments().isEmpty()) {
        return {};
    }
    QList<QDBusObjectPath> unlocked = parseObjectPathList(unlockReply.arguments().at(0));
    if (!unlocked.isEmpty()) {
        return unlocked;
    }
    if (unlockReply.arguments().size() < 2) {
        return {};
    }
    const QDBusObjectPath promptPath = unlockReply.arguments().at(1).value<QDBusObjectPath>();
    if (promptPath.path().isEmpty() || promptPath.path() == QStringLiteral("/")) {
        return {};
    }
    // The user has to type the wallet password. Don't wait for it (KB-155): the window would not
    // appear, or would freeze, until the dialog is answered. The caller goes on as if the keyring were
    // locked; keyringUnlockFinished() tells when to load the tokens again.
    startUnlockPrompt(promptPath.path());
    return {};
}

void CredentialStore::startUnlockPrompt(const QString& promptPath) {
    QDBusConnection bus = activeBus();
    const QString service = m_serviceName;
    const quint64 generation = ++m_unlockGeneration;
    m_unlockInProgress = true;

    QPointer<SecretPromptHelper> helper = new SecretPromptHelper(this);
    // Completed and a failed Prompt call may both arrive; only the first one counts.
    auto released = std::make_shared<bool>(false);
    const auto release = [bus, service, promptPath, helper, released]() {
        if (*released) {
            return false;
        }
        *released = true;
        if (helper) {
            QDBusConnection connection = bus;
            connection.disconnect(service, promptPath, QStringLiteral("org.freedesktop.Secret.Prompt"),
                           QStringLiteral("Completed"), helper.data(), SLOT(completed(bool,QDBusVariant)));
            helper->deleteLater();
        }
        return true;
    };
    bus.connect(service, promptPath, QStringLiteral("org.freedesktop.Secret.Prompt"),
                QStringLiteral("Completed"), helper.data(), SLOT(completed(bool,QDBusVariant)));
    connect(helper.data(), &SecretPromptHelper::done, this, [this, generation, helper, release]() {
        const bool unlocked = helper && !helper->dismissed;
        if (release()) {
            finishUnlock(generation, unlocked);
        }
    });

    QDBusMessage promptMsg = QDBusMessage::createMethodCall(
        service, promptPath, QStringLiteral("org.freedesktop.Secret.Prompt"), QStringLiteral("Prompt"));
    promptMsg << QString();
    // Prompt() should return at once, but an implementation that answers only when the dialog closes must
    // not count as failed after the default 25 s.
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(promptMsg, 10 * 60 * 1000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, generation, release](QDBusPendingCallWatcher* w) {
        w->deleteLater();
        if (!w->isError()) {
            return;  // the answer comes with the Completed signal
        }
        qWarning() << "CredentialStore: keyring unlock prompt failed:" << w->error().message();
        if (release()) {
            finishUnlock(generation, false);
        }
    });
}

CredentialStore::BusyScope::BusyScope(CredentialStore& s) : store(s) {
    ++store.m_busyDepth;
}

CredentialStore::BusyScope::~BusyScope() {
    if (--store.m_busyDepth == 0 && store.m_unlockResult.has_value()) {
        QTimer::singleShot(0, &store, &CredentialStore::deliverUnlockResult);
    }
}

void CredentialStore::finishUnlock(quint64 generation, bool unlocked) {
    if (generation != m_unlockGeneration) {
        return;  // answer to a prompt of an earlier bus/session
    }
    m_unlockInProgress = false;
    if (!unlocked) {
        // One prompt per session: don't ask again on every token access.
        m_unlockPromptDismissed = true;
    }
    m_unlockResult = unlocked;
    if (m_busyDepth == 0) {
        QTimer::singleShot(0, this, &CredentialStore::deliverUnlockResult);
    }
}

void CredentialStore::deliverUnlockResult() {
    if (!m_unlockResult.has_value() || m_busyDepth > 0) {
        return;  // delivered when the running operation ends
    }
    const bool unlocked = *m_unlockResult;
    m_unlockResult.reset();
    if (unlocked) {
        BusyScope busy(*this);
        processPendingDeletions();
    }
    Q_EMIT keyringUnlockFinished(unlocked);
}

void CredentialStore::dismissPrompt(const QString& promptPath) {
    if (promptPath.isEmpty() || promptPath == QStringLiteral("/")) {
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_serviceName, promptPath, QStringLiteral("org.freedesktop.Secret.Prompt"), QStringLiteral("Dismiss"));
    activeBus().asyncCall(msg);
}

QDBusObjectPath CredentialStore::defaultCollectionPath() const {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_serviceName,
        QStringLiteral("/org/freedesktop/secrets"),
        QStringLiteral("org.freedesktop.Secret.Service"),
        QStringLiteral("ReadAlias")
    );
    msg << QStringLiteral("default");
    const QDBusMessage reply = activeBus().call(msg, QDBus::BlockWithGui, 1000);
    if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
        const QDBusObjectPath path = reply.arguments().at(0).value<QDBusObjectPath>();
        if (!path.path().isEmpty() && path.path() != QStringLiteral("/")) {
            return path;
        }
    }
    return QDBusObjectPath(QStringLiteral("/org/freedesktop/secrets/aliases/default"));
}

void CredentialStore::unlockKeyringForCurrentDatabase() {
    BusyScope busy(*this);
    if (m_inMemory || !isKeyringAvailable() || m_unlockPromptDismissed) {
        return;
    }
    const QString dbScope = currentDbScope();
    if (dbScope.isEmpty()) {
        return;
    }
    QDBusConnection bus = activeBus();
    StringStringMap attributes;
    attributes.insert(QStringLiteral("application"), QStringLiteral("go-for-it-qt"));
    attributes.insert(QStringLiteral("db_path"), dbScope);

    QDBusMessage searchMsg = QDBusMessage::createMethodCall(
        m_serviceName,
        QStringLiteral("/org/freedesktop/secrets"),
        QStringLiteral("org.freedesktop.Secret.Service"),
        QStringLiteral("SearchItems")
    );
    searchMsg << QVariant::fromValue(attributes);

    const QDBusMessage searchReply = bus.call(searchMsg, QDBus::BlockWithGui, 2000);
    if (searchReply.type() != QDBusMessage::ReplyMessage || searchReply.arguments().size() < 2) {
        return;
    }

    const QList<QDBusObjectPath> locked = parseObjectPathList(searchReply.arguments().at(1));
    if (!locked.isEmpty()) {
        unlockObjects(locked);
    }
    if (!m_unlockPromptDismissed && !m_unlockInProgress) {
        processPendingDeletions();
    }
}

bool CredentialStore::openSecretServiceSession(QString& sessionPath) const {
    QDBusConnection bus = activeBus();
    if (!isKeyringAvailable()) {
        return false;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_serviceName,
        QStringLiteral("/org/freedesktop/secrets"),
        QStringLiteral("org.freedesktop.Secret.Service"),
        QStringLiteral("OpenSession")
    );
    msg << QStringLiteral("plain") << QVariant::fromValue(QDBusVariant(QString()));
    const QDBusMessage reply = bus.call(msg, QDBus::BlockWithGui, 1000);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() < 2) {
        return false;
    }
    const QDBusObjectPath sessionObj = reply.arguments().at(1).value<QDBusObjectPath>();
    sessionPath = sessionObj.path();
    return !sessionPath.isEmpty();
}

bool CredentialStore::storeInKeyring(const QString& listId, const QString& token) {
    if (!isKeyringAvailable() || listId.isEmpty()) {
        return false;
    }
    QString sessionPath;
    if (!openSecretServiceSession(sessionPath)) {
        return false;
    }

    QDBusConnection bus = activeBus();
    deleteFromKeyring(listId);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        m_serviceName,
        QStringLiteral("/org/freedesktop/secrets/aliases/default"),
        QStringLiteral("org.freedesktop.Secret.Collection"),
        QStringLiteral("CreateItem")
    );

    StringStringMap attributes;
    attributes.insert(QStringLiteral("application"), QStringLiteral("go-for-it-qt"));
    attributes.insert(QStringLiteral("list_id"), listId);
    const QString dbScope = currentDbScope();
    if (!dbScope.isEmpty()) {
        attributes.insert(QStringLiteral("db_path"), dbScope);
    }

    QVariantMap properties;
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"), QStringLiteral("Go for It Qt (%1)").arg(listId));
    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"), QVariant::fromValue(attributes));

    SecretServiceSecret secret;
    secret.session = QDBusObjectPath(sessionPath);
    secret.parameters = QByteArray();
    secret.value = token.toUtf8();
    secret.contentType = QStringLiteral("text/plain");

    msg << properties;
    msg << QVariant::fromValue(secret);
    msg << true;

    const auto createItem = [&]() -> std::pair<bool, QString> {
        const QDBusMessage reply = bus.call(msg, QDBus::BlockWithGui, 1500);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
            return {false, {}};
        }
        const QDBusObjectPath itemPath = reply.arguments().at(0).value<QDBusObjectPath>();
        if (!itemPath.path().isEmpty() && itemPath.path() != QStringLiteral("/")) {
            return {true, {}};
        }
        const QString prompt = reply.arguments().size() > 1
            ? reply.arguments().at(1).value<QDBusObjectPath>().path() : QString();
        return {false, prompt};
    };

    auto [created, prompt] = createItem();
    if (!created && !prompt.isEmpty() && prompt != QStringLiteral("/")) {
        // The collection is locked (KB-156). Unlock it instead of leaving the token in the fallback
        // file: either right away, then create the item again, or through the password prompt, after
        // which loading the tokens moves the file token into the keyring.
        dismissPrompt(prompt);
        if (!unlockObjects({defaultCollectionPath()}).isEmpty()) {
            created = createItem().first;
        }
    }
    return created;
}

QString CredentialStore::loadFromKeyring(const QString& listId) {
    if (!isKeyringAvailable() || listId.isEmpty()) {
        return {};
    }
    QString sessionPath;
    if (!openSecretServiceSession(sessionPath)) {
        return {};
    }

    QDBusConnection bus = activeBus();
    const QString dbScope = currentDbScope();

    auto performSearch = [&](const StringStringMap& attrs) -> QList<QDBusObjectPath> {
        QDBusMessage searchMsg = QDBusMessage::createMethodCall(
            m_serviceName,
            QStringLiteral("/org/freedesktop/secrets"),
            QStringLiteral("org.freedesktop.Secret.Service"),
            QStringLiteral("SearchItems")
        );
        searchMsg << QVariant::fromValue(attrs);

        const QDBusMessage searchReply = bus.call(searchMsg, QDBus::BlockWithGui, 1000);
        if (searchReply.type() != QDBusMessage::ReplyMessage || searchReply.arguments().isEmpty()) {
            return {};
        }

        QList<QDBusObjectPath> unlocked = parseObjectPathList(searchReply.arguments().at(0));
        if (unlocked.isEmpty() && searchReply.arguments().size() > 1) {
            const QList<QDBusObjectPath> locked = parseObjectPathList(searchReply.arguments().at(1));
            if (!locked.isEmpty()) {
                unlocked = unlockObjects(locked);
            }
        }
        return unlocked;
    };

    StringStringMap attributes;
    attributes.insert(QStringLiteral("application"), QStringLiteral("go-for-it-qt"));
    attributes.insert(QStringLiteral("list_id"), listId);
    if (!dbScope.isEmpty()) {
        attributes.insert(QStringLiteral("db_path"), dbScope);
    }

    QList<QDBusObjectPath> unlocked = performSearch(attributes);
    if (unlocked.isEmpty()) {
        return {};
    }

    const QString itemPath = unlocked.first().path();
    QDBusMessage getSecretMsg = QDBusMessage::createMethodCall(
        m_serviceName,
        itemPath,
        QStringLiteral("org.freedesktop.Secret.Item"),
        QStringLiteral("GetSecret")
    );
    getSecretMsg << QVariant::fromValue(QDBusObjectPath(sessionPath));

    const QDBusMessage secretReply = bus.call(getSecretMsg, QDBus::BlockWithGui, 1000);
    if (secretReply.type() != QDBusMessage::ReplyMessage || secretReply.arguments().isEmpty()) {
        return {};
    }

    const QVariant secretArgVar = secretReply.arguments().at(0);
    SecretServiceSecret secret;
    if (secretArgVar.canConvert<SecretServiceSecret>()) {
        secret = secretArgVar.value<SecretServiceSecret>();
    } else if (secretArgVar.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = secretArgVar.value<QDBusArgument>();
        arg >> secret;
    }
    return QString::fromUtf8(secret.value);
}

bool CredentialStore::deleteFromKeyring(const QString& listId) {
    if (!isKeyringAvailable() || listId.isEmpty()) {
        return false;
    }
    QDBusConnection bus = activeBus();
    QDBusMessage searchMsg = QDBusMessage::createMethodCall(
        m_serviceName,
        QStringLiteral("/org/freedesktop/secrets"),
        QStringLiteral("org.freedesktop.Secret.Service"),
        QStringLiteral("SearchItems")
    );

    StringStringMap attributes;
    attributes.insert(QStringLiteral("application"), QStringLiteral("go-for-it-qt"));
    attributes.insert(QStringLiteral("list_id"), listId);
    const QString dbScope = currentDbScope();
    if (!dbScope.isEmpty()) {
        attributes.insert(QStringLiteral("db_path"), dbScope);
    }
    searchMsg << QVariant::fromValue(attributes);

    const QDBusMessage searchReply = bus.call(searchMsg, QDBus::BlockWithGui, 1000);
    if (searchReply.type() != QDBusMessage::ReplyMessage || searchReply.arguments().isEmpty()) {
        return false;
    }

    const QList<QDBusObjectPath> unlockedInitial = parseObjectPathList(searchReply.arguments().at(0));
    QList<QDBusObjectPath> itemsToDelete = unlockedInitial;
    int lockedCount = 0;
    if (searchReply.arguments().size() > 1) {
        const QList<QDBusObjectPath> locked = parseObjectPathList(searchReply.arguments().at(1));
        if (!locked.isEmpty()) {
            lockedCount = locked.size();
            const QList<QDBusObjectPath> unlockedNow = unlockObjects(locked);
            itemsToDelete.append(unlockedNow);
        }
    }

    for (const auto& item : itemsToDelete) {
        QDBusMessage delMsg = QDBusMessage::createMethodCall(
            m_serviceName,
            item.path(),
            QStringLiteral("org.freedesktop.Secret.Item"),
            QStringLiteral("Delete")
        );
        bus.call(delMsg, QDBus::BlockWithGui, 1000);
    }

    if (lockedCount > 0 && itemsToDelete.size() < (unlockedInitial.size() + lockedCount)) {
        return false;
    }
    return true;
}

QJsonObject CredentialStore::readFallbackJson() const {
    const QString path = defaultFallbackFilePath();
    if (path.isEmpty() || !QFile::exists(path)) {
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray data = file.readAll();
    file.close();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }
    return doc.object();
}

bool CredentialStore::writeFallbackJson(const QJsonObject& root) {
    const QString path = defaultFallbackFilePath();
    if (path.isEmpty()) {
        return false;
    }
    QFileInfo fi(path);
    QDir dir = fi.dir();

    // Ensure umask 0077 so all created files and directories have owner-only permissions (0600 / 0700)
    // from the initial open/mkdir syscall, preventing any temporary exposure with 0644/0755
    UmaskGuard umaskGuard(0077);

    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
        QFile::setPermissions(dir.absolutePath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    const QJsonDocument doc(root);
    const QByteArray data = doc.toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool CredentialStore::storeInFile(const QString& listId, const QString& token) {
    if (listId.isEmpty()) {
        return false;
    }
    const QString dbScope = currentDbScope();
    const QString tokenKey = dbScope.isEmpty() ? listId : QStringLiteral("%1:%2").arg(dbScope, listId);
    QJsonObject root = readFallbackJson();
    root[QStringLiteral("version")] = 1;
    QJsonObject tokens = root.value(QStringLiteral("tokens")).toObject();
    if (token.isEmpty()) {
        tokens.remove(tokenKey);
        tokens.remove(listId);
    } else {
        tokens[tokenKey] = token;
        if (tokenKey != listId) {
            tokens.remove(listId);
        }
    }
    root[QStringLiteral("tokens")] = tokens;
    return writeFallbackJson(root);
}

QString CredentialStore::loadFromFile(const QString& listId) const {
    if (listId.isEmpty()) {
        return {};
    }
    const QString dbScope = currentDbScope();
    const QString tokenKey = dbScope.isEmpty() ? listId : QStringLiteral("%1:%2").arg(dbScope, listId);
    const QJsonObject root = readFallbackJson();
    const QJsonObject tokens = root.value(QStringLiteral("tokens")).toObject();
    if (tokens.contains(tokenKey)) {
        return tokens.value(tokenKey).toString();
    }
    if (!dbScope.isEmpty() && tokens.contains(listId)) {
        return tokens.value(listId).toString();
    }
    return {};
}

bool CredentialStore::deleteFromFile(const QString& listId) {
    return storeInFile(listId, QString());
}

void CredentialStore::recordPendingDeletion(const QString& listId) {
    if (listId.isEmpty() || m_inMemory) {
        return;
    }
    const QString dbScope = currentDbScope();
    const QString tokenKey = dbScope.isEmpty() ? listId : QStringLiteral("%1:%2").arg(dbScope, listId);
    QJsonObject root = readFallbackJson();
    root[QStringLiteral("version")] = 1;
    QJsonArray pendingArr = root.value(QStringLiteral("pending_deletions")).toArray();
    for (const QJsonValue& val : pendingArr) {
        if (val.toString() == tokenKey) {
            return;
        }
    }
    pendingArr.append(tokenKey);
    root[QStringLiteral("pending_deletions")] = pendingArr;
    writeFallbackJson(root);
}

void CredentialStore::removePendingDeletion(const QString& listId) {
    if (listId.isEmpty() || m_inMemory) {
        return;
    }
    const QString dbScope = currentDbScope();
    const QString tokenKey = dbScope.isEmpty() ? listId : QStringLiteral("%1:%2").arg(dbScope, listId);
    QJsonObject root = readFallbackJson();
    if (!root.contains(QStringLiteral("pending_deletions"))) {
        return;
    }
    const QJsonArray pendingArr = root.value(QStringLiteral("pending_deletions")).toArray();
    QJsonArray newArr;
    bool found = false;
    for (const QJsonValue& val : pendingArr) {
        const QString str = val.toString();
        if (str == tokenKey || (dbScope.isEmpty() && str == listId)) {
            found = true;
        } else {
            newArr.append(val);
        }
    }
    if (found) {
        if (newArr.isEmpty()) {
            root.remove(QStringLiteral("pending_deletions"));
        } else {
            root[QStringLiteral("pending_deletions")] = newArr;
        }
        writeFallbackJson(root);
    }
}

void CredentialStore::processPendingDeletions() {
    if (m_inMemory || !isKeyringAvailable() || m_unlockPromptDismissed) {
        return;
    }
    const QString dbScope = currentDbScope();
    const QString prefix = dbScope.isEmpty() ? QString() : dbScope + QStringLiteral(":");

    QJsonObject root = readFallbackJson();
    if (!root.contains(QStringLiteral("pending_deletions"))) {
        return;
    }
    const QJsonArray pendingArr = root.value(QStringLiteral("pending_deletions")).toArray();
    if (pendingArr.isEmpty()) {
        return;
    }

    QJsonArray remainingArr;
    bool modified = false;

    for (const QJsonValue& val : pendingArr) {
        const QString itemKey = val.toString();
        QString listId;
        bool matchesScope = false;
        if (!prefix.isEmpty()) {
            if (itemKey.startsWith(prefix)) {
                matchesScope = true;
                listId = itemKey.mid(prefix.length());
            }
        } else {
            if (!itemKey.contains(QLatin1Char(':'))) {
                matchesScope = true;
                listId = itemKey;
            }
        }

        if (matchesScope && !listId.isEmpty()) {
            if (deleteFromKeyring(listId)) {
                modified = true;
                continue;
            }
        }
        remainingArr.append(val);
    }

    if (modified) {
        if (remainingArr.isEmpty()) {
            root.remove(QStringLiteral("pending_deletions"));
        } else {
            root[QStringLiteral("pending_deletions")] = remainingArr;
        }
        writeFallbackJson(root);
    }
}

bool CredentialStore::storeToken(const QString& listId, const QString& token) {
    BusyScope busy(*this);
    if (listId.isEmpty()) {
        return false;
    }
    removePendingDeletion(listId);
    if (m_inMemory) {
        if (token.isEmpty()) {
            m_inMemoryTokens.remove(listId);
        } else {
            m_inMemoryTokens[listId] = token;
        }
        return true;
    }

    if (token.isEmpty()) {
        return deleteToken(listId);
    }

    if (isKeyringAvailable() && storeInKeyring(listId, token)) {
        deleteFromFile(listId);
        return true;
    }

    return storeInFile(listId, token);
}

QString CredentialStore::loadToken(const QString& listId) {
    BusyScope busy(*this);
    if (listId.isEmpty()) {
        return {};
    }
    if (m_inMemory) {
        return m_inMemoryTokens.value(listId);
    }

    // Prefer the fallback file: a token only exists in credentials.json if a previous
    // keyring write failed (e.g. wallet was locked or offline) or from manual/migration edits.
    // If a fallback token exists, it is newer than any stale item remaining in the keyring.
    const QString fileToken = loadFromFile(listId);
    if (!fileToken.isEmpty()) {
        if (isKeyringAvailable() && storeInKeyring(listId, fileToken)) {
            deleteFromFile(listId);
        }
        return fileToken;
    }

    if (isKeyringAvailable()) {
        const QString keyringToken = loadFromKeyring(listId);
        if (!keyringToken.isEmpty()) {
            return keyringToken;
        }
    }

    return {};
}

bool CredentialStore::deleteToken(const QString& listId) {
    BusyScope busy(*this);
    if (listId.isEmpty()) {
        return false;
    }
    removePendingDeletion(listId);
    if (m_inMemory) {
        m_inMemoryTokens.remove(listId);
        return true;
    }
    bool ok = true;
    if (isKeyringAvailable()) {
        ok = deleteFromKeyring(listId) && ok;
    }
    ok = deleteFromFile(listId) && ok;
    return ok;
}

bool CredentialStore::removeListCredentials(const QString& listId) {
    BusyScope busy(*this);
    if (listId.isEmpty()) {
        return false;
    }
    if (m_inMemory) {
        m_inMemoryTokens.remove(listId);
        return true;
    }
    deleteFromFile(listId);
    bool deletedFromKeyring = false;
    if (isKeyringAvailable()) {
        deletedFromKeyring = deleteFromKeyring(listId);
    }
    if (!deletedFromKeyring) {
        recordPendingDeletion(listId);
    } else {
        removePendingDeletion(listId);
    }
    return true;
}

int CredentialStore::migratePlaintextDatabaseTokens() {
    BusyScope busy(*this);
    auto& dbMgr = DatabaseManager::instance();
    if (!dbMgr.isOpen()) {
        return 0;
    }
    QSqlQuery q(dbMgr.db());
    if (!q.exec(QStringLiteral("SELECT id, api_token FROM lists WHERE api_token IS NOT NULL AND api_token != ''"))) {
        return 0;
    }
    int migratedCount = 0;
    QVector<QPair<QString, QString>> toMigrate;
    while (q.next()) {
        const QString id = q.value(0).toString();
        const QString token = q.value(1).toString();
        if (!id.isEmpty() && !token.isEmpty()) {
            toMigrate.append({id, token});
        }
    }
    for (const auto& item : toMigrate) {
        if (storeToken(item.first, item.second)) {
            QSqlQuery updateQ(dbMgr.db());
            updateQ.prepare(QStringLiteral("UPDATE lists SET api_token = '' WHERE id = :id"));
            updateQ.bindValue(QStringLiteral(":id"), item.first);
            if (updateQ.exec()) {
                ++migratedCount;
            }
        }
    }
    return migratedCount;
}

} // namespace gofi
