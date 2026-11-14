#include "AnalyticsService.h"
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMap>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextDocument>
#include <QTextStream>
#include <QTimeZone>
#include <algorithm>

namespace gofi {

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

AnalyticsService::AnalyticsService(QObject* parent)
    : QObject(parent)
{
}

QString AnalyticsService::formatDuration(quint64 totalSeconds) {
    const quint64 hours = totalSeconds / 3600ULL;
    const quint64 mins = (totalSeconds % 3600ULL) / 60ULL;
    if (hours > 0) {
        return QStringLiteral("%1h %2m").arg(hours).arg(mins, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1m").arg(mins);
}

QStringList AnalyticsService::extractProjects(const QString& text) {
    QStringList result;
    static const QRegularExpression projRe(QStringLiteral("(?:^|\\s)\\+([^\\s\\+@]+)"));
    auto it = projRe.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString p = match.captured(1).trimmed();
        if (!p.isEmpty() && !result.contains(p, Qt::CaseInsensitive)) {
            result.append(p);
        }
    }
    return result;
}

QStringList AnalyticsService::extractContexts(const QString& text) {
    QStringList result;
    static const QRegularExpression ctxRe(QStringLiteral("(?:^|\\s)@([^\\s\\+@]+)"));
    auto it = ctxRe.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        const QString c = match.captured(1).trimmed();
        if (!c.isEmpty() && !result.contains(c, Qt::CaseInsensitive)) {
            result.append(c);
        }
    }
    return result;
}

std::pair<QDateTime, QDateTime> AnalyticsService::resolvePeriodRange(const QString& period) const {
    const QString p = period.trimmed().toLower();
    const QDate today = QDate::currentDate();

    if (p == QStringLiteral("day") || p == QStringLiteral("today")) {
        const QDateTime start = QDateTime(today, QTime(0, 0, 0)).toUTC();
        const QDateTime end = QDateTime(today, QTime(23, 59, 59)).toUTC();
        return {start, end};
    }

    if (p == QStringLiteral("week") || p == QStringLiteral("this_week")) {
        const int dayOfWeek = today.dayOfWeek(); // 1=Mon .. 7=Sun
        const QDate monday = today.addDays(1 - dayOfWeek);
        const QDate sunday = today.addDays(7 - dayOfWeek);
        const QDateTime start = QDateTime(monday, QTime(0, 0, 0)).toUTC();
        const QDateTime end = QDateTime(sunday, QTime(23, 59, 59)).toUTC();
        return {start, end};
    }

    if (p == QStringLiteral("month") || p == QStringLiteral("this_month")) {
        const QDate firstDay(today.year(), today.month(), 1);
        const QDate lastDay(today.year(), today.month(), today.daysInMonth());
        const QDateTime start = QDateTime(firstDay, QTime(0, 0, 0)).toUTC();
        const QDateTime end = QDateTime(lastDay, QTime(23, 59, 59)).toUTC();
        return {start, end};
    }

    if (p == QStringLiteral("year") || p == QStringLiteral("this_year")) {
        const QDate firstDay(today.year(), 1, 1);
        const QDate lastDay(today.year(), 12, 31);
        const QDateTime start = QDateTime(firstDay, QTime(0, 0, 0)).toUTC();
        const QDateTime end = QDateTime(lastDay, QTime(23, 59, 59)).toUTC();
        return {start, end};
    }

    // "all"
    return {QDateTime(), QDateTime()};
}

QVariantMap AnalyticsService::getProductivityStats(const QString& period, const QString& listId) const {
    const auto [startUtc, endUtc] = resolvePeriodRange(period);
    const QVector<ActivityRecord> records = DatabaseManager::instance().loadActivityLogs(startUtc, endUtc, listId);

    quint64 totalSeconds = 0;
    int completedPomodoros = 0;
    const int totalSessions = records.size();

    QMap<QString, quint64> projectMap;
    QMap<QString, quint64> contextMap;

    struct TaskAgg {
        QString listId;
        QString description;
        QString jiraKey;
        QString listName;
        quint64 seconds = 0;
        int sessions = 0;
        QDateTime firstStart;
        QDateTime lastStop;
    };
    QMap<QString, TaskAgg> taskMap;
    QMap<QDate, quint64> dailySeconds;
    QMap<QDate, int> dailyPomodoros;

    for (const ActivityRecord& r : records) {
        totalSeconds += r.runtimeSeconds;
        if (r.isPomodoroCompleted || r.runtimeSeconds >= 1200) {
            completedPomodoros++;
        }

        const QDate localDate = r.startTimeUtc.toLocalTime().date();
        dailySeconds[localDate] += r.runtimeSeconds;
        if (r.isPomodoroCompleted || r.runtimeSeconds >= 1200) {
            dailyPomodoros[localDate]++;
        }

        // Projects
        const QStringList projs = extractProjects(r.taskDescription);
        if (!projs.isEmpty()) {
            for (const QString& pr : projs) {
                projectMap[pr] += r.runtimeSeconds;
            }
        } else {
            const QString fallbackProj = !r.listName.isEmpty() ? r.listName : QStringLiteral("General");
            projectMap[fallbackProj] += r.runtimeSeconds;
        }

        // Contexts
        const QStringList ctxs = extractContexts(r.taskDescription);
        if (!ctxs.isEmpty()) {
            for (const QString& cx : ctxs) {
                contextMap[cx] += r.runtimeSeconds;
            }
        } else {
            contextMap[QStringLiteral("default")] += r.runtimeSeconds;
        }

        // Task Aggregation
        const QString taskIdentifier = (!r.jiraKey.isEmpty() ? r.jiraKey : r.taskDescription.trimmed());
        const QString aggKey = r.listId + QLatin1Char('\x1f') + taskIdentifier;
        TaskAgg& item = taskMap[aggKey];
        item.listId = r.listId;
        item.description = r.taskDescription;
        item.jiraKey = r.jiraKey;
        item.listName = r.listName;
        item.seconds += r.runtimeSeconds;
        item.sessions++;
        if (!item.firstStart.isValid() || r.startTimeUtc < item.firstStart) {
            item.firstStart = r.startTimeUtc;
        }
        if (!item.lastStop.isValid() || r.stopTimeUtc > item.lastStop) {
            item.lastStop = r.stopTimeUtc;
        }
    }

    // Format Projects Breakdown
    QVariantList projectBreakdown;
    for (auto it = projectMap.cbegin(); it != projectMap.cend(); ++it) {
        const double pct = totalSeconds > 0 ? (static_cast<double>(it.value()) * 100.0 / static_cast<double>(totalSeconds)) : 0.0;
        projectBreakdown.append(QVariantMap{
            {QStringLiteral("name"), it.key()},
            {QStringLiteral("seconds"), static_cast<double>(it.value())},
            {QStringLiteral("formattedTime"), formatDuration(it.value())},
            {QStringLiteral("hours"), static_cast<double>(it.value()) / 3600.0},
            {QStringLiteral("percentage"), pct}
        });
    }
    std::sort(projectBreakdown.begin(), projectBreakdown.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value(QStringLiteral("seconds")).toDouble() > b.toMap().value(QStringLiteral("seconds")).toDouble();
    });

    // Format Contexts Breakdown
    QVariantList contextBreakdown;
    for (auto it = contextMap.cbegin(); it != contextMap.cend(); ++it) {
        const double pct = totalSeconds > 0 ? (static_cast<double>(it.value()) * 100.0 / static_cast<double>(totalSeconds)) : 0.0;
        contextBreakdown.append(QVariantMap{
            {QStringLiteral("name"), it.key()},
            {QStringLiteral("seconds"), static_cast<double>(it.value())},
            {QStringLiteral("formattedTime"), formatDuration(it.value())},
            {QStringLiteral("hours"), static_cast<double>(it.value()) / 3600.0},
            {QStringLiteral("percentage"), pct}
        });
    }
    std::sort(contextBreakdown.begin(), contextBreakdown.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value(QStringLiteral("seconds")).toDouble() > b.toMap().value(QStringLiteral("seconds")).toDouble();
    });

    // Format Task Breakdown
    QVariantList taskBreakdown;
    for (auto it = taskMap.cbegin(); it != taskMap.cend(); ++it) {
        const TaskAgg& t = it.value();
        const double hours = static_cast<double>(t.seconds) / 3600.0;
        taskBreakdown.append(QVariantMap{
            {QStringLiteral("key"), it.key()},
            {QStringLiteral("listId"), t.listId},
            {QStringLiteral("description"), t.description},
            {QStringLiteral("jiraKey"), t.jiraKey},
            {QStringLiteral("listName"), t.listName},
            {QStringLiteral("seconds"), static_cast<double>(t.seconds)},
            {QStringLiteral("formattedTime"), formatDuration(t.seconds)},
            {QStringLiteral("hours"), hours},
            {QStringLiteral("sessions"), t.sessions},
            {QStringLiteral("firstStart"), t.firstStart.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))},
            {QStringLiteral("lastStop"), t.lastStop.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))}
        });
    }
    std::sort(taskBreakdown.begin(), taskBreakdown.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value(QStringLiteral("seconds")).toDouble() > b.toMap().value(QStringLiteral("seconds")).toDouble();
    });

    QVariantMap res;
    res[QStringLiteral("period")] = period;
    res[QStringLiteral("totalSeconds")] = static_cast<double>(totalSeconds);
    res[QStringLiteral("totalFormattedTime")] = formatDuration(totalSeconds);
    res[QStringLiteral("totalHours")] = static_cast<double>(totalSeconds) / 3600.0;
    res[QStringLiteral("completedPomodoros")] = completedPomodoros;
    res[QStringLiteral("totalSessions")] = totalSessions;
    res[QStringLiteral("activeTaskCount")] = taskBreakdown.size();
    res[QStringLiteral("projectBreakdown")] = projectBreakdown;
    res[QStringLiteral("contextBreakdown")] = contextBreakdown;
    res[QStringLiteral("taskBreakdown")] = taskBreakdown;

    return res;
}

QVariantList AnalyticsService::getActivityHeatmap(int daysBack, const QString& listId) const {
    QVariantList list;
    const int numDays = qBound(7, daysBack, 365);
    const QDate today = QDate::currentDate();
    const QDate nominalStart = today.addDays(-(numDays - 1));
    const QDate startDate = nominalStart.addDays(-(nominalStart.dayOfWeek() - 1));

    const QDateTime startUtc = QDateTime(startDate, QTime(0, 0, 0)).toUTC();
    const QDateTime endUtc = QDateTime(today, QTime(23, 59, 59)).toUTC();

    const QVector<ActivityRecord> records = DatabaseManager::instance().loadActivityLogs(startUtc, endUtc, listId);

    QMap<QDate, quint64> dateSeconds;
    QMap<QDate, int> datePomodoros;

    for (const ActivityRecord& r : records) {
        const QDate localDate = r.startTimeUtc.toLocalTime().date();
        dateSeconds[localDate] += r.runtimeSeconds;
        if (r.isPomodoroCompleted || r.runtimeSeconds >= 1200) {
            datePomodoros[localDate]++;
        }
    }

    const int totalDays = static_cast<int>(startDate.daysTo(today)) + 1;
    for (int i = 0; i < totalDays; ++i) {
        const QDate d = startDate.addDays(i);
        const quint64 sec = dateSeconds.value(d, 0);
        const int pom = datePomodoros.value(d, 0);

        int intensity = 0;
        if (sec > 0 && sec < 1800) intensity = 1;
        else if (sec >= 1800 && sec < 3600) intensity = 2;
        else if (sec >= 3600 && sec < 9000) intensity = 3;
        else if (sec >= 9000) intensity = 4;

        QVariantMap entry;
        entry[QStringLiteral("date")] = d.toString(Qt::ISODate);
        entry[QStringLiteral("dayOfWeek")] = d.dayOfWeek(); // 1=Mon .. 7=Sun
        entry[QStringLiteral("dayName")] = QLocale().dayName(d.dayOfWeek(), QLocale::ShortFormat);
        entry[QStringLiteral("monthName")] = QLocale().monthName(d.month(), QLocale::ShortFormat);
        entry[QStringLiteral("dayOfMonth")] = d.day();
        entry[QStringLiteral("seconds")] = static_cast<double>(sec);
        entry[QStringLiteral("formattedTime")] = formatDuration(sec);
        entry[QStringLiteral("pomodoros")] = pom;
        entry[QStringLiteral("intensity")] = intensity;

        list.append(entry);
    }
    return list;
}

bool AnalyticsService::exportBillingCsv(const QString& filePath,
                                       const QString& period,
                                       const QString& listId) const
{
    if (filePath.trimmed().isEmpty()) return false;

    const QFileInfo targetInfo(filePath);
    if (targetInfo.isDir()) return false;
    if (!targetInfo.dir().exists()) {
        targetInfo.dir().mkpath(QStringLiteral("."));
    }

    const QVariantMap stats = getProductivityStats(period, listId);
    const QVariantList tasks = stats.value(QStringLiteral("taskBreakdown")).toList();

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    // CSV Header
    out << "\"Task Description\",\"Issue Key\",\"List\",\"Projects\",\"Contexts\",\"Duration\",\"Total Hours\",\"Sessions Count\",\"First Worked\",\"Last Worked\"\n";

    for (const QVariant& tv : tasks) {
        const QVariantMap m = tv.toMap();
        const QString desc = escapeCsvField(m.value(QStringLiteral("description")).toString());
        const QString key = escapeCsvField(m.value(QStringLiteral("jiraKey")).toString());
        const QString listName = escapeCsvField(m.value(QStringLiteral("listName")).toString());
        const QString duration = m.value(QStringLiteral("formattedTime")).toString();
        const double hours = m.value(QStringLiteral("hours")).toDouble();
        const int sessions = m.value(QStringLiteral("sessions")).toInt();
        const QString first = m.value(QStringLiteral("firstStart")).toString();
        const QString last = m.value(QStringLiteral("lastStop")).toString();

        const QString projs = escapeCsvField(extractProjects(m.value(QStringLiteral("description")).toString()).join(QStringLiteral(", ")));
        const QString ctxs = escapeCsvField(extractContexts(m.value(QStringLiteral("description")).toString()).join(QStringLiteral(", ")));

        out << "\"" << desc << "\",\""
            << key << "\",\""
            << listName << "\",\""
            << projs << "\",\""
            << ctxs << "\",\""
            << duration << "\","
            << QString::number(hours, 'f', 2) << ","
            << sessions << ",\""
            << first << "\",\""
            << last << "\"\n";
    }

    // Summary Row
    const double totalHours = stats.value(QStringLiteral("totalHours")).toDouble();
    const QString totalDuration = stats.value(QStringLiteral("totalFormattedTime")).toString();
    const int totalSessions = stats.value(QStringLiteral("totalSessions")).toInt();

    out << "\"TOTAL SUMMARY\",\"\",\"\",\"\",\"\",\""
        << totalDuration << "\","
        << QString::number(totalHours, 'f', 2) << ","
        << totalSessions << ",\"\",\"\n";

    out.flush();
    if (!file.commit()) {
        return false;
    }
    if (!QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return false;
    }

    return true;
}

bool AnalyticsService::exportBillingPdf(const QString& filePath,
                                       const QString& period,
                                       const QString& listId) const
{
    if (filePath.trimmed().isEmpty()) return false;

    const QVariantMap stats = getProductivityStats(period, listId);
    const QVariantList tasks = stats.value(QStringLiteral("taskBreakdown")).toList();
    const QVariantList projs = stats.value(QStringLiteral("projectBreakdown")).toList();

    const QString totalDuration = stats.value(QStringLiteral("totalFormattedTime")).toString();
    const double totalHours = stats.value(QStringLiteral("totalHours")).toDouble();
    const int completedPoms = stats.value(QStringLiteral("completedPomodoros")).toInt();
    const int totalSessions = stats.value(QStringLiteral("totalSessions")).toInt();
    const QString generatedAt = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));

    QString html;
    html += QStringLiteral(
        "<html><head><meta charset=\"utf-8\"><style>"
        "body { font-family: 'Helvetica Neue', Helvetica, Arial, sans-serif; color: #2c3e50; margin: 20px; font-size: 11pt; }"
        "h1 { color: #1a5276; margin-bottom: 4px; font-size: 22pt; }"
        "h2 { color: #2980b9; margin-top: 24px; margin-bottom: 8px; font-size: 14pt; border-bottom: 2px solid #3498db; padding-bottom: 4px; }"
        ".subtitle { color: #7f8c8d; font-size: 11pt; margin-bottom: 20px; }"
        ".kpi-table { width: 100%; margin-bottom: 25px; border-collapse: collapse; }"
        ".kpi-box { background-color: #ebf5fb; border: 1px solid #aed6f1; border-radius: 6px; padding: 12px; text-align: center; width: 25%; }"
        ".kpi-value { font-size: 18pt; font-weight: bold; color: #1b4f72; }"
        ".kpi-label { font-size: 10pt; color: #5d6d7e; text-transform: uppercase; margin-top: 4px; }"
        ".data-table { width: 100%; border-collapse: collapse; margin-top: 10px; }"
        ".data-table th { background-color: #2e86c1; color: #ffffff; padding: 8px 10px; text-align: left; font-size: 10pt; }"
        ".data-table td { padding: 8px 10px; border-bottom: 1px solid #d6dbdf; font-size: 10pt; }"
        ".data-table tr:nth-child(even) { background-color: #f8f9f9; }"
        ".total-row { font-weight: bold; background-color: #eaf2f8 !important; }"
        ".badge { display: inline-block; padding: 2px 6px; font-size: 8pt; border-radius: 3px; font-weight: bold; color: #ffffff; background-color: #5d6d7e; }"
        ".footer { margin-top: 35px; font-size: 9pt; color: #95a5a6; text-align: center; border-top: 1px solid #e5e7e9; padding-top: 10px; }"
        "</style></head><body>"
    );

    // Title & Subtitle
    html += QStringLiteral("<h1>⏱ Go for It — Time & Productivity Billing Report</h1>");
    html += QStringLiteral("<div class=\"subtitle\">Period: <strong>%1</strong> | Generated: <strong>%2</strong></div>")
                .arg(period.toUpper(), generatedAt);

    // KPI Cards Table
    html += QStringLiteral(
        "<table class=\"kpi-table\"><tr>"
        "<td class=\"kpi-box\"><div class=\"kpi-value\">%1</div><div class=\"kpi-label\">Total Focus Time</div></td><td style=\"width:10px;\"></td>"
        "<td class=\"kpi-box\"><div class=\"kpi-value\">%2</div><div class=\"kpi-label\">Total Hours</div></td><td style=\"width:10px;\"></td>"
        "<td class=\"kpi-box\"><div class=\"kpi-value\">%3</div><div class=\"kpi-label\">Completed Pomodoros</div></td><td style=\"width:10px;\"></td>"
        "<td class=\"kpi-box\"><div class=\"kpi-value\">%4</div><div class=\"kpi-label\">Work Sessions</div></td>"
        "</tr></table>"
    ).arg(totalDuration, QString::number(totalHours, 'f', 2), QString::number(completedPoms), QString::number(totalSessions));

    // Projects Breakdown Table
    if (!projs.isEmpty()) {
        html += QStringLiteral("<h2>📁 Project Distribution (+Project)</h2>");
        html += QStringLiteral("<table class=\"data-table\"><tr><th>Project Name</th><th>Time Spent</th><th>Hours</th><th>Share</th></tr>");
        for (const QVariant& pv : projs) {
            const QVariantMap pm = pv.toMap();
            html += QStringLiteral("<tr><td><strong>+%1</strong></td><td>%2</td><td>%3h</td><td>%4%</td></tr>")
                .arg(pm.value(QStringLiteral("name")).toString(),
                     pm.value(QStringLiteral("formattedTime")).toString(),
                     QString::number(pm.value(QStringLiteral("hours")).toDouble(), 'f', 2),
                     QString::number(pm.value(QStringLiteral("percentage")).toDouble(), 'f', 1));
        }
        html += QStringLiteral("</table>");
    }

    // Tasks Detailed Billing Table
    html += QStringLiteral("<h2>📋 Detailed Task Billing Log</h2>");
    html += QStringLiteral("<table class=\"data-table\"><tr><th>Task Description</th><th>Key / ID</th><th>List</th><th>Sessions</th><th>Time Spent</th><th>Hours</th></tr>");

    for (const QVariant& tv : tasks) {
        const QVariantMap tm = tv.toMap();
        const QString desc = tm.value(QStringLiteral("description")).toString().toHtmlEscaped();
        const QString key = tm.value(QStringLiteral("jiraKey")).toString();
        const QString keyBadge = !key.isEmpty() ? QStringLiteral("<span class=\"badge\">%1</span>").arg(key.toHtmlEscaped()) : QStringLiteral("-");
        const QString listName = tm.value(QStringLiteral("listName")).toString().toHtmlEscaped();
        const int sessions = tm.value(QStringLiteral("sessions")).toInt();
        const QString duration = tm.value(QStringLiteral("formattedTime")).toString();
        const double h = tm.value(QStringLiteral("hours")).toDouble();

        html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td><strong>%5</strong></td><td>%6h</td></tr>")
            .arg(desc, keyBadge, listName, QString::number(sessions), duration, QString::number(h, 'f', 2));
    }

    // Total Row
    html += QStringLiteral("<tr class=\"total-row\"><td colspan=\"3\">TOTAL BILLABLE TIME</td><td>%1</td><td>%2</td><td>%3h</td></tr>")
        .arg(QString::number(totalSessions), totalDuration, QString::number(totalHours, 'f', 2));
    html += QStringLiteral("</table>");

    // Footer
    html += QStringLiteral("<div class=\"footer\">Report automatically generated by Go for It Qt Productivity & Time Tracking Engine</div>");
    html += QStringLiteral("</body></html>");

    const QFileInfo targetInfo(filePath);
    if (targetInfo.isDir()) return false;
    if (!targetInfo.dir().exists()) {
        targetInfo.dir().mkpath(QStringLiteral("."));
    }

    QSaveFile saveFile(filePath);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        return false;
    }
    saveFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    {
        QPdfWriter writer(&saveFile);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setPageOrientation(QPageLayout::Portrait);
        writer.setPageMargins(QMarginsF(12, 12, 12, 12), QPageLayout::Millimeter);

        QTextDocument doc;
        doc.setDocumentMargin(0);
        doc.setPageSize(QSizeF(writer.width(), writer.height()));
        doc.setHtml(html);
        doc.print(&writer);
    }

    if (!saveFile.commit()) {
        return false;
    }
    if (!QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        return false;
    }
    const QFileInfo fi(filePath);
    if (!fi.exists() || fi.size() == 0) {
        return false;
    }

    return true;
}

} // namespace gofi
