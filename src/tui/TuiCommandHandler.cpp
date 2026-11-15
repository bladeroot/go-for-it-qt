#include "tui/TuiCommandHandler.h"
#include "tui/TuiApp.h"
#include "tui/TuiDashboardView.h"
#include "core/AppController.h"
#include "core/AnalyticsService.h"
#include "core/AmbientSoundService.h"
#include "core/SettingsManager.h"
#include "core/SystemHookService.h"
#include "todotxt/TaskFilterProxyModel.h"
#include <QCoreApplication>

namespace gofi {

TuiCommandHandler::TuiCommandHandler(TuiApp* app)
    : m_tui(app) {
}

void TuiCommandHandler::execute(const QString& cmdLine) {
    if (!m_tui || cmdLine.isEmpty()) return;

    auto tr = [](const char* sourceText) {
        return QCoreApplication::translate("TuiApp", sourceText);
    };

    if (cmdLine == QStringLiteral("q") || cmdLine == QStringLiteral("quit") || cmdLine == QStringLiteral("qa")) {
        m_tui->quitApp();
        return;
    }
    if (cmdLine == QStringLiteral("w") || cmdLine == QStringLiteral("sync") || cmdLine == QStringLiteral("e")) {
        m_tui->m_statusMessage = tr("Syncing '%1'...").arg(m_tui->m_app->currentFilterTitle());
        m_tui->m_app->syncCurrentList();
        return;
    }
    if (cmdLine == QStringLiteral("wq") || cmdLine == QStringLiteral("x")) {
        m_tui->quitApp();
        return;
    }
    if (cmdLine == QStringLiteral("sort")) {
        m_tui->m_app->sortAllTasks();
        m_tui->m_statusMessage = tr("Tasks sorted by priority.");
        return;
    }
    if (cmdLine == QStringLiteral("clear")) {
        m_tui->m_app->clearDoneTasks();
        m_tui->clampSelection();
        m_tui->m_statusMessage = tr("Cleared completed tasks.");
        return;
    }
    if (cmdLine == QStringLiteral("noh") || cmdLine == QStringLiteral("nohlsearch")) {
        m_tui->m_app->todoProxyModel()->setFilterQuery(QString());
        m_tui->m_app->doneProxyModel()->setFilterQuery(QString());
        m_tui->m_statusMessage = tr("Search filter cleared.");
        m_tui->clampSelection();
        return;
    }
    if (cmdLine == QStringLiteral("set") || cmdLine == QStringLiteral("settings") || cmdLine == QStringLiteral("config")) {
        m_tui->m_settingsView.reset();
        m_tui->m_mode = TuiApp::Mode::Settings;
        return;
    }
    if (cmdLine == QStringLiteral("list") || cmdLine == QStringLiteral("lists") || cmdLine == QStringLiteral("ls")) {
        m_tui->openListPicker();
        return;
    }
    if (cmdLine == QStringLiteral("help") || cmdLine == QStringLiteral("h")) {
        m_tui->m_mode = TuiApp::Mode::Help;
        return;
    }
    if (cmdLine == QStringLiteral("stats") || cmdLine == QStringLiteral("dashboard") || cmdLine == QStringLiteral("report")) {
        m_tui->openDashboardView();
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("export-csv"))) {
        const QString rest = cmdLine.mid(10).trimmed();
        const QString outPath = !rest.isEmpty() ? rest : TuiDashboardView::defaultBillingExportPath(QStringLiteral("billing-report.csv"));
        if (m_tui->m_app->analytics()->exportBillingCsv(outPath, QStringLiteral("month"), m_tui->m_app->activeListFilter())) {
            m_tui->m_statusMessage = tr("Billing CSV exported to %1").arg(outPath);
        } else {
            m_tui->m_statusMessage = tr("Failed to export CSV report.");
        }
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("export-pdf"))) {
        const QString rest = cmdLine.mid(10).trimmed();
        const QString outPath = !rest.isEmpty() ? rest : TuiDashboardView::defaultBillingExportPath(QStringLiteral("billing-report.pdf"));
        if (m_tui->m_app->analytics()->exportBillingPdf(outPath, QStringLiteral("month"), m_tui->m_app->activeListFilter())) {
            m_tui->m_statusMessage = tr("Billing PDF exported to %1").arg(outPath);
        } else {
            m_tui->m_statusMessage = tr("Failed to export PDF report.");
        }
        return;
    }
    if (cmdLine == QStringLiteral("md") || cmdLine == QStringLiteral("view")) {
        m_tui->openTaskMarkdownView();
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("md ")) || cmdLine.startsWith(QStringLiteral("view "))) {
        const QString path = cmdLine.section(QLatin1Char(' '), 1).trimmed();
        m_tui->openMarkdownFile(path);
        return;
    }
    if (cmdLine == QStringLiteral("comment")) {
        m_tui->addTaskCommentInVim();
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("comment "))) {
        const QString cText = cmdLine.mid(8).trimmed();
        if (!cText.isEmpty()) {
            if (m_tui->currentProxy()->rowCount() > 0 && m_tui->selectedRow() >= 0 && m_tui->selectedRow() < m_tui->currentProxy()->rowCount()) {
                if (m_tui->m_app->addTaskComment(m_tui->isClosedTab(), m_tui->selectedRow(), cText)) {
                    if (m_tui->m_mode == TuiApp::Mode::MarkdownView) m_tui->openTaskMarkdownView();
                    m_tui->m_statusMessage = tr("Comment added & queued for Jira/YouTrack sync.");
                } else {
                    m_tui->m_statusMessage = tr("Failed to save comment to database.");
                }
            } else {
                m_tui->m_statusMessage = tr("Failed to save comment to database.");
            }
        }
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("add "))) {
        const QString taskText = cmdLine.mid(4).trimmed();
        if (!taskText.isEmpty()) {
            m_tui->m_app->addNewTask(taskText);
            m_tui->m_statusMessage = tr("Added task: %1").arg(taskText);
            m_tui->clampSelection();
        }
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("ambient"))) {
        const QString mode = cmdLine.mid(7).trimmed().toLower();
        if (mode.isEmpty() || mode == QStringLiteral("toggle")) {
            const QString cur = m_tui->m_app->ambient()->ambientMode();
            const QString next = (cur == QStringLiteral("none")) ? QStringLiteral("rain") : QStringLiteral("none");
            m_tui->m_app->ambient()->setAmbientMode(next);
            m_tui->m_statusMessage = tr("Ambient focus sound: %1").arg(next);
        } else if (mode == QStringLiteral("none") || mode == QStringLiteral("off")) {
            m_tui->m_app->ambient()->setAmbientMode(QStringLiteral("none"));
            m_tui->m_statusMessage = tr("Ambient focus sound turned off.");
        } else if (mode.startsWith(QStringLiteral("white"))) {
            m_tui->m_app->ambient()->setAmbientMode(QStringLiteral("white_noise"));
            m_tui->m_statusMessage = tr("Ambient focus sound: White Noise.");
        } else if (mode.startsWith(QStringLiteral("rain"))) {
            m_tui->m_app->ambient()->setAmbientMode(QStringLiteral("rain"));
            m_tui->m_statusMessage = tr("Ambient focus sound: Gentle Rain.");
        } else if (mode.startsWith(QStringLiteral("ocean")) || mode.startsWith(QStringLiteral("wave"))) {
            m_tui->m_app->ambient()->setAmbientMode(QStringLiteral("ocean"));
            m_tui->m_statusMessage = tr("Ambient focus sound: Ocean Waves.");
        } else if (mode.startsWith(QStringLiteral("clock")) || mode.startsWith(QStringLiteral("tick"))) {
            m_tui->m_app->ambient()->setAmbientMode(QStringLiteral("clock"));
            m_tui->m_statusMessage = tr("Ambient focus sound: Mechanical Clock Ticking.");
        } else {
            m_tui->m_statusMessage = tr("Unknown ambient mode. Available: none, white, rain, ocean, clock");
        }
        return;
    }
    if (cmdLine == QStringLiteral("today")) {
        m_tui->m_app->todoProxyModel()->setDueFilter(QStringLiteral("today"));
        m_tui->clampSelection();
        m_tui->m_statusMessage = tr("Filtered tasks due today.");
        return;
    }
    if (cmdLine == QStringLiteral("overdue")) {
        m_tui->m_app->todoProxyModel()->setDueFilter(QStringLiteral("overdue"));
        m_tui->clampSelection();
        m_tui->m_statusMessage = tr("Filtered overdue tasks.");
        return;
    }
    if (cmdLine == QStringLiteral("week")) {
        m_tui->m_app->todoProxyModel()->setDueFilter(QStringLiteral("week"));
        m_tui->clampSelection();
        m_tui->m_statusMessage = tr("Filtered tasks due this week.");
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("dnd"))) {
        const QString arg = cmdLine.mid(3).trimmed().toLower();
        if (arg == QStringLiteral("on") || arg == QStringLiteral("1") || arg == QStringLiteral("enable")) {
            m_tui->m_app->settings()->setDndFocusEnabled(true);
            m_tui->m_statusMessage = tr("Do Not Disturb (DND) focus mode enabled for Work cycles.");
        } else if (arg == QStringLiteral("off") || arg == QStringLiteral("0") || arg == QStringLiteral("disable")) {
            m_tui->m_app->settings()->setDndFocusEnabled(false);
            m_tui->m_statusMessage = tr("Do Not Disturb (DND) focus mode disabled.");
        } else {
            const bool cur = m_tui->m_app->settings()->dndFocusEnabled();
            m_tui->m_app->settings()->setDndFocusEnabled(!cur);
            m_tui->m_statusMessage = tr("Do Not Disturb (DND) focus mode: %1").arg(!cur ? tr("ON") : tr("OFF"));
        }
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("hooks"))) {
        const QString arg = cmdLine.mid(5).trimmed().toLower();
        if (arg == QStringLiteral("on") || arg == QStringLiteral("1") || arg == QStringLiteral("enable")) {
            m_tui->m_app->settings()->setCustomHooksEnabled(true);
            m_tui->m_statusMessage = tr("System event hooks enabled.");
        } else if (arg == QStringLiteral("off") || arg == QStringLiteral("0") || arg == QStringLiteral("disable")) {
            m_tui->m_app->settings()->setCustomHooksEnabled(false);
            m_tui->m_statusMessage = tr("System event hooks disabled.");
        } else {
            const bool cur = m_tui->m_app->settings()->customHooksEnabled();
            m_tui->m_app->settings()->setCustomHooksEnabled(!cur);
            m_tui->m_statusMessage = tr("System event hooks: %1").arg(!cur ? tr("ON") : tr("OFF"));
        }
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("hook"))) {
        QString event = cmdLine.mid(4).trimmed();
        if (event.startsWith(QStringLiteral("test "))) {
            event = event.mid(5).trimmed();
        } else if (event == QStringLiteral("test")) {
            event = QStringLiteral("work_start");
        }
        if (event.isEmpty()) event = QStringLiteral("work_start");
        m_tui->m_app->systemHooks()->testHook(event);
        m_tui->m_statusMessage = tr("Tested hook event '%1' (dir: %2)").arg(event, m_tui->m_app->systemHooks()->hooksDirectory());
        return;
    }
    if (cmdLine == QStringLiteral("all")) {
        m_tui->m_app->todoProxyModel()->setDueFilter(QString());
        m_tui->clampSelection();
        m_tui->m_statusMessage = tr("Showing all tasks.");
        return;
    }
    if (cmdLine.startsWith(QStringLiteral("p ")) || cmdLine.startsWith(QStringLiteral("prio ")) || cmdLine.startsWith(QStringLiteral("priority "))) {
        const QString prioArg = cmdLine.section(QLatin1Char(' '), 1).trimmed();
        if (m_tui->currentProxy()->rowCount() > 0 && m_tui->selectedRow() >= 0) {
            m_tui->m_app->changeTaskPriority(m_tui->isClosedTab(), m_tui->selectedRow(), prioArg);
            m_tui->m_statusMessage = tr("Priority set to %1").arg(prioArg.toUpper());
        }
        return;
    }
    m_tui->m_statusMessage = tr("Unknown command: :%1 (try :help, :sync, :sort, :clear, :list, :q)").arg(cmdLine);
}

} // namespace gofi
