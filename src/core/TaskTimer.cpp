#include "TaskTimer.h"

namespace gofi {

TaskTimer::TaskTimer(SettingsManager* settings, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
{
    m_tickTimer.setInterval(500);
    connect(&m_tickTimer, &QTimer::timeout, this, &TaskTimer::onTick);

    if (m_settings) {
        connect(m_settings, &SettingsManager::timerDurationChanged, this, &TaskTimer::onSettingsDurationChanged);
    }
    reset();
}

uint TaskTimer::remainingSeconds() const {
    qint64 totalRuntimeMs = m_previousRuntimeMs;
    if (m_running && m_monotonicTimer.isValid()) {
        totalRuntimeMs += m_monotonicTimer.elapsed();
    }
    const qint64 remMs = qMax<qint64>(0, m_iterationDurationMs - totalRuntimeMs);
    return static_cast<uint>((remMs + 500) / 1000);
}

void TaskTimer::setRemainingSeconds(uint seconds) {
    if (m_running) {
        return;
    }
    m_iterationDurationMs = static_cast<qint64>(seconds) * 1000LL + m_previousRuntimeMs;
    updateState();
}

QString TaskTimer::formattedRemainingTime() const {
    const uint total = remainingSeconds();
    const uint h = total / 3600U;
    const uint m = (total / 60U) % 60U;
    const uint s = total % 60U;
    return QStringLiteral("%1 : %2 : %3")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
}

void TaskTimer::setListTimerOverrides(const Schedule& overrideSchedule, int overrideReminderSec) {
    m_overrideSchedule = overrideSchedule;
    m_overrideReminderTime = overrideReminderSec;
    m_iteration = 0;
    if (!m_running) {
        reset();
    }
}

void TaskTimer::setActiveTask(const QString& description, uint accumulatedSeconds, uint estimatedDurationSeconds) {
    const bool changed = (m_activeTaskDescription != description);
    if (changed) {
        stop();
        if (m_settings && m_settings->resetTimerOnTaskSwitch()) {
            reset();
        }
    }
    m_activeTaskDescription = description;
    m_activeTaskTimerSec = accumulatedSeconds;
    m_activeTaskDurationSec = estimatedDurationSeconds;
    m_durationExceededSent = (estimatedDurationSeconds == 0 || estimatedDurationSeconds <= accumulatedSeconds);
    emit activeTaskChanged();
}

void TaskTimer::clearActiveTask() {
    stop();
    m_activeTaskDescription.clear();
    m_activeTaskTimerSec = 0;
    m_activeTaskDurationSec = 0;
    m_durationExceededSent = false;
    emit activeTaskChanged();
}

void TaskTimer::start() {
    if (m_running || !hasActiveTask()) {
        return;
    }
    m_startUtc = QDateTime::currentDateTimeUtc();
    m_monotonicTimer.start();
    m_lastTaskSyncMs = 0;
    m_almostOverSent = false;
    m_running = true;
    m_tickTimer.start();
    emit runningChanged();
    updateState();
}

void TaskTimer::stop() {
    if (!m_running) {
        return;
    }
    const qint64 elapsedMs = m_monotonicTimer.isValid() ? m_monotonicTimer.elapsed() : 0;
    m_previousRuntimeMs += elapsedMs;

    if (!m_breakActive) {
        const qint64 unsyncedMs = elapsedMs - m_lastTaskSyncMs;
        if (unsyncedMs > 0) {
            m_activeTaskTimerSec += static_cast<uint>((unsyncedMs + 500) / 1000);
            emit activeTaskTimeUpdated(m_activeTaskTimerSec);
        }
    }

    m_tickTimer.stop();
    m_running = false;
    emit runningChanged();
    emit timerStopped(m_startUtc, static_cast<uint>((elapsedMs + 500) / 1000));
    updateState();
}

void TaskTimer::toggleRunning() {
    if (m_running) {
        stop();
    } else {
        start();
    }
}

void TaskTimer::reset() {
    const Schedule sched = effectiveSchedule();
    const int defSec = m_breakActive
        ? sched.breakDuration(m_iteration)
        : sched.taskDuration(m_iteration);
    m_iterationDurationMs = static_cast<qint64>(defSec) * 1000LL;
    m_previousRuntimeMs = 0;
    m_lastTaskSyncMs = 0;
    updateState();
}

void TaskTimer::skipIteration() {
    emit timerFinished(m_breakActive);
    stop();
    toggleBreakState();
}

void TaskTimer::setTimeHMS(int hours, int minutes, int seconds) {
    const int total = qMax(0, hours) * 3600 + qMax(0, minutes) * 60 + qMax(0, seconds);
    setRemainingSeconds(static_cast<uint>(total));
}

void TaskTimer::onTick() {
    updateState();
}

void TaskTimer::onSettingsDurationChanged() {
    if (!m_running) {
        reset();
    }
}

Schedule TaskTimer::effectiveSchedule() const {
    if (m_overrideSchedule.isValid()) {
        return m_overrideSchedule;
    }
    return m_settings ? m_settings->schedule() : Schedule({1500, 300});
}

int TaskTimer::effectiveReminderTime() const {
    if (m_overrideReminderTime >= 0) {
        return m_overrideReminderTime;
    }
    return m_settings ? m_settings->reminderTime() : 60;
}

void TaskTimer::updateState() {
    qint64 totalRuntimeMs = m_previousRuntimeMs;
    if (m_running && m_monotonicTimer.isValid()) {
        const qint64 elapsedMs = m_monotonicTimer.elapsed();
        totalRuntimeMs += elapsedMs;

        if (m_iterationDurationMs - totalRuntimeMs <= 0) {
            skipIteration();
            return;
        }

        if (!m_breakActive) {
            const qint64 diffMs = elapsedMs - m_lastTaskSyncMs;
            if (diffMs >= 60000LL) {
                const uint addedSec = static_cast<uint>(diffMs / 1000LL);
                m_lastTaskSyncMs += static_cast<qint64>(addedSec) * 1000LL;
                m_activeTaskTimerSec += addedSec;
                emit activeTaskTimeUpdated(m_activeTaskTimerSec);

                if (!m_durationExceededSent && m_activeTaskDurationSec > 0 && m_activeTaskTimerSec >= m_activeTaskDurationSec) {
                    m_durationExceededSent = true;
                    emit taskDurationExceeded();
                }
            }

            const int remSec = effectiveReminderTime();
            if (remSec > 0 && !m_almostOverSent) {
                const qint64 remMs = m_iterationDurationMs - totalRuntimeMs;
                if (remMs <= static_cast<qint64>(remSec) * 1000LL) {
                    m_almostOverSent = true;
                    emit timerAlmostOver(remainingSeconds());
                }
            }
        }
    }

    if (m_iterationDurationMs > 0) {
        m_progress = qBound(0.0, static_cast<double>(totalRuntimeMs) / static_cast<double>(m_iterationDurationMs), 1.0);
    } else {
        m_progress = 1.0;
    }

    emit timerUpdated();
}

void TaskTimer::toggleBreakState() {
    if (m_breakActive) {
        ++m_iteration;
    }
    m_breakActive = !m_breakActive;
    emit breakActiveChanged();
    reset();
    if (m_breakActive || (m_settings && m_settings->resumeTasksAfterBreak())) {
        start();
    }
}

} // namespace gofi
