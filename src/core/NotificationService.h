#pragma once

#include "SettingsManager.h"
#include "TaskTimer.h"
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QObject>
#include <QUrl>
#include <memory>

namespace gofi {

class NotificationService : public QObject {
    Q_OBJECT

public:
    explicit NotificationService(SettingsManager* settings, TaskTimer* timer, QObject* parent = nullptr);

    Q_INVOKABLE void playSoundFile(const QString& presetOrPath, double volume = 1.0);
    Q_INVOKABLE QVariantList availableSoundPresets() const;

    static QUrl resolveSoundUrl(const QString& presetOrPath);

signals:
    void desktopNotification(const QString& title, const QString& body);

private slots:
    void onBreakChanged();
    void onAlmostOver(uint remainingSeconds);
    void onDurationExceeded();

private:
    void sendDesktopNotification(const QString& title, const QString& body);

    SettingsManager* m_settings = nullptr;
    TaskTimer* m_timer = nullptr;
    std::unique_ptr<QMediaPlayer> m_player;
    std::unique_ptr<QAudioOutput> m_audioOutput;
    bool m_previousBreakActive = false;
};

} // namespace gofi
