#include "NotificationService.h"
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QFileInfo>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QVariantMap>

namespace gofi {

NotificationService::NotificationService(SettingsManager* settings, TaskTimer* timer, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_timer(timer)
{
    if (m_timer) {
        m_previousBreakActive = m_timer->isBreakActive();
        connect(m_timer, &TaskTimer::breakActiveChanged, this, &NotificationService::onBreakChanged);
        connect(m_timer, &TaskTimer::timerAlmostOver, this, &NotificationService::onAlmostOver);
        connect(m_timer, &TaskTimer::taskDurationExceeded, this, &NotificationService::onDurationExceeded);
    }
}

void NotificationService::playSoundFile(const QString& presetOrPath, double volume) {
    if (presetOrPath.isEmpty()) {
        return;
    }
    const QUrl url = resolveSoundUrl(presetOrPath);
    if (!url.isValid() || (url.isLocalFile() && !QFileInfo::exists(url.toLocalFile()))) {
        return;
    }
    if (!m_player) {
        m_audioOutput = std::make_unique<QAudioOutput>();
        m_player = std::make_unique<QMediaPlayer>();
        m_player->setAudioOutput(m_audioOutput.get());
    }
    m_audioOutput->setVolume(static_cast<float>(qBound(0.0, volume, 1.0)));
    m_player->stop();
    m_player->setSource(url);
    m_player->play();
}

QVariantList NotificationService::availableSoundPresets() const {
    return {
        QVariantMap{{QStringLiteral("id"), QStringLiteral("singing-bowl.ogg")},   {QStringLiteral("name"), tr("Singing bowl")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("aluminium-bowl.ogg")}, {QStringLiteral("name"), tr("Aluminium bowl")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("bell.ogg")},           {QStringLiteral("name"), tr("Bell")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("loud-bell.ogg")},      {QStringLiteral("name"), tr("Loud bell")}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("")},                   {QStringLiteral("name"), tr("Silent")}}
    };
}

QUrl NotificationService::resolveSoundUrl(const QString& presetOrPath) {
    if (presetOrPath.isEmpty()) {
        return {};
    }
    if (presetOrPath.startsWith(QStringLiteral("file://"))) {
        return QUrl(presetOrPath);
    }
    QFileInfo directInfo(presetOrPath);
    if (directInfo.isAbsolute() && directInfo.exists()) {
        return QUrl::fromLocalFile(directInfo.absoluteFilePath());
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList relCandidates = {
        appDir + QStringLiteral("/data/sounds/") + presetOrPath,
        appDir + QStringLiteral("/../share/go-for-it-qt/sounds/") + presetOrPath,
        appDir + QStringLiteral("/../data/sounds/") + presetOrPath
    };
    for (const QString& cand : relCandidates) {
        if (QFileInfo::exists(cand)) {
            return QUrl::fromLocalFile(QFileInfo(cand).canonicalFilePath());
        }
    }

    QString sysSound = QStandardPaths::locate(
        QStandardPaths::GenericDataLocation,
        QStringLiteral("go-for-it-qt/sounds/") + presetOrPath
    );
    if (sysSound.isEmpty()) {
        sysSound = QStandardPaths::locate(
            QStandardPaths::GenericDataLocation,
            QStringLiteral("com.github.jmoerman.go-for-it/sounds/") + presetOrPath
        );
    }
    if (!sysSound.isEmpty()) {
        return QUrl::fromLocalFile(sysSound);
    }
    return {};
}

void NotificationService::onBreakChanged() {
    if (!m_timer || !m_timer->hasActiveTask()) {
        return;
    }
    const bool breakActive = m_timer->isBreakActive();
    if (breakActive == m_previousBreakActive) {
        return;
    }
    m_previousBreakActive = breakActive;

    if (breakActive) {
        sendDesktopNotification(
            tr("Take a Break"),
            tr("Relax and stop thinking about your current task for a while :-)")
        );
        if (m_settings && !m_settings->muteSounds()) {
            playSoundFile(m_settings->breakStartSoundFile(), m_settings->breakStartSoundVolume());
        }
    } else {
        sendDesktopNotification(
            tr("The Break is Over"),
            tr("Your current task is: %1").arg(m_timer->activeTaskDescription())
        );
        if (m_settings && !m_settings->muteSounds()) {
            playSoundFile(m_settings->breakEndSoundFile(), m_settings->breakEndSoundVolume());
        }
    }
}

void NotificationService::onAlmostOver(uint remainingSeconds) {
    sendDesktopNotification(
        tr("Prepare for your break"),
        tr("You have %1 seconds left").arg(remainingSeconds)
    );
    if (m_settings && !m_settings->muteSounds()) {
        playSoundFile(m_settings->reminderSoundFile(), m_settings->reminderSoundVolume());
    }
}

void NotificationService::onDurationExceeded() {
    sendDesktopNotification(
        tr("Task duration exceeded"),
        tr("Consider switching to a different task")
    );
    if (m_settings && !m_settings->muteSounds()) {
        playSoundFile(m_settings->reminderSoundFile(), m_settings->reminderSoundVolume());
    }
}

void NotificationService::sendDesktopNotification(const QString& title, const QString& body) {
    emit desktopNotification(title, body);

    // Send standard Freedesktop DBus notification if session bus is available
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("Notify")
    );
    msg << QStringLiteral("Go for It Qt")
        << uint(0)
        << QStringLiteral("go-for-it-qt")
        << title
        << body
        << QStringList()
        << QVariantMap()
        << int(4000);
    bus.asyncCall(msg);
}

} // namespace gofi
