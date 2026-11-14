#include "DesktopIntegration.h"
#include "AppController.h"
#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryFile>
#include <QTimer>
#include <QWindow>

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
    if (m_controller) {
        connect(m_controller, &AppController::todoCountsChanged, this, &DesktopIntegration::updateTrayState);
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

DesktopIntegration::~DesktopIntegration() {
    delete m_trayMenu;
    m_trayMenu = nullptr;
}

void DesktopIntegration::showTrayMessage(const QString& title, const QString& body) {
    if (m_trayIcon && m_trayIcon->isVisible()) {
        m_trayIcon->showMessage(title, body, QSystemTrayIcon::Information, 4000);
    }
}

void DesktopIntegration::setMiniPlayerVisible(bool visible) {
    if (m_toggleMiniPlayerAction && m_toggleMiniPlayerAction->isChecked() != visible) {
        const QSignalBlocker blocker(m_toggleMiniPlayerAction);
        m_toggleMiniPlayerAction->setChecked(visible);
    }
}

void DesktopIntegration::setupTrayIcon() {
    if (!qobject_cast<QApplication*>(QCoreApplication::instance())) {
        return;
    }

    m_trayMenu = new QMenu();
    QAction* openAct = m_trayMenu->addAction(tr("Open Go for It Qt"));
    connect(openAct, &QAction::triggered, this, &DesktopIntegration::showWindowRequested);

    m_toggleMiniPlayerAction = m_trayMenu->addAction(tr("Always on Top Mini-Player"));
    m_toggleMiniPlayerAction->setCheckable(true);
    connect(m_toggleMiniPlayerAction, &QAction::triggered, this, &DesktopIntegration::toggleMiniPlayerRequested);

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

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    m_trayIcon = new QSystemTrayIcon(this);

    QIcon icon = QIcon::fromTheme(QStringLiteral("go-for-it-qt"), QIcon::fromTheme(QStringLiteral("com.github.jmoerman.go-for-it")));
    if (icon.isNull()) {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QStringList svgCandidates = {
            appDir + QStringLiteral("/data/icons/48x48/apps/go-for-it.svg"),
            appDir + QStringLiteral("/../share/icons/hicolor/scalable/apps/go-for-it-qt.svg"),
            appDir + QStringLiteral("/../data/icons/48x48/apps/go-for-it.svg")
        };
        for (const QString& cand : svgCandidates) {
            if (QFileInfo::exists(cand)) {
                icon = QIcon(cand);
                break;
            }
        }
    }
    if (icon.isNull() && QApplication::style()) {
        icon = QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    m_baseIcon = icon;
    m_trayIcon->setIcon(icon);

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

    const int unviewed = m_controller ? m_controller->unviewedTodoCount() : 0;
    const int total = m_controller ? m_controller->totalTodoCount() : 0;
    m_trayIcon->setIcon(renderBadgeIcon(unviewed, total));

    const QString todoCounter = tr("To-Do: %1 unread / %2 total").arg(unviewed).arg(total);

    const bool hasTask = m_timer->hasActiveTask();
    m_markDoneAction->setEnabled(hasTask && !m_timer->isBreakActive());
    m_nextTaskAction->setEnabled(hasTask);
    m_prevTaskAction->setEnabled(hasTask);
    m_toggleTimerAction->setEnabled(hasTask);
    m_toggleTimerAction->setText(m_timer->isRunning() ? tr("Stop the timer") : tr("Start the timer"));

    if (!hasTask) {
        m_taskDescAction->setText(tr("No task has been selected"));
        m_trayIcon->setToolTip(QStringLiteral("Go for It Qt\n%1").arg(todoCounter));
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
        m_trayIcon->setToolTip(QStringLiteral("Go for It Qt — %1 (%2)\n%3").arg(label, m_taskDescAction->text(), todoCounter));
    } else {
        m_trayIcon->setToolTip(QStringLiteral("Go for It Qt — %1\n%2").arg(m_taskDescAction->text(), todoCounter));
    }
}

QIcon DesktopIntegration::renderBadgeIcon(int unviewed, int total) const {
    if (m_baseIcon.isNull()) {
        return m_baseIcon;
    }
    if (total <= 0) {
        return m_baseIcon;
    }

    QPixmap pixmap = m_baseIcon.pixmap(64, 64);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);

    const QString badgeText = QStringLiteral("%1/%2").arg(unviewed).arg(total);

    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(badgeText.length() > 4 ? 12 : 14);
    painter.setFont(font);

    const QFontMetrics fm(font);
    const int textWidth = fm.horizontalAdvance(badgeText);
    const int textHeight = fm.height();
    const int badgeWidth = qMax(textWidth + 10, 26);
    const int badgeHeight = qMax(textHeight + 2, 20);

    const int x = qMax(0, 64 - badgeWidth - 2);
    const int y = qMax(0, 64 - badgeHeight - 2);
    const QRect badgeRect(x, y, badgeWidth, badgeHeight);

    const QColor bgColor = (unviewed > 0) ? QColor(224, 49, 49) : QColor(52, 58, 64, 230);
    painter.setBrush(bgColor);
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawRoundedRect(badgeRect, badgeHeight / 2.0, badgeHeight / 2.0);

    painter.setPen(Qt::white);
    painter.drawText(badgeRect, Qt::AlignCenter, badgeText);
    painter.end();

    return QIcon(pixmap);
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
    if (QStandardPaths::isTestModeEnabled()) {
        return;
    }
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

bool DesktopIntegration::kwinIntegrationAvailable() const {
    if (QStandardPaths::isTestModeEnabled()) {
        return false;
    }
    const QString sessionType = qEnvironmentVariable("XDG_SESSION_TYPE").toLower();
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    if (!desktop.contains(QStringLiteral("kde")) && sessionType != QStringLiteral("wayland")) {
        return false;
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return false;
    }
    auto iface = bus.interface();
    return iface && iface->isServiceRegistered(QStringLiteral("org.kde.KWin"));
}

QString DesktopIntegration::kwinPinScript(const QString& windowTitle, bool alwaysOnTop) {
    QString winTitle = windowTitle;
    winTitle.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));

    // Target strictly this process's own mini player window by matching both our PID and the
    // exact window caption, so foreign windows (e.g. VLC/Spotify/browser tabs titled "Mini Player")
    // and our own main window are never modified.
    const QString script = QStringLiteral(
        "function applyTo(w) {\n"
        "    if (!w) return;\n"
        "    if (w.pid === %1 && w.caption === \"%2\") {\n"
        "        w.keepAbove = %3;\n"
        "        w.onAllDesktops = %3;\n"
        "    }\n"
        "}\n"
        "for (var i = 0; i < workspace.windowList().length; ++i) {\n"
        "    applyTo(workspace.windowList()[i]);\n"
        "}\n"
    ).arg(QString::number(QCoreApplication::applicationPid()), winTitle, alwaysOnTop ? QStringLiteral("true") : QStringLiteral("false"));
    return script;
}

static void executeKWinPinScript(const QString& script) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return;

    QTemporaryFile tempFile(QDir::tempPath() + QStringLiteral("/gofi_kwin_XXXXXX.js"));
    tempFile.setAutoRemove(true);
    if (!tempFile.open()) return;
    tempFile.write(script.toUtf8());
    tempFile.flush();
    const QString tempPath = tempFile.fileName();

    const QString pluginName = QStringLiteral("gofi_pin_%1_%2")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch());

    QDBusMessage loadMsg = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("/Scripting"),
        QStringLiteral("org.kde.kwin.Scripting"),
        QStringLiteral("loadScript")
    );
    loadMsg << tempPath << pluginName;
    QDBusReply<int> reply = bus.call(loadMsg);
    if (reply.isValid()) {
        const int scriptId = reply.value();
        const QString scriptPath = QStringLiteral("/Scripting/Script%1").arg(scriptId);
        QDBusMessage runMsg = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.KWin"),
            scriptPath,
            QStringLiteral("org.kde.kwin.Script"),
            QStringLiteral("run")
        );
        bus.call(runMsg);

        QDBusMessage unloadMsg = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.KWin"),
            QStringLiteral("/Scripting"),
            QStringLiteral("org.kde.kwin.Scripting"),
            QStringLiteral("unloadScript")
        );
        unloadMsg << pluginName;
        bus.call(unloadMsg);
    }
}

void DesktopIntegration::setKWinWindowProperties(QWindow* win, bool alwaysOnTop) {
    if (!win || QStandardPaths::isTestModeEnabled()) {
        return;
    }

    const QString script = kwinPinScript(win->title(), alwaysOnTop);

    // Execute immediately in case the window is already mapped
    executeKWinPinScript(script);

    // When the window is first shown, the Wayland compositor maps the surface asynchronously
    // (typically takes 50-150ms). Retry after short delays so KWin catches the window.
    for (int delayMs : {80, 200, 500}) {
        QTimer::singleShot(delayMs, [script]() {
            executeKWinPinScript(script);
        });
    }
}

} // namespace gofi
