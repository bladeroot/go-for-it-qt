#include "Schedule.h"
#include <QtMath>
#include <limits>

namespace gofi {

namespace {

int normalizeDuration(int sec) noexcept {
    if (sec == std::numeric_limits<int>::min()) {
        return std::numeric_limits<int>::max();
    }
    return qAbs(sec);
}

} // namespace

Schedule::Schedule(const QVector<int>& rawDurations) {
    importRaw(rawDurations);
}

void Schedule::append(int taskDurationSec, int breakDurationSec) {
    m_durations.append(normalizeDuration(taskDurationSec));
    m_durations.append(normalizeDuration(breakDurationSec));
}

void Schedule::importRaw(const QVector<int>& durations) {
    int count = static_cast<int>(durations.size());
    if (count % 2 == 1) {
        count -= 1;
    }
    m_durations.resize(count);
    for (int i = 0; i < count; ++i) {
        m_durations[i] = normalizeDuration(durations[i]);
    }
}

int Schedule::taskDuration(int iteration) const {
    if (!isValid()) {
        return 1500;
    }
    int idx = (qMax(0, iteration) % length()) * 2;
    return normalizeDuration(m_durations[idx]);
}

int Schedule::breakDuration(int iteration) const {
    if (!isValid()) {
        return 300;
    }
    int idx = (qMax(0, iteration) % length()) * 2 + 1;
    return normalizeDuration(m_durations[idx]);
}

QVariantList Schedule::toVariantList() const {
    QVariantList out;
    out.reserve(m_durations.size());
    for (int val : m_durations) {
        out.append(val);
    }
    return out;
}

void Schedule::fromVariantList(const QVariantList& list) {
    QVector<int> raw;
    raw.reserve(list.size());
    for (const QVariant& v : list) {
        raw.append(v.toInt());
    }
    importRaw(raw);
}

} // namespace gofi
