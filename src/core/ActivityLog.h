#pragma once

#include <QDateTime>
#include <QString>

namespace gofi {

class ActivityLog {
public:
    explicit ActivityLog(QString defaultLogFilePath = {});

    void setDefaultLogFilePath(const QString& path) { m_defaultLogPath = path; }
    [[nodiscard]] QString defaultLogFilePath() const { return m_defaultLogPath; }

    qint64 logTask(const QString& listId,
                   const QString& listName,
                   qint64 taskId,
                   const QString& jiraKey,
                   const QString& taskDescription,
                   const QDateTime& startTimeUtc,
                   quint32 runtimeSeconds,
                   const QDateTime& stopTimeUtc,
                   bool isPomodoroCompleted = false) const;

    static bool logTaskInFile(const QString& filePath,
                              const QString& listName,
                              const QString& taskDescription,
                              const QDateTime& startTimeUtc,
                              quint32 runtimeSeconds,
                              const QDateTime& stopTimeUtc);

private:
    QString m_defaultLogPath;
};

} // namespace gofi
