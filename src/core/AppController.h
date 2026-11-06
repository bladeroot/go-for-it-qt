#pragma once

#include "ActivityLog.h"
#include "DesktopIntegration.h"
#include "IssueTrackerService.h"
#include "ListManagerModel.h"
#include "NotificationService.h"
#include "SettingsManager.h"
#include "ShortcutManager.h"
#include "TaskFilterProxyModel.h"
#include "TaskManager.h"
#include "TaskTimer.h"
#include <QObject>

namespace gofi {

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(gofi::SettingsManager* settings READ settings CONSTANT)
    Q_PROPERTY(gofi::ShortcutManager* shortcuts READ shortcuts CONSTANT)
    Q_PROPERTY(gofi::NotificationService* notifications READ notifications CONSTANT)
    Q_PROPERTY(gofi::TaskTimer* timer READ timer CONSTANT)
    Q_PROPERTY(gofi::ListManagerModel* listManager READ listManager CONSTANT)
    Q_PROPERTY(gofi::IssueTrackerService* issueTracker READ issueTracker CONSTANT)
    Q_PROPERTY(gofi::TaskFilterProxyModel* todoProxyModel READ todoProxyModel CONSTANT)
    Q_PROPERTY(gofi::TaskFilterProxyModel* doneProxyModel READ doneProxyModel CONSTANT)
    Q_PROPERTY(bool showingOverview READ showingOverview WRITE setShowingOverview NOTIFY showingOverviewChanged)
    Q_PROPERTY(QString currentListId READ currentListId NOTIFY currentListChanged)
    Q_PROPERTY(QString currentListName READ currentListName NOTIFY currentListChanged)
    Q_PROPERTY(QString currentListProvider READ currentListProvider NOTIFY currentListChanged)
    Q_PROPERTY(QString databasePath READ databasePath NOTIFY storagePathChanged)
    Q_PROPERTY(QString configFilePath READ configFilePath CONSTANT)
    Q_PROPERTY(int activeTodoRow READ activeTodoRow WRITE setActiveTodoRow NOTIFY activeTodoRowChanged)

public:
    explicit AppController(const QString& logFilePath = {},
                           const QString& customDbPath = {},
                           QObject* parent = nullptr);

    [[nodiscard]] SettingsManager* settings() noexcept { return &m_settings; }
    [[nodiscard]] ShortcutManager* shortcuts() noexcept { return &m_shortcuts; }
    [[nodiscard]] NotificationService* notifications() noexcept { return &m_notifications; }
    [[nodiscard]] TaskTimer* timer() noexcept { return &m_timer; }
    [[nodiscard]] ListManagerModel* listManager() noexcept { return &m_listManager; }
    [[nodiscard]] IssueTrackerService* issueTracker() noexcept { return &m_issueTracker; }
    [[nodiscard]] TaskFilterProxyModel* todoProxyModel() noexcept { return &m_todoProxy; }
    [[nodiscard]] TaskFilterProxyModel* doneProxyModel() noexcept { return &m_doneProxy; }

    [[nodiscard]] bool showingOverview() const noexcept { return m_showingOverview; }
    void setShowingOverview(bool value);

    [[nodiscard]] QString currentListId() const { return m_currentListId; }
    [[nodiscard]] QString currentListName() const { return m_currentListName; }
    [[nodiscard]] QString currentListProvider() const { return m_currentListProvider; }
    [[nodiscard]] QString databasePath() const;
    [[nodiscard]] QString configFilePath() const;

    [[nodiscard]] int activeTodoRow() const noexcept { return m_activeTodoRow; }
    void setActiveTodoRow(int sourceRow);

    Q_INVOKABLE void selectListById(const QString& listId);
    Q_INVOKABLE void syncCurrentList();
    Q_INVOKABLE void addNewTask(const QString& simpleText);
    Q_INVOKABLE void toggleTaskDone(bool fromDoneList, int proxyRow);
    Q_INVOKABLE void changeTaskStatus(bool fromDoneList, int proxyRow, const QString& newStatus);
    Q_INVOKABLE void updateTaskText(bool fromDoneList, int proxyRow, const QString& simpleText);
    Q_INVOKABLE void updateTaskDetails(bool fromDoneList, int proxyRow, const QString& details);
    Q_INVOKABLE void removeTask(bool fromDoneList, int proxyRow);
    Q_INVOKABLE void moveTaskRow(bool fromDoneList, int fromProxyRow, int toProxyRow);
    Q_INVOKABLE void moveActiveTaskByOffset(int delta);
    Q_INVOKABLE void switchToNextTask();
    Q_INVOKABLE void switchToPreviousTask();
    Q_INVOKABLE void markActiveTaskDone();
    Q_INVOKABLE void clearDoneTasks();
    Q_INVOKABLE void sortAllTasks();
    Q_INVOKABLE bool changeDatabasePath(const QString& newDbPath, bool copyExisting = true);
    Q_INVOKABLE bool exportBackup(const QString& targetFilePath);
    Q_INVOKABLE bool importBackup(const QString& sourceFilePath);

signals:
    void showingOverviewChanged();
    void currentListChanged();
    void storagePathChanged();
    void activeTodoRowChanged();
    void notificationRequested(const QString& title, const QString& body);
    void showWindowRequested();
    void quitRequested();

private slots:
    void onTimerStopped(const QDateTime& startTimeUtc, uint runtimeSeconds);
    void onActiveTaskTimeUpdated(uint totalSeconds);
    void onTodoListCountChanged();

private:
    void syncActiveTaskToTimer();

    SettingsManager m_settings;
    ShortcutManager m_shortcuts;
    TaskTimer m_timer;
    NotificationService m_notifications;
    ListManagerModel m_listManager;
    IssueTrackerService m_issueTracker;
    TaskManager m_taskManager;
    TaskFilterProxyModel m_todoProxy;
    TaskFilterProxyModel m_doneProxy;
    ActivityLog m_activityLog;
    DesktopIntegration m_desktopIntegration;

    QString m_currentListId;
    QString m_currentListName;
    QString m_currentListProvider;
    QString m_currentListActivityLogPath;
    int m_activeTodoRow = -1;
    bool m_showingOverview = false;
};

} // namespace gofi
