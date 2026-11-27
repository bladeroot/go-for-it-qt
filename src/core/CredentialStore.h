#pragma once

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <optional>

namespace gofi {

class SecretPromptHelper : public QObject {
    Q_OBJECT
public:
    explicit SecretPromptHelper(QObject* parent = nullptr) : QObject(parent) {}
    bool dismissed = true;
    QVariant result;
    bool finished = false;

public Q_SLOTS:
    void completed(bool isDismissed, const QDBusVariant& res) {
        dismissed = isDismissed;
        result = res.variant();
        finished = true;
        Q_EMIT done();
    }

Q_SIGNALS:
    void done();
};

// Freedesktop Secret Service secret struct: (ObjectPath, Array<Byte>, Array<Byte>, String)
struct SecretServiceSecret {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

// Tokens of tracker lists, kept in the Freedesktop Secret Service (KWallet, GNOME Keyring) or, when it is
// unavailable or locked, in a 0600 JSON fallback file next to the database.
//
// Unlocking a locked keyring never waits for the user: when the service answers with a password prompt,
// the prompt is shown asynchronously and the calling operation behaves as if the keyring were locked
// (empty token, fallback file, pending deletion). keyringUnlockFinished() reports the user's answer;
// callers then load the tokens again.
class CredentialStore : public QObject {
    Q_OBJECT
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

    // Batch unlock all items for the currently opened database upfront
    void unlockKeyringForCurrentDatabase();
    void resetPromptState();
    // True while a keyring password prompt is open.
    [[nodiscard]] bool isUnlockInProgress() const { return m_unlockInProgress; }

    // Token operations
    [[nodiscard]] QString loadToken(const QString& listId);
    bool storeToken(const QString& listId, const QString& token);
    bool deleteToken(const QString& listId);
    bool removeListCredentials(const QString& listId);

    // Migration of legacy plaintext tokens from SQLite database
    int migratePlaintextDatabaseTokens();

Q_SIGNALS:
    // The user answered the keyring password prompt (unlocked == false: dismissed or failed).
    void keyringUnlockFinished(bool unlocked);

private:
    CredentialStore();
    ~CredentialStore() override = default;

    CredentialStore(const CredentialStore&) = delete;
    CredentialStore& operator=(const CredentialStore&) = delete;

    // Keystore operations (Freedesktop Secret Service)
    bool openSecretServiceSession(QString& sessionPath) const;
    QList<QDBusObjectPath> unlockObjects(const QList<QDBusObjectPath>& locked);
    void startUnlockPrompt(const QString& promptPath);
    void finishUnlock(quint64 generation, bool unlocked);
    void deliverUnlockResult();

    // Blocking D-Bus calls run a nested event loop, so the prompt's answer can arrive in the middle of
    // a token operation. It is delivered (pending deletions, keyringUnlockFinished) only once no
    // operation is running.
    struct BusyScope {
        explicit BusyScope(CredentialStore& store);
        ~BusyScope();
        CredentialStore& store;
    };
    void dismissPrompt(const QString& promptPath);
    QDBusObjectPath defaultCollectionPath() const;
    bool storeInKeyring(const QString& listId, const QString& token);
    QString loadFromKeyring(const QString& listId);
    bool deleteFromKeyring(const QString& listId);

    // Fallback file operations (JSON, 0600 file permissions)
    bool storeInFile(const QString& listId, const QString& token);
    QString loadFromFile(const QString& listId) const;
    bool deleteFromFile(const QString& listId);
    QJsonObject readFallbackJson() const;
    bool writeFallbackJson(const QJsonObject& root);

    void processPendingDeletions();
    void recordPendingDeletion(const QString& listId);
    void removePendingDeletion(const QString& listId);

    QString defaultFallbackFilePath() const;
    QString currentDbScope() const;
    QDBusConnection activeBus() const;

    QString m_fallbackPath;
    bool m_inMemory = false;
    QHash<QString, QString> m_inMemoryTokens;

    mutable std::optional<QDBusConnection> m_customBus;
    QString m_serviceName = QStringLiteral("org.freedesktop.secrets");
    bool m_unlockPromptDismissed = false;
    bool m_unlockInProgress = false;
    quint64 m_unlockGeneration = 0;
    int m_busyDepth = 0;
    std::optional<bool> m_unlockResult;
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

