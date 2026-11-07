#include "SettingsManager.h"
#include "DatabaseManager.h"
#include <QGuiApplication>
#include <QLocale>
#include <QPalette>
#include <QStyleHints>

namespace gofi {

SettingsManager::SettingsManager(QObject* parent)
    : QObject(parent)
    , m_settings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("go-for-it"))
{
    rebuildSchedule();
}

int SettingsManager::taskDuration() const {
    return m_settings.value(QStringLiteral("Timer/task_duration"), 1500).toInt();
}

void SettingsManager::setTaskDuration(int seconds) {
    if (taskDuration() == seconds) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Timer/task_duration"), qMax(1, seconds));
    rebuildSchedule();
    emit timerDurationChanged();
}

int SettingsManager::breakDuration() const {
    return m_settings.value(QStringLiteral("Timer/break_duration"), 300).toInt();
}

void SettingsManager::setBreakDuration(int seconds) {
    if (breakDuration() == seconds) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Timer/break_duration"), qMax(0, seconds));
    rebuildSchedule();
    emit timerDurationChanged();
}

int SettingsManager::longBreakDuration() const {
    return m_settings.value(QStringLiteral("Timer/long_break_duration"), 900).toInt();
}

void SettingsManager::setLongBreakDuration(int seconds) {
    if (longBreakDuration() == seconds) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Timer/long_break_duration"), qMax(0, seconds));
    rebuildSchedule();
    emit timerDurationChanged();
}

int SettingsManager::pomodoroPeriod() const {
    return qMax(1, m_settings.value(QStringLiteral("Timer/pomodoro_period"), 4).toInt());
}

void SettingsManager::setPomodoroPeriod(int period) {
    if (pomodoroPeriod() == period) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Timer/pomodoro_period"), qMax(1, period));
    rebuildSchedule();
    emit timerDurationChanged();
}

int SettingsManager::reminderTime() const {
    return m_settings.value(QStringLiteral("Timer/reminder_time"), 60).toInt();
}

void SettingsManager::setReminderTime(int seconds) {
    if (reminderTime() == seconds) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Timer/reminder_time"), qBound(0, seconds, 600));
    emit reminderTimeChanged();
}

int SettingsManager::timerMode() const {
    return m_settings.value(QStringLiteral("Timer/timer_mode"), static_cast<int>(Simple)).toInt();
}

void SettingsManager::setTimerMode(int mode) {
    if (timerMode() == mode) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Timer/timer_mode"), mode);
    rebuildSchedule();
    emit timerModeChanged();
    emit timerDurationChanged();
}

QVariantList SettingsManager::customScheduleVariant() const {
    QVariantList stored = m_settings.value(QStringLiteral("Timer/custom_schedule")).toList();
    if (stored.size() >= 2) {
        return stored;
    }
    return m_schedule.toVariantList();
}

void SettingsManager::setCustomScheduleVariant(const QVariantList& pairs) {
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Timer/custom_schedule"), pairs);
    if (timerMode() == Custom) {
        rebuildSchedule();
        emit timerDurationChanged();
    }
}

bool SettingsManager::resumeTasksAfterBreak() const {
    return m_settings.value(QStringLiteral("Timer/resume_tasks_after_break"), false).toBool();
}

void SettingsManager::setResumeTasksAfterBreak(bool value) {
    if (resumeTasksAfterBreak() == value) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Timer/resume_tasks_after_break"), value);
    emit resumeTasksAfterBreakChanged();
}

bool SettingsManager::resetTimerOnTaskSwitch() const {
    return m_settings.value(QStringLiteral("Timer/reset_timer_on_task_switch"), false).toBool();
}

void SettingsManager::setResetTimerOnTaskSwitch(bool value) {
    if (resetTimerOnTaskSwitch() == value) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Timer/reset_timer_on_task_switch"), value);
    emit resetTimerOnTaskSwitchChanged();
}

bool SettingsManager::newTasksOnTop() const {
    return m_settings.value(QStringLiteral("Behavior/new_tasks_on_top"), false).toBool();
}

void SettingsManager::setNewTasksOnTop(bool value) {
    if (newTasksOnTop() == value) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Behavior/new_tasks_on_top"), value);
    emit newTasksOnTopChanged();
}

QString SettingsManager::language() const {
    return m_settings.value(QStringLiteral("UI/language"), QStringLiteral("system")).toString();
}

void SettingsManager::setLanguage(const QString& lang) {
    const QString norm = lang.trimmed().isEmpty() ? QStringLiteral("system") : lang.trimmed().toLower();
    if (language() == norm) return;
    DatabaseManager::instance().setSetting(QStringLiteral("language"), norm);
    m_settings.setValue(QStringLiteral("UI/language"), norm);
    emit languageChanged();
}

QString SettingsManager::effectiveLanguage() const {
    const QString lang = language().trimmed().toLower();
    for (const QString& code : {QStringLiteral("uk"), QStringLiteral("de"), QStringLiteral("fr"),
                                QStringLiteral("es"), QStringLiteral("it"), QStringLiteral("en")}) {
        if (lang == code || lang.startsWith(code + QLatin1Char('_')) || lang.startsWith(code + QLatin1Char('-'))) {
            return code;
        }
    }
    const QString sysName = QLocale::system().name().toLower();
    for (const QString& code : {QStringLiteral("uk"), QStringLiteral("de"), QStringLiteral("fr"),
                                QStringLiteral("es"), QStringLiteral("it")}) {
        if (sysName == code || sysName.startsWith(code + QLatin1Char('_')) || sysName.startsWith(code + QLatin1Char('-'))) {
            return code;
        }
    }
    return QStringLiteral("en");
}

bool SettingsManager::isUkrainianActive() const {
    return effectiveLanguage() == QStringLiteral("uk");
}

int SettingsManager::colorScheme() const {
    return m_settings.value(QStringLiteral("UI/color_scheme"), static_cast<int>(DefaultScheme)).toInt();
}

void SettingsManager::setColorScheme(int scheme) {
    if (colorScheme() == scheme) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("UI/color_scheme"), scheme);
    emit colorSchemeChanged();
    emit useDarkThemeChanged();
}

bool SettingsManager::useDarkTheme() const {
    const int cs = colorScheme();
    if (cs == LightScheme) return false;
    if (cs == DarkScheme) return true;
    if (QGuiApplication::styleHints()) {
        return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    }
    return false;
}

void SettingsManager::setUseDarkTheme(bool value) {
    setColorScheme(value ? DarkScheme : LightScheme);
}

bool SettingsManager::useSmallToolbarIcons() const {
    return m_settings.value(QStringLiteral("UI/small_toolbar_icons"), false).toBool();
}

void SettingsManager::setUseSmallToolbarIcons(bool value) {
    if (useSmallToolbarIcons() == value) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("UI/small_toolbar_icons"), value);
    emit appearanceChanged();
}

bool SettingsManager::switcherUseIcons() const {
    return m_settings.value(QStringLiteral("UI/switcher_use_icons"), false).toBool();
}

void SettingsManager::setSwitcherUseIcons(bool value) {
    if (switcherUseIcons() == value) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("UI/switcher_use_icons"), value);
    emit appearanceChanged();
}

bool SettingsManager::useHeaderBar() const {
    return m_settings.value(QStringLiteral("UI/use_header_bar"), true).toBool();
}

void SettingsManager::setUseHeaderBar(bool value) {
    if (useHeaderBar() == value) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("UI/use_header_bar"), value);
    emit appearanceChanged();
}

bool SettingsManager::muteSounds() const {
    return m_settings.value(QStringLiteral("Sounds/mute"), false).toBool();
}

void SettingsManager::setMuteSounds(bool value) {
    if (muteSounds() == value) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Sounds/mute"), value);
    emit soundSettingsChanged();
}

QString SettingsManager::reminderSoundFile() const {
    return m_settings.value(QStringLiteral("Sounds/reminder_sound_file"), QStringLiteral("singing-bowl.ogg")).toString();
}

void SettingsManager::setReminderSoundFile(const QString& file) {
    if (reminderSoundFile() == file) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Sounds/reminder_sound_file"), file);
    emit soundSettingsChanged();
}

QString SettingsManager::breakStartSoundFile() const {
    return m_settings.value(QStringLiteral("Sounds/break_start_sound_file"), QStringLiteral("aluminium-bowl.ogg")).toString();
}

void SettingsManager::setBreakStartSoundFile(const QString& file) {
    if (breakStartSoundFile() == file) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Sounds/break_start_sound_file"), file);
    emit soundSettingsChanged();
}

QString SettingsManager::breakEndSoundFile() const {
    return m_settings.value(QStringLiteral("Sounds/break_end_sound_file"), QStringLiteral("bell.ogg")).toString();
}

void SettingsManager::setBreakEndSoundFile(const QString& file) {
    if (breakEndSoundFile() == file) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Sounds/break_end_sound_file"), file);
    emit soundSettingsChanged();
}

double SettingsManager::reminderSoundVolume() const {
    return qBound(0.0, m_settings.value(QStringLiteral("Sounds/reminder_sound_volume"), 1.0).toDouble(), 1.0);
}

void SettingsManager::setReminderSoundVolume(double vol) {
    const double clamped = qBound(0.0, vol, 1.0);
    if (qFuzzyCompare(reminderSoundVolume(), clamped)) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Sounds/reminder_sound_volume"), clamped);
    emit soundSettingsChanged();
}

double SettingsManager::breakStartSoundVolume() const {
    return qBound(0.0, m_settings.value(QStringLiteral("Sounds/break_start_sound_volume"), 1.0).toDouble(), 1.0);
}

void SettingsManager::setBreakStartSoundVolume(double vol) {
    const double clamped = qBound(0.0, vol, 1.0);
    if (qFuzzyCompare(breakStartSoundVolume(), clamped)) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Sounds/break_start_sound_volume"), clamped);
    emit soundSettingsChanged();
}

double SettingsManager::breakEndSoundVolume() const {
    return qBound(0.0, m_settings.value(QStringLiteral("Sounds/break_end_sound_volume"), 1.0).toDouble(), 1.0);
}

void SettingsManager::setBreakEndSoundVolume(double vol) {
    const double clamped = qBound(0.0, vol, 1.0);
    if (qFuzzyCompare(breakEndSoundVolume(), clamped)) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Sounds/break_end_sound_volume"), clamped);
    emit soundSettingsChanged();
}

bool SettingsManager::trayIndicatorEnabled() const {
    return m_settings.value(QStringLiteral("Plugins/appindicator"), true).toBool();
}

void SettingsManager::setTrayIndicatorEnabled(bool enabled) {
    if (trayIndicatorEnabled() == enabled) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Plugins/appindicator"), enabled);
    emit pluginsChanged();
}

bool SettingsManager::launcherIndicatorEnabled() const {
    return m_settings.value(QStringLiteral("Plugins/launcherindicator"), true).toBool();
}

void SettingsManager::setLauncherIndicatorEnabled(bool enabled) {
    if (launcherIndicatorEnabled() == enabled) return;
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("Plugins/launcherindicator"), enabled);
    emit pluginsChanged();
}

QSize SettingsManager::windowSize() const {
    return m_settings.value(QStringLiteral("SavedState/window_size"), QSize(440, 580)).toSize();
}

void SettingsManager::setWindowSize(const QSize& size) {
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("SavedState/window_size"), size);
}

QPoint SettingsManager::windowPosition() const {
    return m_settings.value(QStringLiteral("SavedState/window_pos"), QPoint(-1, -1)).toPoint();
}

void SettingsManager::setWindowPosition(const QPoint& pos) {
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("SavedState/window_pos"), pos);
}

void SettingsManager::setWindowWidth(int w) {
    const int clamped = qMax(320, w);
    const QSize cur = windowSize();
    if (cur.width() == clamped) return;
    setWindowSize(QSize(clamped, cur.height()));
    emit windowGeometryChanged();
}

void SettingsManager::setWindowHeight(int h) {
    const int clamped = qMax(420, h);
    const QSize cur = windowSize();
    if (cur.height() == clamped) return;
    setWindowSize(QSize(cur.width(), clamped));
    emit windowGeometryChanged();
}

void SettingsManager::setWindowX(int x) {
    const QPoint cur = windowPosition();
    if (cur.x() == x) return;
    setWindowPosition(QPoint(x, cur.y()));
    emit windowGeometryChanged();
}

void SettingsManager::setWindowY(int y) {
    const QPoint cur = windowPosition();
    if (cur.y() == y) return;
    setWindowPosition(QPoint(cur.x(), y));
    emit windowGeometryChanged();
}

void SettingsManager::saveWindowGeometry(int w, int h, int x, int y) {
    if (w >= 320 && h >= 420) {
        setWindowSize(QSize(w, h));
        DatabaseManager::instance().setSetting(QStringLiteral("window_width"), QString::number(w));
        DatabaseManager::instance().setSetting(QStringLiteral("window_height"), QString::number(h));
    }
    if (x >= 0 && y >= 0) {
        setWindowPosition(QPoint(x, y));
        DatabaseManager::instance().setSetting(QStringLiteral("window_x"), QString::number(x));
        DatabaseManager::instance().setSetting(QStringLiteral("window_y"), QString::number(y));
    }
    m_settings.sync();
}

bool SettingsManager::startMinimized() const {
    return m_settings.value(QStringLiteral("Behavior/start_minimized"), false).toBool();
}

void SettingsManager::setStartMinimized(bool value) {
    if (startMinimized() == value) return;
    DatabaseManager::instance().setSetting(QStringLiteral("start_minimized"), value ? QStringLiteral("1") : QStringLiteral("0"));
    m_settings.setValue(QStringLiteral("Behavior/start_minimized"), value);
    emit startupBehaviorChanged();
}

bool SettingsManager::startInTray() const {
    return m_settings.value(QStringLiteral("Behavior/start_in_tray"), false).toBool();
}

void SettingsManager::setStartInTray(bool value) {
    if (startInTray() == value) return;
    DatabaseManager::instance().setSetting(QStringLiteral("start_in_tray"), value ? QStringLiteral("1") : QStringLiteral("0"));
    m_settings.setValue(QStringLiteral("Behavior/start_in_tray"), value);
    if (value && !trayIndicatorEnabled()) {
        setTrayIndicatorEnabled(true);
    }
    emit startupBehaviorChanged();
}

int SettingsManager::autoSyncIntervalMinutes() const {
    return qBound(0, m_settings.value(QStringLiteral("Behavior/auto_sync_interval_minutes"), 10).toInt(), 1440);
}

void SettingsManager::setAutoSyncIntervalMinutes(int minutes) {
    const int clamped = qBound(0, minutes, 1440);
    if (autoSyncIntervalMinutes() == clamped) return;
    DatabaseManager::instance().setSetting(QStringLiteral("auto_sync_interval_minutes"), QString::number(clamped));
    m_settings.setValue(QStringLiteral("Behavior/auto_sync_interval_minutes"), clamped);
    emit autoSyncChanged();
}

bool SettingsManager::notifyOnTaskChanges() const {
    return notifyTaskChangesSound() || notifyTaskChangesPopup();
}

void SettingsManager::setNotifyOnTaskChanges(bool value) {
    setNotifyTaskChangesSound(value);
    setNotifyTaskChangesPopup(value);
}

bool SettingsManager::notifyTaskChangesSound() const {
    return m_settings.value(QStringLiteral("Notifications/notify_task_changes_sound"), true).toBool();
}

void SettingsManager::setNotifyTaskChangesSound(bool value) {
    if (notifyTaskChangesSound() == value) return;
    DatabaseManager::instance().setSetting(QStringLiteral("notify_task_changes_sound"), value ? QStringLiteral("1") : QStringLiteral("0"));
    m_settings.setValue(QStringLiteral("Notifications/notify_task_changes_sound"), value);
    emit soundSettingsChanged();
}

bool SettingsManager::notifyTaskChangesPopup() const {
    return m_settings.value(QStringLiteral("Notifications/notify_task_changes_popup"), true).toBool();
}

void SettingsManager::setNotifyTaskChangesPopup(bool value) {
    if (notifyTaskChangesPopup() == value) return;
    DatabaseManager::instance().setSetting(QStringLiteral("notify_task_changes_popup"), value ? QStringLiteral("1") : QStringLiteral("0"));
    m_settings.setValue(QStringLiteral("Notifications/notify_task_changes_popup"), value);
    emit soundSettingsChanged();
}

QString SettingsManager::lastLoadedListId() const {
    return m_settings.value(QStringLiteral("SavedState/last_list_id")).toString();
}

void SettingsManager::setLastLoadedListId(const QString& id) {
    DatabaseManager::instance().setSetting(QStringLiteral("lastSynced"), QStringLiteral("1")); m_settings.setValue(QStringLiteral("SavedState/last_list_id"), id);
}

bool SettingsManager::dndFocusEnabled() const {
    return m_settings.value(QStringLiteral("Hooks/dnd_focus"), false).toBool();
}

void SettingsManager::setDndFocusEnabled(bool value) {
    if (dndFocusEnabled() == value) return;
    m_settings.setValue(QStringLiteral("Hooks/dnd_focus"), value);
    emit hookSettingsChanged();
}

bool SettingsManager::inhibitSleepEnabled() const {
    return m_settings.value(QStringLiteral("Hooks/inhibit_sleep"), false).toBool();
}

void SettingsManager::setInhibitSleepEnabled(bool value) {
    if (inhibitSleepEnabled() == value) return;
    m_settings.setValue(QStringLiteral("Hooks/inhibit_sleep"), value);
    emit hookSettingsChanged();
}

bool SettingsManager::customHooksEnabled() const {
    return m_settings.value(QStringLiteral("Hooks/custom_hooks"), false).toBool();
}

void SettingsManager::setCustomHooksEnabled(bool value) {
    if (customHooksEnabled() == value) return;
    m_settings.setValue(QStringLiteral("Hooks/custom_hooks"), value);
    emit hookSettingsChanged();
}

QString SettingsManager::hookWorkStartCommand() const {
    return m_settings.value(QStringLiteral("Hooks/cmd_work_start")).toString();
}

void SettingsManager::setHookWorkStartCommand(const QString& cmd) {
    if (hookWorkStartCommand() == cmd) return;
    m_settings.setValue(QStringLiteral("Hooks/cmd_work_start"), cmd);
    emit hookSettingsChanged();
}

QString SettingsManager::hookBreakStartCommand() const {
    return m_settings.value(QStringLiteral("Hooks/cmd_break_start")).toString();
}

void SettingsManager::setHookBreakStartCommand(const QString& cmd) {
    if (hookBreakStartCommand() == cmd) return;
    m_settings.setValue(QStringLiteral("Hooks/cmd_break_start"), cmd);
    emit hookSettingsChanged();
}

QString SettingsManager::hookPomodoroCompletedCommand() const {
    return m_settings.value(QStringLiteral("Hooks/cmd_pomodoro_completed")).toString();
}

void SettingsManager::setHookPomodoroCompletedCommand(const QString& cmd) {
    if (hookPomodoroCompletedCommand() == cmd) return;
    m_settings.setValue(QStringLiteral("Hooks/cmd_pomodoro_completed"), cmd);
    emit hookSettingsChanged();
}

QString SettingsManager::hookTaskCompletedCommand() const {
    return m_settings.value(QStringLiteral("Hooks/cmd_task_completed")).toString();
}

void SettingsManager::setHookTaskCompletedCommand(const QString& cmd) {
    if (hookTaskCompletedCommand() == cmd) return;
    m_settings.setValue(QStringLiteral("Hooks/cmd_task_completed"), cmd);
    emit hookSettingsChanged();
}

void SettingsManager::rebuildSchedule() {
    const int mode = timerMode();
    if (mode == Simple) {
        m_schedule.importRaw({taskDuration(), breakDuration()});
    } else if (mode == Custom) {
        const QVariantList customList = m_settings.value(QStringLiteral("Timer/custom_schedule")).toList();
        m_schedule.fromVariantList(customList);
        if (!m_schedule.isValid()) {
            m_schedule.importRaw({taskDuration(), breakDuration()});
        }
    } else {
        const int period = pomodoroPeriod();
        QVector<int> raw;
        raw.reserve(period * 2);
        for (int i = 0; i < period - 1; ++i) {
            raw.append(taskDuration());
            raw.append(breakDuration());
        }
        raw.append(taskDuration());
        raw.append(longBreakDuration());
        m_schedule.importRaw(raw);
    }
}

} // namespace gofi
