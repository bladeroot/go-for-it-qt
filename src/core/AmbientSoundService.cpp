#include "AmbientSoundService.h"
#include "NotificationService.h"
#include <QAudioOutput>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QUrl>
#include <cmath>

namespace gofi {

AmbientSoundService::AmbientSoundService(TaskTimer* timer, QObject* parent)
    : QObject(parent)
    , m_timer(timer)
{
    if (m_timer) {
        connect(m_timer, &TaskTimer::runningChanged, this, &AmbientSoundService::updatePlaybackState);
        connect(m_timer, &TaskTimer::breakActiveChanged, this, &AmbientSoundService::updatePlaybackState);
    }
}

AmbientSoundService::~AmbientSoundService() = default;

void AmbientSoundService::ensurePlayerInitialized() {
    if (!NotificationService::isAudioPlaybackEnabled()) {
        return;
    }
    if (!m_player) {
        m_audioOutput = std::make_unique<QAudioOutput>();
        m_player = std::make_unique<QMediaPlayer>();
        m_player->setAudioOutput(m_audioOutput.get());
        m_player->setLoops(QMediaPlayer::Infinite);
        m_audioOutput->setVolume(static_cast<float>(qBound(0.0, m_volume, 1.0)));
    }
}

QVariantList AmbientSoundService::availableModes() const {
    const QString offName = tr("Off (No ambient sound)");
    const QString whiteName = tr("White / Pink Noise (Focus Hum)");
    const QString rainName = tr("Gentle Rain (Rainfall)");
    const QString clockName = tr("Mechanical Clock Ticking");
    return QVariantList{
        QVariantMap{{QStringLiteral("id"), QStringLiteral("none")}, {QStringLiteral("name"), offName}, {QStringLiteral("label"), offName}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("white_noise")}, {QStringLiteral("name"), whiteName}, {QStringLiteral("label"), whiteName}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("rain")}, {QStringLiteral("name"), rainName}, {QStringLiteral("label"), rainName}},
        QVariantMap{{QStringLiteral("id"), QStringLiteral("clock")}, {QStringLiteral("name"), clockName}, {QStringLiteral("label"), clockName}}
    };
}

QString AmbientSoundService::ambientModeName() const {
    if (m_mode == QStringLiteral("white_noise")) {
        return tr("White / Pink Noise (Focus Hum)");
    }
    if (m_mode == QStringLiteral("rain")) {
        return tr("Gentle Rain (Rainfall)");
    }
    if (m_mode == QStringLiteral("clock")) {
        return tr("Mechanical Clock Ticking");
    }
    return tr("Off (No ambient sound)");
}

void AmbientSoundService::cycleAmbientMode() {
    if (m_mode == QStringLiteral("none")) {
        setAmbientMode(QStringLiteral("white_noise"));
    } else if (m_mode == QStringLiteral("white_noise")) {
        setAmbientMode(QStringLiteral("rain"));
    } else if (m_mode == QStringLiteral("rain")) {
        setAmbientMode(QStringLiteral("clock"));
    } else {
        setAmbientMode(QStringLiteral("none"));
    }
}

void AmbientSoundService::setVolume(double vol) {
    const double clamped = qBound(0.0, vol, 1.0);
    if (qFuzzyCompare(m_volume, clamped)) return;
    m_volume = clamped;
    if (m_audioOutput) {
        m_audioOutput->setVolume(static_cast<float>(m_volume));
    }
    emit volumeChanged();
}

void AmbientSoundService::setAmbientMode(const QString& mode) {
    const QString norm = mode.trimmed().toLower();
    if (m_mode == norm) return;
    m_mode = norm;
    emit ambientModeChanged();

    if (m_mode == QStringLiteral("none") || QStandardPaths::isTestModeEnabled()) {
        if (m_player) m_player->stop();
        if (m_isPlaying) {
            m_isPlaying = false;
            emit playingChanged();
        }
        return;
    }

    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheDir);
    QFile::setPermissions(cacheDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString wavPath = QDir(cacheDir).filePath(QStringLiteral("gofi_ambient_%1.wav").arg(m_mode));

    bool validFile = false;
    QFile checkFile(wavPath);
    if (checkFile.exists()) {
        const QFileDevice::Permissions others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther;
        if ((checkFile.permissions() & others) == 0 && checkFile.size() == (44 + 44100 * 4 * 2)) {
            if (checkFile.open(QIODevice::ReadOnly)) {
                const QByteArray hdr = checkFile.read(12);
                if (hdr.size() == 12 && hdr.startsWith("RIFF") && hdr.mid(8, 4) == "WAVE") {
                    validFile = true;
                }
                checkFile.close();
            }
        }
    }

    if (!validFile) {
        QFile::remove(wavPath);
        generateAmbientWav(m_mode, wavPath);
    }

    ensurePlayerInitialized();
    if (m_player) {
        m_player->stop();
        m_player->setSource(QUrl::fromLocalFile(wavPath));
    }

    updatePlaybackState();
}

void AmbientSoundService::updatePlaybackState() {
    if (m_mode.isEmpty() || m_mode == QStringLiteral("none")) {
        if (m_player) m_player->stop();
        if (m_isPlaying) {
            m_isPlaying = false;
            emit playingChanged();
        }
        return;
    }

    const bool shouldPlay = m_timer && m_timer->isRunning() && !m_timer->isBreakActive();
    ensurePlayerInitialized();
    if (!m_player) {
        return;
    }

    if (shouldPlay) {
        if (m_player->playbackState() != QMediaPlayer::PlayingState) {
            m_player->play();
        }
        if (!m_isPlaying) {
            m_isPlaying = true;
            emit playingChanged();
        }
    } else {
        if (m_player->playbackState() == QMediaPlayer::PlayingState) {
            m_player->pause();
        }
        if (m_isPlaying) {
            m_isPlaying = false;
            emit playingChanged();
        }
    }
}

bool AmbientSoundService::generateAmbientWav(const QString& type, const QString& destinationPath) {
    constexpr int sampleRate = 44100;
    constexpr int numChannels = 1;
    constexpr int bitsPerSample = 16;
    constexpr double durationSec = 4.0;
    const int totalSamples = static_cast<int>(sampleRate * durationSec);
    const int dataSize = totalSamples * numChannels * (bitsPerSample / 8);

    QFile file(destinationPath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    // Write 44-byte standard RIFF WAVE header
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);

    file.write("RIFF", 4);
    out << quint32(36 + dataSize);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    out << quint32(16);             // Subchunk1Size for PCM
    out << quint16(1);              // AudioFormat: PCM
    out << quint16(numChannels);
    out << quint32(sampleRate);
    out << quint32(sampleRate * numChannels * bitsPerSample / 8); // ByteRate
    out << quint16(numChannels * bitsPerSample / 8);              // BlockAlign
    out << quint16(bitsPerSample);
    file.write("data", 4);
    out << quint32(dataSize);

    auto* rng = QRandomGenerator::global();
    double filterState = 0.0;

    for (int i = 0; i < totalSamples; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        double sample = 0.0;

        if (type == QStringLiteral("clock")) {
            // Mechanical clock tick at 0.0s, 1.0s, 2.0s, 3.0s
            const double subT = std::fmod(t, 1.0);
            if (subT < 0.04) {
                const double freq = (std::fmod(t, 2.0) < 1.0) ? 2400.0 : 1700.0;
                const double decay = std::exp(-subT * 120.0);
                sample = std::sin(2.0 * M_PI * freq * subT) * decay * 0.45;
            }
        } else if (type == QStringLiteral("rain")) {
            // Filtered pink noise + rain droplet spikes
            const double rawNoise = rng->generateDouble() * 2.0 - 1.0;
            filterState = filterState * 0.92 + rawNoise * 0.08;
            sample = filterState * 0.35;

            // Occasional rain drops
            if (rng->bounded(1000) < 6) {
                sample += (rng->generateDouble() * 0.4 - 0.2);
            }
        } else {
            // White / Pink Noise: smoothed random noise (comfortable focus hum)
            const double rawNoise = rng->generateDouble() * 2.0 - 1.0;
            filterState = filterState * 0.85 + rawNoise * 0.15;
            sample = filterState * 0.3;
        }

        const qint16 pcm16 = static_cast<qint16>(qBound(-1.0, sample, 1.0) * 32767.0);
        out << pcm16;
    }

    file.close();
    QFile::setPermissions(destinationPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

} // namespace gofi
