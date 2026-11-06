#pragma once

#include "SettingsManager.h"
#include "TaskTimer.h"
#include <QAction>
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

namespace gofi {

class AppController;

class DesktopIntegration : public QObject {
    Q_OBJECT

public:
    explicit DesktopIntegration(AppController* controller,
                                SettingsManager* settings,
                                TaskTimer* timer,
                                QObject* parent = nullptr);

    void showTrayMessage(const QString& title, const QString& body);

signals:
    void showWindowRequested();
    void quitRequested();

private slots:
    void updatePluginStates();
    void updateTrayState();
    void updateLauncherBadge();

private:
    void setupTrayIcon();
    void emitUnityLauncherUpdate(const QVariantMap& properties);

    AppController* m_controller = nullptr;
    SettingsManager* m_settings = nullptr;
    TaskTimer* m_timer = nullptr;

    QSystemTrayIcon* m_trayIcon = nullptr;
    QMenu* m_trayMenu = nullptr;
    QAction* m_taskDescAction = nullptr;
    QAction* m_markDoneAction = nullptr;
    QAction* m_nextTaskAction = nullptr;
    QAction* m_prevTaskAction = nullptr;
    QAction* m_toggleTimerAction = nullptr;

    qint64 m_lastLauncherBadgeCount = -1;
    bool m_lastLauncherBadgeVisible = false;
};

} // namespace gofi
