#include "SystemHookService.h"
#include "AppController.h"
#include "SettingsManager.h"
#include "TaskTimer.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <signal.h>
#endif

namespace gofi {

static bool s_systemIntegrationEnabled = true;

static bool isProcessRunning(qint64 pid) {
    if (pid <= 0) return false;
#if defined(Q_OS_UNIX)
    if (kill(static_cast<pid_t>(pid), 0) == 0) {
        return true;
    }
    return errno != ESRCH;
#else
    return false;
#endif
}

void SystemHookService::setSystemIntegrationEnabled(bool enabled) {
    s_systemIntegrationEnabled = enabled;
}

bool SystemHookService::isSystemIntegrationEnabled() {
    return s_systemIntegrationEnabled;
}

SystemHookService::SystemHookService(AppController* controller,
                                     SettingsManager* settings,
                                     TaskTimer* timer,
                                     QObject* parent)
    : QObject(parent)
    , m_controller(controller)
    , m_settings(settings)
    , m_timer(timer)
{
    // Crash recovery: check if a previous run crashed while GNOME banners were disabled
    if (!QStandardPaths::isTestModeEnabled()) {
        QSettings crashState(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("settings"));
        const qint64 pid = crashState.value(QStringLiteral("State/gnome_banners_inhibited_pid"), 0).toLongLong();
        const bool legacyFlag = crashState.value(QStringLiteral("State/gnome_banners_inhibited"), false).toBool();

        if (pid > 0) {
            // Check if the owner process is still alive. If it's another active instance, leave DND intact.
            if (pid != QCoreApplication::applicationPid() && !isProcessRunning(pid)) {
                QProcess::execute(QStringLiteral("gsettings"),
                                 {QStringLiteral("set"),
                                  QStringLiteral("org.gnome.desktop.notifications"),
                                  QStringLiteral("show-banners"),
                                  QStringLiteral("true")});
                crashState.remove(QStringLiteral("State/gnome_banners_inhibited_pid"));
                crashState.remove(QStringLiteral("State/gnome_banners_inhibited"));
            }
        } else if (legacyFlag) {
            // Upgrade legacy flag: no PID was recorded, restore banners once and clear
            QProcess::execute(QStringLiteral("gsettings"),
                             {QStringLiteral("set"),
                              QStringLiteral("org.gnome.desktop.notifications"),
                              QStringLiteral("show-banners"),
                              QStringLiteral("true")});
            crashState.remove(QStringLiteral("State/gnome_banners_inhibited"));
        }
    }

    if (m_timer) {
        connect(m_timer, &TaskTimer::workStarted, this, &SystemHookService::onWorkStarted);
        connect(m_timer, &TaskTimer::breakStarted, this, &SystemHookService::onBreakStarted);
        connect(m_timer, &TaskTimer::timerStopped, this, &SystemHookService::onTimerStopped);
        connect(m_timer, &TaskTimer::timerFinished, this, [this](bool wasBreak) {
            if (!wasBreak) {
                onPomodoroCompleted();
            }
        });
    }

    if (m_settings) {
        if (m_settings->customHooksEnabled() && !QStandardPaths::isTestModeEnabled()) {
            ensureHooksDirectory();
        }
        connect(m_settings, &SettingsManager::hookSettingsChanged, this, [this]() {
            if (m_settings->customHooksEnabled() && !QStandardPaths::isTestModeEnabled()) {
                ensureHooksDirectory();
            }
            if (!m_settings->dndFocusEnabled() && m_dndActive) {
                applyDnd(false);
            }
            if (!m_settings->inhibitSleepEnabled() && m_sleepInhibitActive) {
                applySleepInhibit(false);
            }
        });
    }
}

SystemHookService::~SystemHookService() {
    applyDnd(false);
    applySleepInhibit(false);
    for (QProcess* proc : findChildren<QProcess*>()) {
        if (proc->state() != QProcess::NotRunning) {
            proc->waitForFinished(500);
            if (proc->state() != QProcess::NotRunning) {
                proc->kill();
            }
        }
    }
}

QString SystemHookService::hooksDirectory() const {
    if (!m_customHooksDirectory.isEmpty()) {
        return m_customHooksDirectory;
    }
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return base + QStringLiteral("/go-for-it-qt/hooks");
}

void SystemHookService::setCustomHooksDirectory(const QString& path) {
    m_customHooksDirectory = path;
    ensureHooksDirectory();
}

void SystemHookService::ensureHooksDirectory() {
    const QString dirPath = hooksDirectory();
    QDir().mkpath(dirPath);
    QDir dir(dirPath);
    if (!dir.exists()) {
        return;
    }

    const QString sampleScript = dir.filePath(QStringLiteral("sample_hook.sh"));
    if (!QFile::exists(sampleScript)) {
        QFile file(sampleScript);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            const char* content =
                "#!/usr/bin/env bash\n"
                "# Go for It Qt — Focus Mode & Event Hook Template\n"
                "#\n"
                "# Environment variables provided by Go for It Qt:\n"
                "#   GOFI_EVENT           Event name: work_start | break_start | pomodoro_completed | task_completed\n"
                "#   GOFI_TASK_TITLE      Title / description of the active task\n"
                "#   GOFI_TASK_KEY        Issue tracker key (e.g. PROJ-123, #42)\n"
                "#   GOFI_TASK_ID         Local database task ID\n"
                "#   GOFI_PHASE           Timer phase: work | break\n"
                "#   GOFI_DURATION_SEC    Configured duration in seconds\n"
                "#   GOFI_REMAINING_SEC   Remaining time in seconds\n"
                "#\n"
                "# Example actions:\n"
                "#   case \"$GOFI_EVENT\" in\n"
                "#     work_start)\n"
                "#       # Set Slack / Discord / Mattermost status:\n"
                "#       # curl -X POST -H 'Authorization: Bearer ...' ...\n"
                "#       # Dim smart lights via Home Assistant / Philips Hue:\n"
                "#       # curl -s -X POST http://homeassistant.local:8123/api/services/light/turn_on ...\n"
                "#       ;;\n"
                "#     break_start)\n"
                "#       # Restore online status / notify smart home\n"
                "#       ;;\n"
                "#     task_completed)\n"
                "#       # Trigger git commit or webhook\n"
                "#       ;;\n"
                "#   esac\n\n"
                "echo \"[Hook] $GOFI_EVENT: $GOFI_TASK_TITLE ($GOFI_PHASE)\"\n";
            file.write(content);
            file.close();
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                QFile::ReadGroup | QFile::ExeGroup |
                                QFile::ReadOther | QFile::ExeOther);
        }
    }
}

void SystemHookService::openHooksFolder() {
    ensureHooksDirectory();
    QDesktopServices::openUrl(QUrl::fromLocalFile(hooksDirectory()));
}

void SystemHookService::applyDnd(bool enable) {
    if (enable && (!m_settings || !m_settings->dndFocusEnabled())) {
        return;
    }

    if (enable == m_dndActive) return;

    if (!isSystemIntegrationEnabled() || QStandardPaths::isTestModeEnabled()) {
        m_dndActive = enable;
        emit dndFocusActiveChanged();
        return;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (enable) {
        // 1. Freedesktop Notifications Inhibit
        if (bus.isConnected()) {
            QDBusMessage msg = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("/org/freedesktop/Notifications"),
                QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("Inhibit")
            );
            msg << QStringLiteral("go-for-it-qt")
                << QStringLiteral("Pomodoro Focus Session")
                << QVariantMap();
            QDBusReply<uint> reply = bus.call(msg);
            if (reply.isValid()) {
                m_dndCookie = reply.value();
            }
        }

        // 2. GNOME Shell fallback: gsettings set org.gnome.desktop.notifications show-banners false
        const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
        if (desktop.contains(QStringLiteral("gnome")) || desktop.contains(QStringLiteral("unity"))) {
            QProcess queryProc;
            queryProc.start(QStringLiteral("gsettings"),
                           {QStringLiteral("get"),
                            QStringLiteral("org.gnome.desktop.notifications"),
                            QStringLiteral("show-banners")});
            if (queryProc.waitForFinished(500)) {
                const QString val = QString::fromUtf8(queryProc.readAllStandardOutput()).trimmed();
                if (val == QStringLiteral("true")) {
                    QProcess::execute(QStringLiteral("gsettings"),
                                     {QStringLiteral("set"),
                                      QStringLiteral("org.gnome.desktop.notifications"),
                                      QStringLiteral("show-banners"),
                                      QStringLiteral("false")});
                    m_gnomeBannersDisabled = true;
                    QSettings crashSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("settings"));
                    crashSettings.setValue(QStringLiteral("State/gnome_banners_inhibited_pid"), QCoreApplication::applicationPid());
                    crashSettings.setValue(QStringLiteral("State/gnome_banners_inhibited"), true);
                }
            }
        }

        m_dndActive = true;
    } else {
        // Un-inhibit
        if (m_dndCookie != 0 && bus.isConnected()) {
            QDBusMessage msg = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("/org/freedesktop/Notifications"),
                QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("UnInhibit")
            );
            msg << m_dndCookie;
            bus.call(msg);
            m_dndCookie = 0;
        }

        if (m_gnomeBannersDisabled) {
            QProcess::execute(QStringLiteral("gsettings"),
                             {QStringLiteral("set"),
                              QStringLiteral("org.gnome.desktop.notifications"),
                              QStringLiteral("show-banners"),
                              QStringLiteral("true")});
            m_gnomeBannersDisabled = false;
            QSettings crashSettings(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("settings"));
            crashSettings.remove(QStringLiteral("State/gnome_banners_inhibited_pid"));
            crashSettings.remove(QStringLiteral("State/gnome_banners_inhibited"));
        }

        m_dndActive = false;
    }
    emit dndFocusActiveChanged();
}

void SystemHookService::applySleepInhibit(bool enable) {
    if (enable && (!m_settings || !m_settings->inhibitSleepEnabled())) {
        return;
    }

    if (enable == m_sleepInhibitActive) return;

    if (!isSystemIntegrationEnabled() || QStandardPaths::isTestModeEnabled()) {
        m_sleepInhibitActive = enable;
        emit sleepInhibitActiveChanged();
        return;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (enable) {
        if (bus.isConnected()) {
            QDBusMessage msg = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.ScreenSaver"),
                QStringLiteral("/org/freedesktop/ScreenSaver"),
                QStringLiteral("org.freedesktop.ScreenSaver"),
                QStringLiteral("Inhibit")
            );
            msg << QStringLiteral("go-for-it-qt")
                << QStringLiteral("Pomodoro focus work session");
            QDBusReply<uint> reply = bus.call(msg);
            if (reply.isValid()) {
                m_sleepCookie = reply.value();
            }
        }
        m_sleepInhibitActive = true;
    } else {
        if (m_sleepCookie != 0 && bus.isConnected()) {
            QDBusMessage msg = QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.ScreenSaver"),
                QStringLiteral("/org/freedesktop/ScreenSaver"),
                QStringLiteral("org.freedesktop.ScreenSaver"),
                QStringLiteral("UnInhibit")
            );
            msg << m_sleepCookie;
            bus.call(msg);
            m_sleepCookie = 0;
        }
        m_sleepInhibitActive = false;
    }
    emit sleepInhibitActiveChanged();
}

void SystemHookService::runCommandOrScript(const QString& eventName,
                                          const QString& customCommand,
                                          const QString& scriptBaseName,
                                          const QProcessEnvironment& env)
{
    QString programToRun;
    QStringList args;

    if (!customCommand.trimmed().isEmpty()) {
        programToRun = QStringLiteral("/bin/sh");
        args << QStringLiteral("-c") << customCommand;
    } else {
        const QDir dir(hooksDirectory());
        const QString withSh = dir.filePath(scriptBaseName + QStringLiteral(".sh"));
        const QString withoutSh = dir.filePath(scriptBaseName);
        if (QFileInfo::exists(withSh)) {
            programToRun = withSh;
        } else if (QFileInfo::exists(withoutSh)) {
            programToRun = withoutSh;
        }
    }

    if (programToRun.isEmpty()) {
        return;
    }

    if (!isSystemIntegrationEnabled()) {
        emit hookExecuted(eventName, true, QStringLiteral("[Simulated in test mode]"));
        return;
    }

    auto* proc = new QProcess(this);
    proc->setProcessEnvironment(env);
    connect(proc, &QProcess::finished, [this, proc, eventName](int exitCode, QProcess::ExitStatus) {
        const QString out = QString::fromUtf8(proc->readAllStandardOutput() + proc->readAllStandardError()).trimmed();
        emit hookExecuted(eventName, exitCode == 0, out);
        proc->deleteLater();
    });
    connect(proc, &QProcess::errorOccurred, [this, proc, eventName](QProcess::ProcessError err) {
        emit hookExecuted(eventName, false, QStringLiteral("Process execution error: %1").arg(err));
        proc->deleteLater();
    });

    if (args.isEmpty()) {
        proc->start(programToRun);
    } else {
        proc->start(programToRun, args);
    }
}

void SystemHookService::triggerHook(const QString& eventName,
                                    const QString& taskTitle,
                                    const QString& taskKey,
                                    const QString& taskId)
{
    if (!m_settings || !m_settings->customHooksEnabled()) {
        return;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GOFI_EVENT"), eventName);
    env.insert(QStringLiteral("GOFI_TASK_TITLE"), taskTitle);
    env.insert(QStringLiteral("GOFI_TASK_KEY"), taskKey);
    env.insert(QStringLiteral("GOFI_TASK_ID"), taskId);
    env.insert(QStringLiteral("GOFI_PHASE"), (m_timer && m_timer->isBreakActive()) ? QStringLiteral("break") : QStringLiteral("work"));
    env.insert(QStringLiteral("GOFI_DURATION_SEC"), QString::number(m_timer ? m_timer->remainingSeconds() : 0));

    QString customCmd;
    QString scriptName;

    if (eventName == QStringLiteral("work_start")) {
        customCmd = m_settings->hookWorkStartCommand();
        scriptName = QStringLiteral("on_work_start");
    } else if (eventName == QStringLiteral("break_start")) {
        customCmd = m_settings->hookBreakStartCommand();
        scriptName = QStringLiteral("on_break_start");
    } else if (eventName == QStringLiteral("pomodoro_completed")) {
        customCmd = m_settings->hookPomodoroCompletedCommand();
        scriptName = QStringLiteral("on_pomodoro_completed");
    } else if (eventName == QStringLiteral("task_completed")) {
        customCmd = m_settings->hookTaskCompletedCommand();
        scriptName = QStringLiteral("on_task_completed");
    }

    runCommandOrScript(eventName, customCmd, scriptName, env);
}

void SystemHookService::onWorkStarted() {
    applyDnd(true);
    applySleepInhibit(true);

    const QString title = m_timer ? m_timer->activeTaskDescription() : QString();
    triggerHook(QStringLiteral("work_start"), title);
}

void SystemHookService::onBreakStarted() {
    applyDnd(false);
    applySleepInhibit(false);

    const QString title = m_timer ? m_timer->activeTaskDescription() : QString();
    triggerHook(QStringLiteral("break_start"), title);
}

void SystemHookService::onPomodoroCompleted() {
    const QString title = m_timer ? m_timer->activeTaskDescription() : QString();
    triggerHook(QStringLiteral("pomodoro_completed"), title);
}

void SystemHookService::onTimerStopped() {
    applyDnd(false);
    applySleepInhibit(false);
}

void SystemHookService::onTaskCompleted(const QString& taskTitle,
                                       const QString& taskKey,
                                       const QString& taskId)
{
    triggerHook(QStringLiteral("task_completed"), taskTitle, taskKey, taskId);
}

bool SystemHookService::testHook(const QString& eventName) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GOFI_EVENT"), eventName);
    env.insert(QStringLiteral("GOFI_TASK_TITLE"), QStringLiteral("Test Task [PROJ-42]"));
    env.insert(QStringLiteral("GOFI_TASK_KEY"), QStringLiteral("PROJ-42"));
    env.insert(QStringLiteral("GOFI_TASK_ID"), QStringLiteral("999"));
    env.insert(QStringLiteral("GOFI_PHASE"), eventName.contains(QStringLiteral("break")) ? QStringLiteral("break") : QStringLiteral("work"));
    env.insert(QStringLiteral("GOFI_DURATION_SEC"), QStringLiteral("1500"));

    QString customCmd;
    QString scriptName;

    if (eventName == QStringLiteral("work_start")) {
        customCmd = m_settings ? m_settings->hookWorkStartCommand() : QString();
        scriptName = QStringLiteral("on_work_start");
    } else if (eventName == QStringLiteral("break_start")) {
        customCmd = m_settings ? m_settings->hookBreakStartCommand() : QString();
        scriptName = QStringLiteral("on_break_start");
    } else if (eventName == QStringLiteral("pomodoro_completed")) {
        customCmd = m_settings ? m_settings->hookPomodoroCompletedCommand() : QString();
        scriptName = QStringLiteral("on_pomodoro_completed");
    } else if (eventName == QStringLiteral("task_completed")) {
        customCmd = m_settings ? m_settings->hookTaskCompletedCommand() : QString();
        scriptName = QStringLiteral("on_task_completed");
    } else {
        return false;
    }

    if (customCmd.trimmed().isEmpty()) {
        const QDir dir(hooksDirectory());
        const QString withSh = dir.filePath(scriptName + QStringLiteral(".sh"));
        const QString withoutSh = dir.filePath(scriptName);
        if (!QFileInfo::exists(withSh) && !QFileInfo::exists(withoutSh)) {
            // If neither custom command nor script file exists, test a fallback echo to confirm hook runner works
            customCmd = QStringLiteral("echo \"[Test Hook OK] %1 executed successfully\"").arg(eventName);
        }
    }

    runCommandOrScript(eventName, customCmd, scriptName, env);
    return true;
}

} // namespace gofi
