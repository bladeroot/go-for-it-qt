#include "DesktopIntegration.h"
#include "AppController.h"
#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QFileInfo>
#include <QIcon>
#include <QStyle>

namespace gofi {

DesktopIntegration::DesktopIntegration(AppController* controller,
                                       SettingsManager* settings,
                                       TaskTimer* timer,
                                       QObject* parent)
    : QObject(parent)
    , m_controller(controller)
    , m_settings(settings)
    , m_timer(timer)
{
    setupTrayIcon();

    if (m_settings) {
        connect(m_settings, &SettingsManager::pluginsChanged, this, &DesktopIntegration::updatePluginStates);
    }
    if (m_timer) {
        connect(m_timer, &TaskTimer::timerUpdated, this, &DesktopIntegration::updateTrayState);
        connect(m_timer, &TaskTimer::runningChanged, this, &DesktopIntegration::updateTrayState);
        connect(m_timer, &TaskTimer::activeTaskChanged, this, &DesktopIntegration::updateTrayState);
        connect(m_timer, &TaskTimer::breakActiveChanged, this, &DesktopIntegration::updateTrayState);

        connect(m_timer, &TaskTimer::timerUpdated, this, &DesktopIntegration::updateLauncherBadge);
        connect(m_timer, &TaskTimer::runningChanged, this, &DesktopIntegration::updateLauncherBadge);
    }

    updatePluginStates();
}

void DesktopIntegration::showTrayMessage(const QString& title, const QString& body) {
    if (m_trayIcon && m_trayIcon->isVisible()) {
        m_trayIcon->showMessage(title, body, QSystemTrayIcon::Information, 4000);
    }
}

void DesktopIntegration::setupTrayIcon() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    m_trayIcon = new QSystemTrayIcon(this);

    QIcon icon = QIcon::fromTheme(QStringLiteral("go-for-it-qt"), QIcon::fromTheme(QStringLiteral("com.github.jmoerman.go-for-it")));
#ifdef GOFI_SOURCE_DATADIR
    if (icon.isNull()) {
        const QString svgPath = QStringLiteral(GOFI_SOURCE_DATADIR) + QStringLiteral("/icons/48x48/apps/go-for-it.svg");
        if (QFileInfo::exists(svgPath)) {
            icon = QIcon(svgPath);
        }
    }
#endif
    if (icon.isNull() && QApplication::style()) {
        icon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    m_trayIcon->setIcon(icon);

    m_trayMenu = new QMenu();
    QAction* openAct = m_trayMenu->addAction(tr("Open Go for It Qt"));
    connect(openAct, &QAction::triggered, this, &DesktopIntegration::showWindowRequested);

    QAction* closeAct = m_trayMenu->addAction(tr("Close"));
    connect(closeAct, &QAction::triggered, this, &DesktopIntegration::quitRequested);

    m_trayMenu->addSeparator();

    m_taskDescAction = m_trayMenu->addAction(tr("No task has been selected"));
    m_taskDescAction->setEnabled(false);

    m_trayMenu->addSeparator();

    m_markDoneAction = m_trayMenu->addAction(tr("Mark the task as complete"));
    connect(m_markDoneAction, &QAction::triggered, m_controller, &AppController::markActiveTaskDone);

    m_nextTaskAction = m_trayMenu->addAction(tr("Switch to next task"));
    connect(m_nextTaskAction, &QAction::triggered, m_controller, &AppController::switchToNextTask);

    m_prevTaskAction = m_trayMenu->addAction(tr("Switch to the previous task"));
    connect(m_prevTaskAction, &QAction::triggered, m_controller, &AppController::switchToPreviousTask);

    m_toggleTimerAction = m_trayMenu->addAction(tr("Start the timer"));
    connect(m_toggleTimerAction, &QAction::triggered, m_timer, &TaskTimer::toggleRunning);

    m_trayIcon->setContextMenu(m_trayMenu);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
            emit showWindowRequested();
        }
    });
}

void DesktopIntegration::updatePluginStates() {
    if (m_trayIcon) {
        const bool enableTray = !m_settings || m_settings->trayIndicatorEnabled();
        m_trayIcon->setVisible(enableTray);
        if (enableTray) {
            updateTrayState();
        }
    }
    updateLauncherBadge();
}

void DesktopIntegration::updateTrayState() {
    if (!m_trayIcon || !m_timer) {
        return;
    }

    const bool hasTask = m_timer->hasActiveTask();
    m_markDoneAction->setEnabled(hasTask && !m_timer->isBreakActive());
    m_nextTaskAction->setEnabled(hasTask);
    m_prevTaskAction->setEnabled(hasTask);
    m_toggleTimerAction->setEnabled(hasTask);
    m_toggleTimerAction->setText(m_timer->isRunning() ? tr("Stop the timer") : tr("Start the timer"));

    if (!hasTask) {
        m_taskDescAction->setText(tr("No task has been selected"));
        m_trayIcon->setToolTip(QStringLiteral("Go for It Qt"));
        return;
    }

    if (m_timer->isBreakActive()) {
        m_taskDescAction->setText(tr("Take a Break!"));
    } else {
        QString desc = m_timer->activeTaskDescription();
        if (desc.size() > 30) {
            desc = desc.left(27) + QStringLiteral("…");
        }
        m_taskDescAction->setText(desc);
    }

    if (m_timer->isRunning()) {
        const uint rem = m_timer->remainingSeconds();
        const uint h = rem / 3600U;
        uint m = (rem / 60U) % 60U;
        const uint s = rem % 60U;
        QString label;
        if (h > 0 || m > 0) {
            if (s >= 30) ++m;
            label = (h > 0) ? QStringLiteral("%1h-%2m").arg(h).arg(m) : QStringLiteral("%1m").arg(m);
        } else {
            label = QStringLiteral("%1s").arg(s);
        }
        m_trayIcon->setToolTip(QStringLiteral("Go for It Qt — %1 (%2)").arg(label, m_taskDescAction->text()));
    } else {
        m_trayIcon->setToolTip(QStringLiteral("Go for It Qt — %1").arg(m_taskDescAction->text()));
    }
}

void DesktopIntegration::updateLauncherBadge() {
    if (!m_timer) return;

    const bool enabled = (!m_settings || m_settings->launcherIndicatorEnabled()) && m_timer->isRunning();
    if (!enabled) {
        if (m_lastLauncherBadgeVisible) {
            m_lastLauncherBadgeVisible = false;
            m_lastLauncherBadgeCount = -1;
            emitUnityLauncherUpdate({
                {QStringLiteral("count-visible"), false},
                {QStringLiteral("progress-visible"), false}
            });
        }
        return;
    }

    const uint rem = m_timer->remainingSeconds();
    uint minutes = rem / 60U;
    const uint seconds = rem % 60U;
    qint64 countToShow = seconds;
    if (minutes > 0) {
        if (seconds >= 30) ++minutes;
        countToShow = minutes;
    }

    if (!m_lastLauncherBadgeVisible || countToShow != m_lastLauncherBadgeCount) {
        m_lastLauncherBadgeVisible = true;
        m_lastLauncherBadgeCount = countToShow;
        emitUnityLauncherUpdate({
            {QStringLiteral("count-visible"), true},
            {QStringLiteral("count"), countToShow},
            {QStringLiteral("progress-visible"), true},
            {QStringLiteral("progress"), m_timer->progress()}
        });
    }
}

void DesktopIntegration::emitUnityLauncherUpdate(const QVariantMap& properties) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    QDBusMessage sig = QDBusMessage::createSignal(
        QStringLiteral("/com/canonical/unity/launcherentry/goforit"),
        QStringLiteral("com.canonical.Unity.LauncherEntry"),
        QStringLiteral("Update")
    );
    sig << QStringLiteral("application://go-for-it-qt.desktop")
        << properties;
    bus.send(sig);
}

} // namespace gofi
