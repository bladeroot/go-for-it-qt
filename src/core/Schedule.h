#pragma once

#include <QVector>
#include <QVariantList>
#include <QtTypes>

namespace gofi {

class Schedule {
public:
    Schedule() = default;
    explicit Schedule(const QVector<int>& rawDurations);

    [[nodiscard]] int length() const noexcept { return static_cast<int>(m_durations.size() / 2); }
    [[nodiscard]] bool isValid() const noexcept { return length() > 0; }

    void append(int taskDurationSec, int breakDurationSec);
    void importRaw(const QVector<int>& durations);
    [[nodiscard]] const QVector<int>& exportRaw() const noexcept { return m_durations; }

    [[nodiscard]] int taskDuration(int iteration) const;
    [[nodiscard]] int breakDuration(int iteration) const;

    [[nodiscard]] QVariantList toVariantList() const;
    void fromVariantList(const QVariantList& list);

    bool operator==(const Schedule& other) const noexcept { return m_durations == other.m_durations; }
    bool operator!=(const Schedule& other) const noexcept { return !(*this == other); }

private:
    QVector<int> m_durations;
};

} // namespace gofi
