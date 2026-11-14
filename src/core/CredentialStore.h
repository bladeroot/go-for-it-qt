#pragma once

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <optional>

namespace gofi {

// Freedesktop Secret Service secret struct: (ObjectPath, Array<Byte>, Array<Byte>, String)
struct SecretServiceSecret {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

class CredentialStore {
public:
    static CredentialStore& instance();

    // Fallback file path configuration
    void setFallbackFilePath(const QString& path);
    [[nodiscard]] QString fallbackFilePath() const;

    // In-memory mode (used for in-memory databases and isolated test cases)
    void setInMemoryMode(bool enabled);
    [[nodiscard]] bool isInMemoryMode() const;
    void clearInMemoryTokens();

    // Custom D-Bus connection for testing/mocking
    void setDBusConnection(const QDBusConnection& connection, const QString& serviceName = QStringLiteral("org.freedesktop.secrets"));
    void resetDBusConnection();

    // Check if Freedesktop Secret Service is currently available
    [[nodiscard]] bool isKeyringAvailable() const;

    // Token operations
    [[nodiscard]] QString loadToken(const QString& listId);
    bool storeToken(const QString& listId, const QString& token);
    bool deleteToken(const QString& listId);

    // Migration of legacy plaintext tokens from SQLite database
    int migratePlaintextDatabaseTokens();

private:
    CredentialStore();
    ~CredentialStore() = default;

    CredentialStore(const CredentialStore&) = delete;
    CredentialStore& operator=(const CredentialStore&) = delete;

    // Keystore operations (Freedesktop Secret Service)
    bool openSecretServiceSession(QString& sessionPath) const;
    bool storeInKeyring(const QString& listId, const QString& token);
    QString loadFromKeyring(const QString& listId) const;
    bool deleteFromKeyring(const QString& listId);

    // Fallback file operations (JSON, 0600 file permissions)
    bool storeInFile(const QString& listId, const QString& token);
    QString loadFromFile(const QString& listId) const;
    bool deleteFromFile(const QString& listId);
    QJsonObject readFallbackJson() const;
    bool writeFallbackJson(const QJsonObject& root);

    QString defaultFallbackFilePath() const;
    QDBusConnection activeBus() const;

    QString m_fallbackPath;
    bool m_inMemory = false;
    QHash<QString, QString> m_inMemoryTokens;

    mutable std::optional<QDBusConnection> m_customBus;
    QString m_serviceName = QStringLiteral("org.freedesktop.secrets");
};

} // namespace gofi

Q_DECLARE_METATYPE(gofi::SecretServiceSecret)

inline QDBusArgument& operator<<(QDBusArgument& argument, const gofi::SecretServiceSecret& secret) {
    argument.beginStructure();
    argument << secret.session;
    argument << secret.parameters;
    argument << secret.value;
    argument << secret.contentType;
    argument.endStructure();
    return argument;
}

inline const QDBusArgument& operator>>(const QDBusArgument& argument, gofi::SecretServiceSecret& secret) {
    argument.beginStructure();
    argument >> secret.session;
    argument >> secret.parameters;
    argument >> secret.value;
    argument >> secret.contentType;
    argument.endStructure();
    return argument;
}

typedef QMap<QString, QString> StringStringMap;
Q_DECLARE_METATYPE(StringStringMap)

inline QDBusArgument& operator<<(QDBusArgument& argument, const StringStringMap& map) {
    argument.beginMap(QMetaType::fromType<QString>(), QMetaType::fromType<QString>());
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        argument.beginMapEntry();
        argument << it.key() << it.value();
        argument.endMapEntry();
    }
    argument.endMap();
    return argument;
}

inline const QDBusArgument& operator>>(const QDBusArgument& argument, StringStringMap& map) {
    argument.beginMap();
    map.clear();
    while (!argument.atEnd()) {
        QString key;
        QString val;
        argument.beginMapEntry();
        argument >> key >> val;
        argument.endMapEntry();
        map.insert(key, val);
    }
    argument.endMap();
    return argument;
}

