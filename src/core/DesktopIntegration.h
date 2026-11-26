#pragma once

#include "SettingsManager.h"
#include "TaskTimer.h"
#include <QAction>
#include <QHash>
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

// D-Bus endpoint that the KWin window tracker scripts call (via callDBus) after the user moves one
// of our windows, since a Wayland client cannot read its own global position. `role` names the
// window (DesktopIntegration::MainWindowRole / MiniPlayerRole).
class WindowPositionReceiver : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.bladeroot.GoForItQt.WindowPosition")

public:
    static constexpr const char* ObjectPath = "/io/github/bladeroot/GoForItQt/WindowPosition";
    static constexpr const char* Interface = "io.github.bladeroot.GoForItQt.WindowPosition";
    using QObject::QObject;

public slots:
    Q_SCRIPTABLE void reportPosition(const QString& role, int x, int y) { emit positionReported(role, QPoint(x, y)); }

signals:
    void positionReported(const QString& role, const QPoint& pos);
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
    static constexpr const char* MainWindowRole = "main";
    static constexpr const char* MiniPlayerRole = "mini_player";
    // KWin script that moves this process's window titled `windowTitle` to `position` (when given)
    // as soon as it is mapped, and reports every finished interactive move of it, tagged with
    // `role`, to `replyService` through WindowPositionReceiver. It stays loaded while the window is shown.
    [[nodiscard]] static QString kwinWindowTrackerScript(const QString& role,
                                                         const QString& windowTitle,
                                                         std::optional<QPoint> position,
                                                         const QString& replyService);
    // `saved` if a `size` window placed there would have its centre on one of `screenAreas`;
    // otherwise empty, so the window system places it (e.g. after a monitor was unplugged).
    [[nodiscard]] static std::optional<QPoint> restorableWindowPosition(std::optional<QPoint> saved,
                                                                        const QSize& size,
                                                                        const QList<QRect>& screenAreas);
    // Wayland + KWin: place `win` at `position` and keep reporting its moves through
    // windowPositionReported(role, …) until stopWindowTracking(role). No-op in test mode.
    void startWindowTracking(const QString& role, QWindow* win, std::optional<QPoint> position);
    void stopWindowTracking(const QString& role);
    static void schedulePinScript(QWindow* win, bool alwaysOnTop, const std::function<void(const QString&)>& execute);
    [[nodiscard]] QMenu* trayMenu() const noexcept { return m_trayMenu; }
    [[nodiscard]] QAction* toggleMiniPlayerAction() const noexcept { return m_toggleMiniPlayerAction; }

signals:
    void showWindowRequested();
    void toggleMiniPlayerRequested();
    void quitRequested();
    void windowPositionReported(const QString& role, const QPoint& pos);

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

    WindowPositionReceiver m_positionReceiver;
    bool m_positionReceiverRegistered = false;
    QHash<QString, QString> m_trackerPlugins;  // role -> loaded KWin script name
    QHash<QString, QString> m_trackerTitles;   // role -> window title the loaded script matches

    qint64 m_lastLauncherBadgeCount = -1;
    bool m_lastLauncherBadgeVisible = false;
};

} // namespace gofi
