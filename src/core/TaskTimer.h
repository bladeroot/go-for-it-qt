#pragma once

#include "Schedule.h"
#include "SettingsManager.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace gofi {

class TaskTimer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(bool breakActive READ isBreakActive NOTIFY breakActiveChanged)
    Q_PROPERTY(uint remainingSeconds READ remainingSeconds WRITE setRemainingSeconds NOTIFY timerUpdated)
    Q_PROPERTY(double progress READ progress NOTIFY timerUpdated)
    Q_PROPERTY(QString formattedRemainingTime READ formattedRemainingTime NOTIFY timerUpdated)
    Q_PROPERTY(bool hasActiveTask READ hasActiveTask NOTIFY activeTaskChanged)
    Q_PROPERTY(qint64 activeTaskId READ activeTaskId NOTIFY activeTaskChanged)
    Q_PROPERTY(QString activeTaskDescription READ activeTaskDescription NOTIFY activeTaskChanged)
    Q_PROPERTY(uint activeTaskTimerValue READ activeTaskTimerValue NOTIFY activeTaskTimeUpdated)
    Q_PROPERTY(uint activeTaskDuration READ activeTaskDuration NOTIFY activeTaskChanged)

public:
    explicit TaskTimer(SettingsManager* settings, QObject* parent = nullptr);

    [[nodiscard]] bool isRunning() const noexcept { return m_running; }
    [[nodiscard]] bool isBreakActive() const noexcept { return m_breakActive; }
    [[nodiscard]] uint remainingSeconds() const;
    void setRemainingSeconds(uint seconds);
    [[nodiscard]] double progress() const noexcept { return m_progress; }
    [[nodiscard]] QString formattedRemainingTime() const;

    [[nodiscard]] qint64 activeTaskId() const noexcept { return m_activeTaskId; }
    [[nodiscard]] bool hasActiveTask() const noexcept { return m_activeTaskId >= 0 || !m_activeTaskDescription.isEmpty(); }
    [[nodiscard]] QString activeTaskDescription() const { return m_activeTaskDescription; }
    [[nodiscard]] uint activeTaskTimerValue() const noexcept { return m_activeTaskTimerSec; }
    [[nodiscard]] uint activeTaskDuration() const noexcept { return m_activeTaskDurationSec; }
    [[nodiscard]] uint iterationDurationSeconds() const noexcept {
        return static_cast<uint>(qMax<qint64>(0, m_iterationDurationMs / 1000LL));
    }

    [[nodiscard]] const Schedule& overrideSchedule() const noexcept { return m_overrideSchedule; }
    [[nodiscard]] int overrideReminderTime() const noexcept { return m_overrideReminderTime; }

    void setListTimerOverrides(const Schedule& overrideSchedule, int overrideReminderSec, bool forceReset = true);
    void setActiveTask(qint64 taskId, const QString& description, uint accumulatedSeconds, uint estimatedDurationSeconds);
    void setActiveTask(const QString& description, uint accumulatedSeconds, uint estimatedDurationSeconds);
    void clearActiveTask();
    uint flushActiveTaskSeconds();

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void toggleRunning();
    Q_INVOKABLE void reset();
    Q_INVOKABLE void skipIteration();
    Q_INVOKABLE void setTimeHMS(int hours, int minutes, int seconds);

signals:
    void runningChanged();
    void breakActiveChanged();
    void workStarted();
    void breakStarted();
    void timerUpdated();
    void activeTaskChanged();
    void activeTaskTimeUpdated(uint totalSeconds);
    void timerStopped(const QDateTime& startTimeUtc, uint runtimeSeconds);
    void timerAlmostOver(uint remainingSeconds);
    void timerFinished(bool wasBreak);
    void taskDurationExceeded();

private slots:
    void onTick();
    void onSettingsDurationChanged();

private:
    [[nodiscard]] Schedule effectiveSchedule() const;
    [[nodiscard]] int effectiveReminderTime() const;
    void updateState();
    void toggleBreakState();

    SettingsManager* m_settings = nullptr;
    Schedule m_overrideSchedule;
    int m_overrideReminderTime = -1;

    QTimer m_tickTimer;
    QElapsedTimer m_monotonicTimer;
    QDateTime m_startUtc;

    qint64 m_iterationDurationMs = 1500LL * 1000LL;
    qint64 m_previousRuntimeMs = 0;
    qint64 m_lastTaskSyncMs = 0;
    qint64 m_accumulatedMsRemainder = 0;
    qint64 m_accumulatedSessionMsRemainder = 0;
    double m_progress = 0.0;

    qint64 m_activeTaskId = -1;
    QString m_activeTaskDescription;
    uint m_activeTaskTimerSec = 0;
    uint m_activeTaskDurationSec = 0;
    int m_iteration = 0;
    bool m_running = false;
    bool m_breakActive = false;
    bool m_almostOverSent = false;
    bool m_durationExceededSent = false;
};

} // namespace gofi
