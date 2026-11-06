#pragma once

#include "Schedule.h"
#include <QObject>
#include <QPoint>
#include <QSettings>
#include <QSize>
#include <QVariantList>

namespace gofi {

class SettingsManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(int taskDuration READ taskDuration WRITE setTaskDuration NOTIFY timerDurationChanged)
    Q_PROPERTY(int breakDuration READ breakDuration WRITE setBreakDuration NOTIFY timerDurationChanged)
    Q_PROPERTY(int longBreakDuration READ longBreakDuration WRITE setLongBreakDuration NOTIFY timerDurationChanged)
    Q_PROPERTY(int pomodoroPeriod READ pomodoroPeriod WRITE setPomodoroPeriod NOTIFY timerDurationChanged)
    Q_PROPERTY(int reminderTime READ reminderTime WRITE setReminderTime NOTIFY reminderTimeChanged)
    Q_PROPERTY(int timerMode READ timerMode WRITE setTimerMode NOTIFY timerModeChanged)
    Q_PROPERTY(QVariantList customSchedule READ customScheduleVariant WRITE setCustomScheduleVariant NOTIFY timerDurationChanged)
    Q_PROPERTY(bool resumeTasksAfterBreak READ resumeTasksAfterBreak WRITE setResumeTasksAfterBreak NOTIFY resumeTasksAfterBreakChanged)
    Q_PROPERTY(bool resetTimerOnTaskSwitch READ resetTimerOnTaskSwitch WRITE setResetTimerOnTaskSwitch NOTIFY resetTimerOnTaskSwitchChanged)
    Q_PROPERTY(bool newTasksOnTop READ newTasksOnTop WRITE setNewTasksOnTop NOTIFY newTasksOnTopChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    Q_PROPERTY(int colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged)
    Q_PROPERTY(bool useDarkTheme READ useDarkTheme WRITE setUseDarkTheme NOTIFY useDarkThemeChanged)
    Q_PROPERTY(bool useSmallToolbarIcons READ useSmallToolbarIcons WRITE setUseSmallToolbarIcons NOTIFY appearanceChanged)
    Q_PROPERTY(bool switcherUseIcons READ switcherUseIcons WRITE setSwitcherUseIcons NOTIFY appearanceChanged)
    Q_PROPERTY(bool useHeaderBar READ useHeaderBar WRITE setUseHeaderBar NOTIFY appearanceChanged)
    Q_PROPERTY(bool muteSounds READ muteSounds WRITE setMuteSounds NOTIFY soundSettingsChanged)
    Q_PROPERTY(QString reminderSoundFile READ reminderSoundFile WRITE setReminderSoundFile NOTIFY soundSettingsChanged)
    Q_PROPERTY(QString breakStartSoundFile READ breakStartSoundFile WRITE setBreakStartSoundFile NOTIFY soundSettingsChanged)
    Q_PROPERTY(QString breakEndSoundFile READ breakEndSoundFile WRITE setBreakEndSoundFile NOTIFY soundSettingsChanged)
    Q_PROPERTY(double reminderSoundVolume READ reminderSoundVolume WRITE setReminderSoundVolume NOTIFY soundSettingsChanged)
    Q_PROPERTY(double breakStartSoundVolume READ breakStartSoundVolume WRITE setBreakStartSoundVolume NOTIFY soundSettingsChanged)
    Q_PROPERTY(double breakEndSoundVolume READ breakEndSoundVolume WRITE setBreakEndSoundVolume NOTIFY soundSettingsChanged)
    Q_PROPERTY(bool trayIndicatorEnabled READ trayIndicatorEnabled WRITE setTrayIndicatorEnabled NOTIFY pluginsChanged)
    Q_PROPERTY(bool launcherIndicatorEnabled READ launcherIndicatorEnabled WRITE setLauncherIndicatorEnabled NOTIFY pluginsChanged)

public:
    enum TimerMode {
        Simple = 0,
        Pomodoro = 1,
        Custom = 2
    };
    Q_ENUM(TimerMode)

    enum ColorScheme {
        DefaultScheme = 0,
        LightScheme = 1,
        DarkScheme = 2
    };
    Q_ENUM(ColorScheme)

    explicit SettingsManager(QObject* parent = nullptr);

    [[nodiscard]] int taskDuration() const;
    void setTaskDuration(int seconds);

    [[nodiscard]] int breakDuration() const;
    void setBreakDuration(int seconds);

    [[nodiscard]] int longBreakDuration() const;
    void setLongBreakDuration(int seconds);

    [[nodiscard]] int pomodoroPeriod() const;
    void setPomodoroPeriod(int period);

    [[nodiscard]] int reminderTime() const;
    void setReminderTime(int seconds);

    [[nodiscard]] bool reminderActive() const { return reminderTime() > 0; }

    [[nodiscard]] int timerMode() const;
    void setTimerMode(int mode);

    [[nodiscard]] QVariantList customScheduleVariant() const;
    void setCustomScheduleVariant(const QVariantList& pairs);

    [[nodiscard]] bool resumeTasksAfterBreak() const;
    void setResumeTasksAfterBreak(bool value);

    [[nodiscard]] bool resetTimerOnTaskSwitch() const;
    void setResetTimerOnTaskSwitch(bool value);

    [[nodiscard]] bool newTasksOnTop() const;
    void setNewTasksOnTop(bool value);

    [[nodiscard]] QString language() const;
    void setLanguage(const QString& lang);
    [[nodiscard]] bool isUkrainianActive() const;

    [[nodiscard]] int colorScheme() const;
    void setColorScheme(int scheme);

    [[nodiscard]] bool useDarkTheme() const;
    void setUseDarkTheme(bool value);

    [[nodiscard]] bool useSmallToolbarIcons() const;
    void setUseSmallToolbarIcons(bool value);

    [[nodiscard]] bool switcherUseIcons() const;
    void setSwitcherUseIcons(bool value);

    [[nodiscard]] bool useHeaderBar() const;
    void setUseHeaderBar(bool value);

    [[nodiscard]] bool muteSounds() const;
    void setMuteSounds(bool value);

    [[nodiscard]] QString reminderSoundFile() const;
    void setReminderSoundFile(const QString& file);

    [[nodiscard]] QString breakStartSoundFile() const;
    void setBreakStartSoundFile(const QString& file);

    [[nodiscard]] QString breakEndSoundFile() const;
    void setBreakEndSoundFile(const QString& file);

    [[nodiscard]] double reminderSoundVolume() const;
    void setReminderSoundVolume(double vol);

    [[nodiscard]] double breakStartSoundVolume() const;
    void setBreakStartSoundVolume(double vol);

    [[nodiscard]] double breakEndSoundVolume() const;
    void setBreakEndSoundVolume(double vol);

    [[nodiscard]] bool trayIndicatorEnabled() const;
    void setTrayIndicatorEnabled(bool enabled);

    [[nodiscard]] bool launcherIndicatorEnabled() const;
    void setLauncherIndicatorEnabled(bool enabled);

    [[nodiscard]] Schedule schedule() const { return m_schedule; }

    [[nodiscard]] QSize windowSize() const;
    void setWindowSize(const QSize& size);

    [[nodiscard]] QPoint windowPosition() const;
    void setWindowPosition(const QPoint& pos);

    [[nodiscard]] QString lastLoadedListId() const;
    void setLastLoadedListId(const QString& id);

signals:
    void timerDurationChanged();
    void reminderTimeChanged();
    void timerModeChanged();
    void resumeTasksAfterBreakChanged();
    void resetTimerOnTaskSwitchChanged();
    void newTasksOnTopChanged();
    void languageChanged();
    void colorSchemeChanged();
    void useDarkThemeChanged();
    void appearanceChanged();
    void soundSettingsChanged();
    void pluginsChanged();

private:
    void rebuildSchedule();

    mutable QSettings m_settings;
    Schedule m_schedule;
};

} // namespace gofi
