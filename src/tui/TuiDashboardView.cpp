#include "tui/TuiDashboardView.h"
#include "tui/TuiUtils.h"
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>
#include <algorithm>

namespace gofi {

using namespace tui;

TuiDashboardView::TuiDashboardView(AppController* controller)
    : m_app(controller) {
}

QString TuiDashboardView::defaultBillingExportPath(const QString& filename) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (dir.isEmpty() || !QDir(dir).exists()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    if (dir.isEmpty() || !QDir(dir).exists()) {
        dir = QDir::homePath();
    }
    if (!dir.isEmpty()) {
        QDir().mkpath(dir);
        return QDir(dir).filePath(filename);
    }
    return filename;
}

QString TuiDashboardView::periodName() const {
    switch (m_period) {
    case Period::Day: return QStringLiteral("day");
    case Period::Week: return QStringLiteral("week");
    case Period::Month: return QStringLiteral("month");
    case Period::All: return QStringLiteral("all");
    }
    return QStringLiteral("day");
}

QString TuiDashboardView::periodTitle() const {
    switch (m_period) {
    case Period::Day: return QCoreApplication::translate("TuiApp", "TODAY");
    case Period::Week: return QCoreApplication::translate("TuiApp", "THIS WEEK");
    case Period::Month: return QCoreApplication::translate("TuiApp", "THIS MONTH");
    case Period::All: return QCoreApplication::translate("TuiApp", "ALL TIME");
    }
    return QCoreApplication::translate("TuiApp", "TODAY");
}

TuiDashboardView::KeyResult TuiDashboardView::handleKey(const QByteArray& seq) {
    if (seq == "\033" || seq == "q") {
        return KeyResult::Close;
    }
    if (seq == "1") { m_period = Period::Day; m_scroll = 0; return KeyResult::Consumed; }
    if (seq == "2") { m_period = Period::Week; m_scroll = 0; return KeyResult::Consumed; }
    if (seq == "3") { m_period = Period::Month; m_scroll = 0; return KeyResult::Consumed; }
    if (seq == "4") { m_period = Period::All; m_scroll = 0; return KeyResult::Consumed; }

    if (seq == "j" || seq == "\033[B") { ++m_scroll; return KeyResult::Consumed; }
    if (seq == "k" || seq == "\033[A") { m_scroll = std::max(0, m_scroll - 1); return KeyResult::Consumed; }
    if (seq == "d" || seq == "\x04" || seq == " ") { m_scroll += 8; return KeyResult::Consumed; }
    if (seq == "u" || seq == "\x15") { m_scroll = std::max(0, m_scroll - 8); return KeyResult::Consumed; }
    if (seq == "g") { m_scroll = 0; return KeyResult::Consumed; }
    if (seq == "G") { m_scroll = 100000; return KeyResult::Consumed; }

    if (seq == "c") {
        return KeyResult::ExportCsv;
    }
    if (seq == "p") {
        return KeyResult::ExportPdf;
    }
    return KeyResult::Ignored;
}

QString TuiDashboardView::render(int bodyRows, int cols) {
    if (!m_app) return {};

    const QString periodKey = periodName();
    const QString pTitle = periodTitle();

    QString out;
    const QString headerText = QCoreApplication::translate(
        "TuiApp",
        " 📊 PRODUCTIVITY DASHBOARD: %1  ([1]Day [2]Week [3]Month [4]All │ [c]CSV [p]PDF │ q/Esc:Close)"
    ).arg(pTitle);

    out += BG_HEADER + FG_CYAN + ESC_BOLD
        + padRight(headerText, cols)
        + ESC_RESET + QStringLiteral("\033[K\r\n");

    const QVariantMap stats = m_app->analytics()->getProductivityStats(periodKey, m_app->activeListFilter());
    const QVariantList heatmap = m_app->analytics()->getActivityHeatmap(28, m_app->activeListFilter());

    QStringList lines;
    lines.append(FG_YELLOW + ESC_BOLD + QCoreApplication::translate("TuiApp", "  ── SUMMARY METRICS ──") + ESC_RESET);
    lines.append(QStringLiteral("  ⏱ Total Focus: %1 (%2h)   🍅 Pomodoros: %3   📋 Active Tasks: %4   ⚡ Sessions: %5")
        .arg(stats.value(QStringLiteral("totalFormattedTime")).toString(),
             QString::number(stats.value(QStringLiteral("totalHours")).toDouble(), 'f', 2),
             QString::number(stats.value(QStringLiteral("completedPomodoros")).toInt()),
             QString::number(stats.value(QStringLiteral("activeTaskCount")).toInt()),
             QString::number(stats.value(QStringLiteral("totalSessions")).toInt())));

    lines.append(QString());
    lines.append(FG_GREEN + ESC_BOLD + QCoreApplication::translate("TuiApp", "  ── %1-DAY ACTIVITY HEATMAP ──", nullptr, heatmap.size()).arg(heatmap.size()) + ESC_RESET);
    const int maxWeeks = (cols - 2) / 21;
    const int cellsPerRow = maxWeeks >= 1 ? maxWeeks * 7 : std::max(1, (cols - 2) / 3);

    QString heatRow = QStringLiteral("  ");
    int countInRow = 0;
    for (const QVariant& hv : heatmap) {
        if (countInRow >= cellsPerRow) {
            lines.append(heatRow);
            heatRow = QStringLiteral("  ");
            countInRow = 0;
        }
        const QVariantMap hm = hv.toMap();
        const int lvl = hm.value(QStringLiteral("intensity")).toInt();
        if (lvl == 0) heatRow += FG_GRAY + QStringLiteral("[ ]") + ESC_RESET;
        else if (lvl == 1) heatRow += QStringLiteral("\033[38;5;108m[░]") + ESC_RESET;
        else if (lvl == 2) heatRow += QStringLiteral("\033[38;5;114m[▒]") + ESC_RESET;
        else if (lvl == 3) heatRow += QStringLiteral("\033[38;5;120m[▓]") + ESC_RESET;
        else heatRow += QStringLiteral("\033[38;5;46m[█]") + ESC_RESET;
        ++countInRow;
    }
    if (!heatRow.trimmed().isEmpty()) {
        lines.append(heatRow);
    }

    lines.append(QString());
    lines.append(FG_ORANGE + ESC_BOLD + QCoreApplication::translate("TuiApp", "  ── PROJECTS (+Project) ──") + ESC_RESET);
    const QVariantList projs = stats.value(QStringLiteral("projectBreakdown")).toList();
    if (projs.isEmpty()) {
        lines.append(FG_GRAY + QCoreApplication::translate("TuiApp", "  (No project tags recorded in this period)") + ESC_RESET);
    } else {
        for (const QVariant& pv : projs) {
            const QVariantMap pm = pv.toMap();
            const double pct = pm.value(QStringLiteral("percentage")).toDouble();
            const int barLen = std::clamp(static_cast<int>(pct / 5.0), 0, 20);
            const QString bar = QString(barLen, QChar(0x2588)) + QString(20 - barLen, QChar(0x2591));
            const QString name = pm.value(QStringLiteral("name")).toString().leftJustified(16, QLatin1Char(' '));
            lines.append(QStringLiteral("  +%1 [%2] %3%  (%4)")
                .arg(name, bar, QString::number(pct, 'f', 1), pm.value(QStringLiteral("formattedTime")).toString()));
        }
    }

    lines.append(QString());
    lines.append(FG_PURPLE + ESC_BOLD + QCoreApplication::translate("TuiApp", "  ── CONTEXTS (@Context) ──") + ESC_RESET);
    const QVariantList ctxs = stats.value(QStringLiteral("contextBreakdown")).toList();
    if (ctxs.isEmpty()) {
        lines.append(FG_GRAY + QCoreApplication::translate("TuiApp", "  (No context tags recorded in this period)") + ESC_RESET);
    } else {
        for (const QVariant& cv : ctxs) {
            const QVariantMap cm = cv.toMap();
            const double pct = cm.value(QStringLiteral("percentage")).toDouble();
            const int barLen = std::clamp(static_cast<int>(pct / 5.0), 0, 20);
            const QString bar = QString(barLen, QChar(0x2588)) + QString(20 - barLen, QChar(0x2591));
            const QString name = cm.value(QStringLiteral("name")).toString().leftJustified(16, QLatin1Char(' '));
            lines.append(QStringLiteral("  @%1 [%2] %3%  (%4)")
                .arg(name, bar, QString::number(pct, 'f', 1), cm.value(QStringLiteral("formattedTime")).toString()));
        }
    }

    lines.append(QString());
    lines.append(FG_CYAN + ESC_BOLD + QCoreApplication::translate("TuiApp", "  ── TOP TASKS (Time & Billing) ──") + ESC_RESET);
    const QVariantList tasks = stats.value(QStringLiteral("taskBreakdown")).toList();
    if (tasks.isEmpty()) {
        lines.append(FG_GRAY + QCoreApplication::translate("TuiApp", "  (No tasks tracked during this period)") + ESC_RESET);
    } else {
        for (const QVariant& tv : tasks) {
            const QVariantMap tm = tv.toMap();
            const QString key = tm.value(QStringLiteral("jiraKey")).toString();
            const QString badge = !key.isEmpty() ? QStringLiteral("[%1] ").arg(key) : QString();
            lines.append(QStringLiteral("  • %1%2 (%3, %4 sessions)")
                .arg(badge,
                     sanitizeTerminalText(tm.value(QStringLiteral("description")).toString()),
                     tm.value(QStringLiteral("formattedTime")).toString(),
                     QString::number(tm.value(QStringLiteral("sessions")).toInt())));
        }
    }

    const int availRows = bodyRows - 1;
    const int maxScroll = std::max(0, static_cast<int>(lines.size()) - availRows);
    m_scroll = std::clamp(m_scroll, 0, maxScroll);
    for (int r = 0; r < availRows; ++r) {
        const int idx = m_scroll + r;
        if (idx < lines.size()) {
            out += truncateVisible(lines.at(idx), cols) + QStringLiteral("\033[K\r\n");
        } else {
            out += FG_GRAY + QStringLiteral(" ~") + ESC_RESET + QStringLiteral("\033[K\r\n");
        }
    }
    return out;
}

} // namespace gofi
