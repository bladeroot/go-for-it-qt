#pragma once

#include "SettingsManager.h"
#include "TaskTimer.h"
#include <QAction>
#include <QMenu>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSystemTrayIcon>
#include <functional>
#include <optional>

class QDBusConnection;

namespace gofi {

class AppController;

// D-Bus endpoint that the KWin mini player tracker script calls (via callDBus) after the user
// moves the mini player, since a Wayland client cannot read its own global position.
class MiniPlayerPositionReceiver : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.bladeroot.GoForItQt.MiniPlayer")

public:
    static constexpr const char* ObjectPath = "/io/github/bladeroot/GoForItQt/MiniPlayer";
    using QObject::QObject;

public slots:
    Q_SCRIPTABLE void reportPosition(int x, int y) { emit positionReported(QPoint(x, y)); }

signals:
    void positionReported(const QPoint& pos);
};

class DesktopIntegration : public QObject {
    Q_OBJECT

public:
    explicit DesktopIntegration(AppController* controller,
                                SettingsManager* settings,
                                TaskTimer* timer,
                                QObject* parent = nullptr);
    ~DesktopIntegration() override;

    void showTrayMessage(const QString& title, const QString& body);
    void setMiniPlayerVisible(bool visible);
    [[nodiscard]] bool kwinIntegrationAvailable() const;
    void setKWinWindowProperties(QWindow* win, bool alwaysOnTop);
    // KWin script that applies the mini player keep-above state; `windowTitle` is the raw title.
    // Loads, runs and unloads `script` through the KWin Scripting D-Bus API of `service` on `connection`.
    static void runKWinScript(const QString& script, const QDBusConnection& connection, const QString& service);
    // Like runKWinScript, but leaves the script loaded under `pluginName` until unloadKWinScript().
    static void startKWinScript(const QString& script, const QDBusConnection& connection, const QString& service,
                                const QString& pluginName);
    static void unloadKWinScript(const QDBusConnection& connection, const QString& service, const QString& pluginName);
    [[nodiscard]] static QString kwinPinScript(const QString& windowTitle, bool alwaysOnTop);
    // Calls `execute` with the pin script now and again on the retry schedule (the compositor maps a
    // new window asynchronously). A later call for the same window cancels the pending retries of
    // earlier calls, so a stale pin/unpin never runs after a newer state.
    // KWin script that moves this process's mini player to `position` (when given) as soon as it
    // is mapped, and reports every finished interactive move of it to `replyService` through
    // MiniPlayerPositionReceiver. It stays loaded while the mini player is visible.
    [[nodiscard]] static QString kwinMiniPlayerTrackerScript(const QString& windowTitle,
                                                             std::optional<QPoint> position,
                                                             const QString& replyService);
    // `saved` if a `size` window placed there would have its centre on one of `screenAreas`;
    // otherwise empty, so the window system places it (e.g. after a monitor was unplugged).
    [[nodiscard]] static std::optional<QPoint> restorableMiniPlayerPosition(std::optional<QPoint> saved,
                                                                            const QSize& size,
                                                                            const QList<QRect>& screenAreas);
    // Wayland + KWin: place the mini player at `position` and keep reporting its moves through
    // miniPlayerPositionReported until stopMiniPlayerTracking(). No-op in test mode.
    void startMiniPlayerTracking(QWindow* win, std::optional<QPoint> position);
    void stopMiniPlayerTracking();
    static void schedulePinScript(QWindow* win, bool alwaysOnTop, const std::function<void(const QString&)>& execute);
    [[nodiscard]] QMenu* trayMenu() const noexcept { return m_trayMenu; }
    [[nodiscard]] QAction* toggleMiniPlayerAction() const noexcept { return m_toggleMiniPlayerAction; }

signals:
    void showWindowRequested();
    void toggleMiniPlayerRequested();
    void quitRequested();
    void miniPlayerPositionReported(const QPoint& pos);

private slots:
    void updatePluginStates();
    void updateTrayState();
    void updateLauncherBadge();

private:
    void setupTrayIcon();
    [[nodiscard]] QIcon renderBadgeIcon(int unviewed, int total) const;
    void emitUnityLauncherUpdate(const QVariantMap& properties);

    AppController* m_controller = nullptr;
    SettingsManager* m_settings = nullptr;
    TaskTimer* m_timer = nullptr;

    QSystemTrayIcon* m_trayIcon = nullptr;
    QIcon m_baseIcon;
    QMenu* m_trayMenu = nullptr;
    QAction* m_toggleMiniPlayerAction = nullptr;
    QAction* m_taskDescAction = nullptr;
    QAction* m_markDoneAction = nullptr;
    QAction* m_nextTaskAction = nullptr;
    QAction* m_prevTaskAction = nullptr;
    QAction* m_toggleTimerAction = nullptr;

    MiniPlayerPositionReceiver m_positionReceiver;
    bool m_positionReceiverRegistered = false;
    QString m_trackerPluginName;

    qint64 m_lastLauncherBadgeCount = -1;
    bool m_lastLauncherBadgeVisible = false;
};

} // namespace gofi
