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
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QStandardPaths>

namespace gofi {

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
}

void CredentialStore::resetDBusConnection() {
    m_customBus.reset();
    m_serviceName = QStringLiteral("org.freedesktop.secrets");
}

QDBusConnection CredentialStore::activeBus() const {
    if (m_customBus.has_value()) {
        return *m_customBus;
    }
    return QDBusConnection::sessionBus();
}

bool CredentialStore::isKeyringAvailable() const {
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
    QDBusMessage searchMsg = QDBusMessage::createMethodCall(
        m_serviceName,
        QStringLiteral("/org/freedesktop/secrets"),
        QStringLiteral("org.freedesktop.Secret.Service"),
        QStringLiteral("SearchItems")
    );
    StringStringMap attributes;
    attributes.insert(QStringLiteral("application"), QStringLiteral("go-for-it-qt"));
    attributes.insert(QStringLiteral("list_id"), listId);
    searchMsg << QVariant::fromValue(attributes);

    const QDBusMessage searchReply = bus.call(searchMsg, QDBus::BlockWithGui, 1000);
    if (searchReply.type() != QDBusMessage::ReplyMessage || searchReply.arguments().isEmpty()) {
        return {};
    }

    QList<QDBusObjectPath> unlocked;
    const QVariant unlockedVar = searchReply.arguments().at(0);
    if (unlockedVar.canConvert<QList<QDBusObjectPath>>()) {
        unlocked = unlockedVar.value<QList<QDBusObjectPath>>();
    } else if (unlockedVar.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = unlockedVar.value<QDBusArgument>();
        arg >> unlocked;
    } else {
        unlocked = qdbus_cast<QList<QDBusObjectPath>>(unlockedVar);
    }
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
    searchMsg << QVariant::fromValue(attributes);

    const QDBusMessage searchReply = bus.call(searchMsg, QDBus::BlockWithGui, 1000);
    if (searchReply.type() != QDBusMessage::ReplyMessage || searchReply.arguments().isEmpty()) {
        return false;
    }

    QList<QDBusObjectPath> unlocked;
    const QVariant unlockedVar = searchReply.arguments().at(0);
    if (unlockedVar.canConvert<QList<QDBusObjectPath>>()) {
        unlocked = unlockedVar.value<QList<QDBusObjectPath>>();
    } else if (unlockedVar.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = unlockedVar.value<QDBusArgument>();
        arg >> unlocked;
    } else {
        unlocked = qdbus_cast<QList<QDBusObjectPath>>(unlockedVar);
    }
    for (const auto& item : unlocked) {
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
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
        QFile::setPermissions(dir.absolutePath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    const QString tempPath = dir.filePath(QStringLiteral(".credentials.tmp.%1_%2")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch()));

    QFile file(tempPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.flush();
    file.close();

    QFile::setPermissions(tempPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    if (QFile::exists(path)) {
        QFile::remove(path);
    }
    if (!QFile::rename(tempPath, path)) {
        if (QFile::copy(tempPath, path)) {
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            QFile::remove(tempPath);
            return true;
        }
        QFile::remove(tempPath);
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool CredentialStore::storeInFile(const QString& listId, const QString& token) {
    if (listId.isEmpty()) {
        return false;
    }
    QJsonObject root = readFallbackJson();
    root[QStringLiteral("version")] = 1;
    QJsonObject tokens = root.value(QStringLiteral("tokens")).toObject();
    if (token.isEmpty()) {
        tokens.remove(listId);
    } else {
        tokens[listId] = token;
    }
    root[QStringLiteral("tokens")] = tokens;
    return writeFallbackJson(root);
}

QString CredentialStore::loadFromFile(const QString& listId) const {
    if (listId.isEmpty()) {
        return {};
    }
    const QJsonObject root = readFallbackJson();
    const QJsonObject tokens = root.value(QStringLiteral("tokens")).toObject();
    return tokens.value(listId).toString();
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

    if (isKeyringAvailable()) {
        const QString keyringToken = loadFromKeyring(listId);
        if (!keyringToken.isEmpty()) {
            return keyringToken;
        }
    }

    const QString fileToken = loadFromFile(listId);
    if (!fileToken.isEmpty()) {
        if (isKeyringAvailable() && storeInKeyring(listId, fileToken)) {
            deleteFromFile(listId);
        }
        return fileToken;
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
