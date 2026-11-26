#include "DesktopIntegration.h"
#include "AppController.h"
#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QIcon>
#include <QPainter>
#include <QPointer>
#include <QStandardPaths>
#include <QStyle>
#include <QTemporaryFile>
#include <QTimer>
#include <QWindow>
#include <atomic>
#include <memory>

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

    connect(&m_positionReceiver, &MiniPlayerPositionReceiver::positionReported,
            this, &DesktopIntegration::miniPlayerPositionReported);

    updatePluginStates();
}

DesktopIntegration::~DesktopIntegration() {
    if (!m_trackerPluginName.isEmpty()) {
        // Blocking: an asynchronous call may never leave the process while it shuts down.
        QDBusMessage unloadMsg = QDBusMessage::createMethodCall(
            QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"),
            QStringLiteral("org.kde.kwin.Scripting"), QStringLiteral("unloadScript"));
        unloadMsg << m_trackerPluginName;
        QDBusConnection::sessionBus().call(unloadMsg, QDBus::Block, 500);
        m_trackerPluginName.clear();
    }
    if (m_positionReceiverRegistered) {
        QDBusConnection::sessionBus().unregisterObject(QString::fromLatin1(MiniPlayerPositionReceiver::ObjectPath));
    }
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

// JS escaping for a string placed inside double quotes in a KWin script.
static QString kwinJsString(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n")).replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    return value;
}

// `isGofiMiniPlayer(w)`: true only for this process's own mini player window.
// 1. Process identity: match our PID if present; for native Wayland clients where KWin
//    reports pid=0/undefined, fall back to matching resourceClass/desktopFileName "go-for-it-qt".
// 2. Caption identity: the caption or normalCaption equals the (already translated) mini player
//    title or starts with it (KWin may append suffixes). Keyword matching such as "Mini" is not
//    used: our main window and task dialogs carry user-chosen list/task names (KB-131).
static QString kwinMiniPlayerMatcher(const QString& windowTitle) {
    return QStringLiteral(
        "function isGofiMiniPlayer(w) {\n"
        "    if (!w) return false;\n"
        "    var isOwn = (w.pid && w.pid === %1) || (!w.pid && (w.resourceClass === \"go-for-it-qt\" || (w.desktopFileName && w.desktopFileName.indexOf(\"go-for-it\") !== -1)));\n"
        "    if (!isOwn) return false;\n"
        "    var c = w.caption || \"\";\n"
        "    var nc = (typeof w.normalCaption !== \"undefined\" && w.normalCaption) ? w.normalCaption : \"\";\n"
        "    return (c === \"%2\" || nc === \"%2\" || c.indexOf(\"%2\") === 0 || nc.indexOf(\"%2\") === 0);\n"
        "}\n"
    ).arg(QString::number(QCoreApplication::applicationPid()), kwinJsString(windowTitle));
}

QString DesktopIntegration::kwinPinScript(const QString& windowTitle, bool alwaysOnTop) {
    return kwinMiniPlayerMatcher(windowTitle) + QStringLiteral(
        "function applyTo(w) {\n"
        "    if (isGofiMiniPlayer(w)) {\n"
        "        w.keepAbove = %1;\n"
        "        w.onAllDesktops = %1;\n"
        "    }\n"
        "}\n"
        "for (var i = 0; i < workspace.windowList().length; ++i) {\n"
        "    applyTo(workspace.windowList()[i]);\n"
        "}\n"
    ).arg(alwaysOnTop ? QStringLiteral("true") : QStringLiteral("false"));
}

QString DesktopIntegration::kwinMiniPlayerTrackerScript(const QString& windowTitle, std::optional<QPoint> position,
                                                        const QString& replyService) {
    // The window may not be mapped yet when the script runs, so also watch windowAdded. KWin 6
    // hands out frameGeometry as a read-only KWin::RectF copy (setting .x on it does nothing) and
    // has no Qt.rect, so the move assigns a plain {x, y, width, height} object.
    return kwinMiniPlayerMatcher(windowTitle) + QStringLiteral(
        "var gofiTracked = [];\n"
        "function gofiTrack(w) {\n"
        "    if (!isGofiMiniPlayer(w) || gofiTracked.indexOf(w) !== -1) return;\n"
        "    gofiTracked.push(w);\n"
        "    if (%1) {\n"
        "        var g = w.frameGeometry;\n"
        "        w.frameGeometry = {x: %2, y: %3, width: g.width, height: g.height};\n"
        "    }\n"
        "    w.interactiveMoveResizeFinished.connect(function() {\n"
        "        var f = w.frameGeometry;\n"
        "        callDBus(\"%4\", \"%5\", \"%6\", \"reportPosition\", Math.round(f.x), Math.round(f.y));\n"
        "    });\n"
        "}\n"
        "var gofiWindows = workspace.windowList();\n"
        "for (var i = 0; i < gofiWindows.length; ++i) {\n"
        "    gofiTrack(gofiWindows[i]);\n"
        "}\n"
        "workspace.windowAdded.connect(gofiTrack);\n"
    ).arg(position ? QStringLiteral("true") : QStringLiteral("false"),
          QString::number(position ? position->x() : 0),
          QString::number(position ? position->y() : 0),
          kwinJsString(replyService),
          QString::fromLatin1(MiniPlayerPositionReceiver::ObjectPath),
          QStringLiteral("io.github.bladeroot.GoForItQt.MiniPlayer"));
}

std::optional<QPoint> DesktopIntegration::restorableMiniPlayerPosition(std::optional<QPoint> saved, const QSize& size,
                                                                       const QList<QRect>& screenAreas) {
    if (!saved) {
        return std::nullopt;
    }
    const QPoint centre = QRect(*saved, size.isValid() ? size : QSize(1, 1)).center();
    for (const QRect& area : screenAreas) {
        if (area.contains(centre)) {
            return saved;
        }
    }
    return std::nullopt;
}

static void executeKWinPinScript(const QString& script) {
    DesktopIntegration::runKWinScript(script, QDBusConnection::sessionBus(), QStringLiteral("org.kde.KWin"));
}

// Loads `script` into KWin under `pluginName` and starts it. The temporary file is kept until KWin
// has answered `start`. With `unloadAfterRun` the script is unloaded
// right after it ran; otherwise it stays loaded (with its signal connections) until unloaded.
static void loadAndRunKWinScript(const QString& script, const QDBusConnection& connection, const QString& service,
                                 const QString& pluginName, bool unloadAfterRun) {
    QDBusConnection bus = connection;
    if (!bus.isConnected()) return;

    auto tempFile = std::make_shared<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/gofi_kwin_XXXXXX.js"));
    tempFile->setAutoRemove(true);
    if (!tempFile->open()) return;
    tempFile->write(script.toUtf8());
    tempFile->flush();
    tempFile->close();
    const QString tempPath = tempFile->fileName();

    QDBusMessage loadMsg = QDBusMessage::createMethodCall(
        service,
        QStringLiteral("/Scripting"),
        QStringLiteral("org.kde.kwin.Scripting"),
        QStringLiteral("loadScript")
    );
    loadMsg << tempPath << pluginName;

    auto* loadWatcher = new QDBusPendingCallWatcher(bus.asyncCall(loadMsg, 1500));
    QObject::connect(loadWatcher, &QDBusPendingCallWatcher::finished, [bus, service, pluginName, tempFile, unloadAfterRun](QDBusPendingCallWatcher* watcher) {
        watcher->deleteLater();
        QDBusPendingReply<int> reply = *watcher;
        if (!reply.isValid() || reply.value() < 0) {
            return;
        }
        // Not /Scripting/Script<id>.run(): KWin 6 numbers scripts by how many are loaded, so after
        // an unload the returned id (and its object path) can belong to an older script that is
        // still running, and run() would re-run that one instead (KB-149). start() runs every
        // loaded script that has not started yet.
        QDBusMessage startMsg = QDBusMessage::createMethodCall(
            service,
            QStringLiteral("/Scripting"),
            QStringLiteral("org.kde.kwin.Scripting"),
            QStringLiteral("start")
        );
        auto* startWatcher = new QDBusPendingCallWatcher(bus.asyncCall(startMsg, 1500));
        QObject::connect(startWatcher, &QDBusPendingCallWatcher::finished, [bus, service, pluginName, tempFile, unloadAfterRun](QDBusPendingCallWatcher* sw) {
            sw->deleteLater();
            if (unloadAfterRun) {
                DesktopIntegration::unloadKWinScript(bus, service, pluginName);
            }
        });
    });
}

void DesktopIntegration::runKWinScript(const QString& script, const QDBusConnection& connection, const QString& service) {
    static std::atomic<quint64> s_scriptSeq{0};
    const QString pluginName = QStringLiteral("gofi_pin_%1_%2_%3")
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(++s_scriptSeq);
    loadAndRunKWinScript(script, connection, service, pluginName, true);
}

void DesktopIntegration::startKWinScript(const QString& script, const QDBusConnection& connection, const QString& service,
                                         const QString& pluginName) {
    loadAndRunKWinScript(script, connection, service, pluginName, false);
}

void DesktopIntegration::unloadKWinScript(const QDBusConnection& connection, const QString& service, const QString& pluginName) {
    QDBusConnection bus = connection;
    if (!bus.isConnected() || pluginName.isEmpty()) return;
    QDBusMessage unloadMsg = QDBusMessage::createMethodCall(
        service,
        QStringLiteral("/Scripting"),
        QStringLiteral("org.kde.kwin.Scripting"),
        QStringLiteral("unloadScript")
    );
    unloadMsg << pluginName;
    bus.asyncCall(unloadMsg, 1500);
}

void DesktopIntegration::schedulePinScript(QWindow* win, bool alwaysOnTop, const std::function<void(const QString&)>& execute) {
    if (!win || !execute) {
        return;
    }
    static const char* const generationProperty = "_gofi_kwin_pin_generation";
    const quint64 generation = win->property(generationProperty).toULongLong() + 1;
    win->setProperty(generationProperty, generation);

    const QString script = kwinPinScript(win->title(), alwaysOnTop);

    // Execute immediately in case the window is already mapped
    execute(script);

    // When the window is first shown, the Wayland compositor maps the surface asynchronously
    // (typically takes 50-150ms). Retry after short delays so KWin catches the window. The window
    // is the timer context, so retries die with it; a newer call bumps the generation and the
    // retries of this one skip themselves.
    QPointer<QWindow> guard(win);
    for (int delayMs : {80, 200, 500, 1000}) {
        QTimer::singleShot(delayMs, win, [guard, generation, script, execute]() {
            if (!guard || guard->property(generationProperty).toULongLong() != generation) {
                return;
            }
            execute(script);
        });
    }
}

void DesktopIntegration::startMiniPlayerTracking(QWindow* win, std::optional<QPoint> position) {
    stopMiniPlayerTracking();
    if (!win || QStandardPaths::isTestModeEnabled()) {
        return;
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    if (!m_positionReceiverRegistered) {
        m_positionReceiverRegistered = bus.registerObject(QString::fromLatin1(MiniPlayerPositionReceiver::ObjectPath),
                                                          &m_positionReceiver,
                                                          QDBusConnection::ExportScriptableSlots);
        if (!m_positionReceiverRegistered) {
            qWarning() << "Mini player position: cannot register the D-Bus receiver";
            return;
        }
    }
    static std::atomic<quint64> s_trackerSeq{0};
    m_trackerPluginName = QStringLiteral("gofi_miniplayer_%1_%2")
        .arg(QCoreApplication::applicationPid())
        .arg(++s_trackerSeq);
    startKWinScript(kwinMiniPlayerTrackerScript(win->title(), position, bus.baseService()),
                    bus, QStringLiteral("org.kde.KWin"), m_trackerPluginName);
}

void DesktopIntegration::stopMiniPlayerTracking() {
    if (m_trackerPluginName.isEmpty()) {
        return;
    }
    unloadKWinScript(QDBusConnection::sessionBus(), QStringLiteral("org.kde.KWin"), m_trackerPluginName);
    m_trackerPluginName.clear();
}

void DesktopIntegration::setKWinWindowProperties(QWindow* win, bool alwaysOnTop) {
    if (!win || QStandardPaths::isTestModeEnabled()) {
        return;
    }
    schedulePinScript(win, alwaysOnTop, [](const QString& script) {
        executeKWinPinScript(script);
    });
}

} // namespace gofi
