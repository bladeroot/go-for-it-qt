#include "tui/TuiSettingsView.h"
#include "core/AppController.h"
#include "core/SettingsManager.h"
#include "tui/TuiUtils.h"
#include <QCoreApplication>
#include <QRegularExpression>

namespace gofi {

using namespace tui;

TuiSettingsView::TuiSettingsView(AppController* controller)
    : m_app(controller) {
}

void TuiSettingsView::setController(AppController* controller) {
    m_app = controller;
}

void TuiSettingsView::reset() {
    m_selectedRow = 0;
}

SettingsKeyResult TuiSettingsView::handleKey(const QByteArray& seq, QString& editInitialValue) {
    if (seq == "\033" || seq == "q" || seq == "O") {
        return SettingsKeyResult::Close;
    }
    if (seq == "j" || seq == "\033[B") {
        m_selectedRow = qMin(14, m_selectedRow + 1);
        return SettingsKeyResult::Handled;
    }
    if (seq == "k" || seq == "\033[A") {
        m_selectedRow = qMax(0, m_selectedRow - 1);
        return SettingsKeyResult::Handled;
    }
    if (!m_app) {
        return SettingsKeyResult::Handled;
    }
    SettingsManager* s = m_app->settings();
    if (seq == "h" || seq == "-" || seq == "\033[D") {
        if (m_selectedRow == 0) s->setTaskDuration(qMax(60, s->taskDuration() - 60));
        else if (m_selectedRow == 1) s->setBreakDuration(qMax(60, s->breakDuration() - 60));
        else if (m_selectedRow == 2) s->setReminderTime(qMax(0, s->reminderTime() - 10));
        else if (m_selectedRow == 10) s->setAutoSyncIntervalMinutes(qMax(0, s->autoSyncIntervalMinutes() - 1));
        return SettingsKeyResult::Handled;
    }
    if (seq == "l" || seq == "+" || seq == "=" || seq == "\033[C") {
        if (m_selectedRow == 0) s->setTaskDuration(qMin(86400, s->taskDuration() + 60));
        else if (m_selectedRow == 1) s->setBreakDuration(qMin(86400, s->breakDuration() + 60));
        else if (m_selectedRow == 2) s->setReminderTime(qMin(3600, s->reminderTime() + 10));
        else if (m_selectedRow == 10) s->setAutoSyncIntervalMinutes(qMin(1440, s->autoSyncIntervalMinutes() + 1));
        return SettingsKeyResult::Handled;
    }
    if (seq == " " || seq == "\r" || seq == "\n" || seq == "e" || seq == "i") {
        if (m_selectedRow == 0) {
            editInitialValue = QString::number(s->taskDuration() / 60);
            return SettingsKeyResult::EditValue;
        } else if (m_selectedRow == 1) {
            editInitialValue = QString::number(s->breakDuration() / 60);
            return SettingsKeyResult::EditValue;
        } else if (m_selectedRow == 2) {
            editInitialValue = QString::number(s->reminderTime());
            return SettingsKeyResult::EditValue;
        } else if (m_selectedRow == 3) {
            s->setMuteSounds(!s->muteSounds());
        } else if (m_selectedRow == 4) {
            s->setUseDarkTheme(!s->useDarkTheme());
        } else if (m_selectedRow == 5) {
            s->setNewTasksOnTop(!s->newTasksOnTop());
        } else if (m_selectedRow == 6) {
            s->setResetTimerOnTaskSwitch(!s->resetTimerOnTaskSwitch());
        } else if (m_selectedRow == 7) {
            editInitialValue = m_app->databasePath();
            return SettingsKeyResult::EditValue;
        } else if (m_selectedRow == 8) {
            editInitialValue = m_app->databasePath() + QStringLiteral(".backup.db");
            return SettingsKeyResult::EditValue;
        } else if (m_selectedRow == 9) {
            const QStringList langs = m_app->availableLanguageCodes();
            const int idx = langs.indexOf(s->language());
            if (idx >= 0 && !langs.isEmpty()) {
                s->setLanguage(langs.at((idx + 1) % langs.size()));
            } else {
                s->setLanguage(langs.isEmpty() ? QStringLiteral("system") : langs.first());
            }
        } else if (m_selectedRow == 10) {
            editInitialValue = QString::number(s->autoSyncIntervalMinutes());
            return SettingsKeyResult::EditValue;
        } else if (m_selectedRow == 11) {
            s->setNotifyTaskChangesSound(!s->notifyTaskChangesSound());
        } else if (m_selectedRow == 12) {
            s->setNotifyTaskChangesPopup(!s->notifyTaskChangesPopup());
        } else if (m_selectedRow == 13) {
            s->setStartMinimized(!s->startMinimized());
        } else if (m_selectedRow == 14) {
            s->setStartInTray(!s->startInTray());
        }
        return SettingsKeyResult::Handled;
    }
    return SettingsKeyResult::Handled;
}

QString TuiSettingsView::applyEditValue(const QString& text) {
    if (!m_app) return QString();

    auto parseSettingNumber = [&](qint64 minValue, qint64 maxValue, qint64& out) {
        static const QRegularExpression numberRe(QStringLiteral("^[+-]?\\d+$"));
        if (!numberRe.match(text).hasMatch()) {
            return false;
        }
        bool ok = false;
        const qint64 value = text.toLongLong(&ok);
        if (!ok) {
            out = text.startsWith(QLatin1Char('-')) ? minValue : maxValue;
        } else {
            out = qBound(minValue, value, maxValue);
        }
        return true;
    };

    QString statusMessage;
    qint64 parsedValue = 0;
    if (m_selectedRow == 0) {
        if (parseSettingNumber(1, 1440, parsedValue)) {
            m_app->settings()->setTaskDuration(static_cast<int>(parsedValue * 60));
        } else {
            statusMessage = QCoreApplication::translate("TuiApp", "Invalid number: \"%1\" (value unchanged)").arg(text);
        }
    } else if (m_selectedRow == 1) {
        if (parseSettingNumber(1, 1440, parsedValue)) {
            m_app->settings()->setBreakDuration(static_cast<int>(parsedValue * 60));
        } else {
            statusMessage = QCoreApplication::translate("TuiApp", "Invalid number: \"%1\" (value unchanged)").arg(text);
        }
    } else if (m_selectedRow == 2) {
        if (parseSettingNumber(0, 3600, parsedValue)) {
            m_app->settings()->setReminderTime(static_cast<int>(parsedValue));
        } else {
            statusMessage = QCoreApplication::translate("TuiApp", "Invalid number: \"%1\" (value unchanged)").arg(text);
        }
    } else if (m_selectedRow == 7 && !text.isEmpty()) {
        if (m_app->changeDatabasePath(text, true)) {
            statusMessage = QCoreApplication::translate("TuiApp", "Database moved to: %1").arg(m_app->databasePath());
        } else {
            statusMessage = QCoreApplication::translate("TuiApp", "Failed to move database to: %1").arg(text);
        }
    } else if (m_selectedRow == 8 && !text.isEmpty()) {
        if (m_app->exportBackup(text)) {
            statusMessage = QCoreApplication::translate("TuiApp", "Backup exported to: %1").arg(text);
        } else {
            statusMessage = QCoreApplication::translate("TuiApp", "Failed to export backup to: %1").arg(text);
        }
    } else if (m_selectedRow == 10) {
        if (parseSettingNumber(0, 1440, parsedValue)) {
            m_app->settings()->setAutoSyncIntervalMinutes(static_cast<int>(parsedValue));
        } else {
            statusMessage = QCoreApplication::translate("TuiApp", "Invalid number: \"%1\" (value unchanged)").arg(text);
        }
    }
    return statusMessage;
}

QString TuiSettingsView::render(int bodyRows, int cols) const {
    if (!m_app) return QString();

    QString out;
    out += FG_CYAN + ESC_BOLD + QCoreApplication::translate("TuiApp", "  ⚙ SETTINGS (j/k move, Enter/Space/h/l change, q/Esc close):") + ESC_RESET + QStringLiteral("\033[K\r\n");

    SettingsManager* s = m_app->settings();
    const QString onStr = QCoreApplication::translate("TuiApp", "[ON]");
    const QString offStr = QCoreApplication::translate("TuiApp", "[OFF]");

    const QStringList settingItems = {
        QCoreApplication::translate("TuiApp", "Pomodoro Work Duration (min)    : %1 min").arg(s->taskDuration() / 60),
        QCoreApplication::translate("TuiApp", "Pomodoro Break Duration (min)   : %1 min").arg(s->breakDuration() / 60),
        QCoreApplication::translate("TuiApp", "Reminder Before End (sec)       : %1 sec").arg(s->reminderTime()),
        QCoreApplication::translate("TuiApp", "Mute Sound Notifications        : %1").arg(s->muteSounds() ? onStr : offStr),
        QCoreApplication::translate("TuiApp", "GUI Dark Theme                  : %1").arg(s->useDarkTheme() ? onStr : offStr),
        QCoreApplication::translate("TuiApp", "Add New Tasks On Top            : %1").arg(s->newTasksOnTop() ? onStr : offStr),
        QCoreApplication::translate("TuiApp", "Reset Timer On Task Switch      : %1").arg(s->resetTimerOnTaskSwitch() ? onStr : offStr),
        QCoreApplication::translate("TuiApp", "Move SQLite Database Path       : %1").arg(m_app->databasePath()),
        QCoreApplication::translate("TuiApp", "Export SQLite Database Backup   : [Press Enter to export .db backup]"),
        QCoreApplication::translate("TuiApp", "Language (Enter to cycle)       : [%1] (%2)").arg(s->language(), m_app->languageDisplayName(s->language())),
        QCoreApplication::translate("TuiApp", "Auto-Sync Jira/YouTrack (min)   : %1 min").arg(s->autoSyncIntervalMinutes()),
        QCoreApplication::translate("TuiApp", "Sound On New/Changed Tasks      : %1").arg(s->notifyTaskChangesSound() ? onStr : offStr),
        QCoreApplication::translate("TuiApp", "Popup On New/Changed Tasks      : %1").arg(s->notifyTaskChangesPopup() ? onStr : offStr),
        QCoreApplication::translate("TuiApp", "GUI Start Minimized             : %1").arg(s->startMinimized() ? onStr : offStr),
        QCoreApplication::translate("TuiApp", "GUI Start In System Tray        : %1").arg(s->startInTray() ? onStr : offStr)
    };

    for (int r = 1; r < bodyRows; ++r) {
        const int idx = r - 1;
        if (idx < settingItems.size()) {
            const bool isSel = (idx == m_selectedRow);
            const QString prefix = isSel ? QStringLiteral("  > ") : QStringLiteral("    ");
            out += (isSel ? (BG_SELECTED + FG_WHITE + ESC_BOLD) : FG_WHITE)
                + padRight(prefix + settingItems.at(idx), cols) + ESC_RESET + QStringLiteral("\033[K\r\n");
        } else {
            out += QStringLiteral("\033[K\r\n");
        }
    }
    return out;
}

} // namespace gofi
