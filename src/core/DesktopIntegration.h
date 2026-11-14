#pragma once

#include "SettingsManager.h"
#include "TaskTimer.h"
#include <QAction>
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

class QDBusConnection;

namespace gofi {

class AppController;

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
    [[nodiscard]] static QString kwinPinScript(const QString& windowTitle, bool alwaysOnTop);
    [[nodiscard]] QMenu* trayMenu() const noexcept { return m_trayMenu; }
    [[nodiscard]] QAction* toggleMiniPlayerAction() const noexcept { return m_toggleMiniPlayerAction; }

signals:
    void showWindowRequested();
    void toggleMiniPlayerRequested();
    void quitRequested();

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

    qint64 m_lastLauncherBadgeCount = -1;
    bool m_lastLauncherBadgeVisible = false;
};

} // namespace gofi
