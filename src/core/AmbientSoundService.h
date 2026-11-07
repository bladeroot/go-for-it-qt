#pragma once

#include "TaskTimer.h"
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <memory>

namespace gofi {

class AmbientSoundService : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString ambientMode READ ambientMode WRITE setAmbientMode NOTIFY ambientModeChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY playingChanged)

public:
    explicit AmbientSoundService(TaskTimer* timer, QObject* parent = nullptr);
    ~AmbientSoundService() override;

    [[nodiscard]] QString ambientMode() const { return m_mode; }
    void setAmbientMode(const QString& mode);

    [[nodiscard]] double volume() const noexcept { return m_volume; }
    void setVolume(double vol);

    [[nodiscard]] bool isPlaying() const noexcept { return m_isPlaying; }

    Q_INVOKABLE QVariantList availableModes() const;

    static bool generateAmbientWav(const QString& type, const QString& destinationPath);

signals:
    void ambientModeChanged();
    void volumeChanged();
    void playingChanged();

private slots:
    void updatePlaybackState();

private:
    void ensurePlayerInitialized();

    TaskTimer* m_timer = nullptr;
    std::unique_ptr<QAudioOutput> m_audioOutput;
    std::unique_ptr<QMediaPlayer> m_player;
    QString m_mode = QStringLiteral("none");
    double m_volume = 0.5;
    bool m_isPlaying = false;
};

} // namespace gofi
