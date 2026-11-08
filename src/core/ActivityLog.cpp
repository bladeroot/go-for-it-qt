#include "ActivityLog.h"
#include "DatabaseManager.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace gofi {

ActivityLog::ActivityLog(QString defaultLogFilePath)
    : m_defaultLogPath(std::move(defaultLogFilePath))
{
}

qint64 ActivityLog::logTask(const QString& listId,
                            const QString& listName,
                            qint64 taskId,
                            const QString& jiraKey,
                            const QString& taskDescription,
                            const QDateTime& startTimeUtc,
                            quint32 runtimeSeconds,
                            const QDateTime& stopTimeUtc,
                            bool isPomodoroCompleted) const
{
    const qint64 logId = DatabaseManager::instance().insertActivityLog(
        listId, listName, taskId, jiraKey, taskDescription, startTimeUtc, runtimeSeconds, stopTimeUtc, false, isPomodoroCompleted
    );
    if (!m_defaultLogPath.isEmpty()) {
        logTaskInFile(m_defaultLogPath, listName, taskDescription, startTimeUtc, runtimeSeconds, stopTimeUtc);
    }
    return logId;
}

namespace {
QString escapeCsvField(QString val) {
    val.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    if (!val.isEmpty()) {
        const QChar first = val.at(0);
        if (first == QLatin1Char('=') || first == QLatin1Char('+') || first == QLatin1Char('-') ||
            first == QLatin1Char('@') || first == QLatin1Char('\t') || first == QLatin1Char('\r')) {
            val.prepend(QLatin1Char('\''));
        }
    }
    return val;
}
} // namespace

bool ActivityLog::logTaskInFile(const QString& filePath,
                                const QString& listName,
                                const QString& taskDescription,
                                const QDateTime& startTimeUtc,
                                quint32 runtimeSeconds,
                                const QDateTime& stopTimeUtc)
{
    if (filePath.isEmpty()) {
        return false;
    }

    QFileInfo info(filePath);
    QDir dir = info.dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return false;
    }

    const bool writeHeader = !info.exists() || info.size() == 0;
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return false;
    }

    QTextStream out(&file);
    if (writeHeader) {
        out << "list name,task description,run time,timer started,timer stopped\r\n";
    }

    const QString escapedList = escapeCsvField(listName);
    const QString escapedDesc = escapeCsvField(taskDescription);

    out << '"' << escapedList << "\","
        << '"' << escapedDesc << "\","
        << runtimeSeconds << ','
        << startTimeUtc.toLocalTime().toString(Qt::ISODate) << ','
        << stopTimeUtc.toLocalTime().toString(Qt::ISODate) << "\r\n";
    return true;
}

} // namespace gofi
