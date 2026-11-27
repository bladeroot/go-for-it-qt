#pragma once

#include "DatabaseManager.h"
#include <QDateTime>
#include <QDate>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace gofi {

class AnalyticsService : public QObject {
    Q_OBJECT

public:
    explicit AnalyticsService(QObject* parent = nullptr);

    // Productivity statistics for a given period ("day", "week", "month", "all")
    Q_INVOKABLE QVariantMap getProductivityStats(const QString& period = QStringLiteral("day"),
                                                const QString& listId = QString()) const;

    // Heatmap data for the last N days (default 90)
    Q_INVOKABLE QVariantList getActivityHeatmap(int daysBack = 90,
                                               const QString& listId = QString()) const;

    // Export billing reports to CSV or PDF
    Q_INVOKABLE bool exportBillingCsv(const QString& filePath,
                                      const QString& period = QStringLiteral("month"),
                                      const QString& listId = QString()) const;

    Q_INVOKABLE bool exportBillingPdf(const QString& filePath,
                                      const QString& period = QStringLiteral("month"),
                                      const QString& listId = QString()) const;

    // PDF rendering (QTextDocument on QPdfWriter) needs QFontDatabase, i.e. a QGuiApplication.
    // go-for-it-tui runs on a QCoreApplication, where it would abort the process.
    [[nodiscard]] static bool canExportPdf();

    // Helper utilities
    static QString formatDuration(quint64 totalSeconds);
    static QStringList extractProjects(const QString& text);
    static QStringList extractContexts(const QString& text);

    // "Today" for periods and the heatmap; tests set a fixed date, an invalid date means the real one.
    void setReferenceDate(const QDate& date) { m_referenceDate = date; }

private:
    std::pair<QDateTime, QDateTime> resolvePeriodRange(const QString& period) const;
    [[nodiscard]] QDate today() const { return m_referenceDate.isValid() ? m_referenceDate : QDate::currentDate(); }

    QDate m_referenceDate;
};

} // namespace gofi
