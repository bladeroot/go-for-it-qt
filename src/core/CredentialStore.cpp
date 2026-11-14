#include "CredentialStore.h"
#include "DatabaseManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTimer>
#include <sys/stat.h>

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
    m_unlockPromptDismissed = false;
}

void CredentialStore::resetDBusConnection() {
    m_customBus.reset();
    m_serviceName = QStringLiteral("org.freedesktop.secrets");
    m_unlockPromptDismissed = false;
}

void CredentialStore::resetPromptState() {
    m_unlockPromptDismissed = false;
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
    if (!iface || !iface->isServiceRegistered(m_serviceName)) {
        return false;
    }
    return true;
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

QList<QDBusObjectPath> CredentialStore::unlockObjects(const QList<QDBusObjectPath>& locked) const {
    if (locked.isEmpty() || m_unlockPromptDismissed) {
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

    SecretPromptHelper helper;
    QEventLoop loop;
    QObject::connect(&helper, &SecretPromptHelper::done, &loop, &QEventLoop::quit);

    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    bus.connect(
        m_serviceName,
        promptPath.path(),
        QStringLiteral("org.freedesktop.Secret.Prompt"),
        QStringLiteral("Completed"),
        &helper,
        SLOT(completed(bool,QDBusVariant))
    );

    QDBusMessage promptMsg = QDBusMessage::createMethodCall(
        m_serviceName,
        promptPath.path(),
        QStringLiteral("org.freedesktop.Secret.Prompt"),
        QStringLiteral("Prompt")
    );
    promptMsg << QStringLiteral("");
    bus.call(promptMsg, QDBus::BlockWithGui, 30000);

    if (!helper.finished) {
        timer.start(120000);
        loop.exec();
    }

    bus.disconnect(
        m_serviceName,
        promptPath.path(),
        QStringLiteral("org.freedesktop.Secret.Prompt"),
        QStringLiteral("Completed"),
        &helper,
        SLOT(completed(bool,QDBusVariant))
    );

    if (helper.dismissed) {
        m_unlockPromptDismissed = true;
        return {};
    }

    QList<QDBusObjectPath> unlockedFromPrompt = parseObjectPathList(helper.result);
    if (unlockedFromPrompt.isEmpty()) {
        unlockedFromPrompt = locked;
    }
    return unlockedFromPrompt;
}

void CredentialStore::unlockKeyringForCurrentDatabase() {
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

    const QDBusMessage reply = bus.call(msg, QDBus::BlockWithGui, 1500);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return false;
    }

    const QDBusObjectPath itemPath = reply.arguments().at(0).value<QDBusObjectPath>();
    return !itemPath.path().isEmpty() && itemPath.path() != QStringLiteral("/");
}

QString CredentialStore::loadFromKeyring(const QString& listId) const {
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

    QList<QDBusObjectPath> itemsToDelete = parseObjectPathList(searchReply.arguments().at(0));
    if (searchReply.arguments().size() > 1) {
        const QList<QDBusObjectPath> locked = parseObjectPathList(searchReply.arguments().at(1));
        if (!locked.isEmpty()) {
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

bool CredentialStore::storeToken(const QString& listId, const QString& token) {
    if (listId.isEmpty()) {
        return false;
    }
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
    if (listId.isEmpty()) {
        return false;
    }
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

int CredentialStore::migratePlaintextDatabaseTokens() {
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
