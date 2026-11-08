#pragma once

#include "ActivityLog.h"
#include "AmbientSoundService.h"
#include "AnalyticsService.h"
#include "DesktopIntegration.h"
#include "IssueTrackerService.h"
#include "ListManagerModel.h"
#include "NotificationService.h"
#include "SettingsManager.h"
#include "ShortcutManager.h"
#include "SystemHookService.h"
#include "TaskFilterProxyModel.h"
#include "TaskManager.h"
#include "TaskTimer.h"
#include "UkTranslator.h"
#include <QObject>
#include <QTimer>

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
    Q_PROPERTY(gofi::AnalyticsService* analytics READ analytics CONSTANT)
    Q_PROPERTY(gofi::AmbientSoundService* ambient READ ambient CONSTANT)
    Q_PROPERTY(gofi::SystemHookService* systemHooks READ systemHooks CONSTANT)
    Q_PROPERTY(bool showingOverview READ showingOverview WRITE setShowingOverview NOTIFY showingOverviewChanged)
    Q_PROPERTY(QString currentListId READ currentListId NOTIFY currentListChanged)
    Q_PROPERTY(QString currentListName READ currentListName NOTIFY currentListChanged)
    Q_PROPERTY(QString currentListProvider READ currentListProvider NOTIFY currentListChanged)
    Q_PROPERTY(bool isMultiListView READ isMultiListView NOTIFY currentListChanged)
    Q_PROPERTY(bool isCurrentListRemote READ isCurrentListRemote NOTIFY currentListChanged)
    Q_PROPERTY(QString currentFilterTitle READ currentFilterTitle NOTIFY currentListChanged)
    Q_PROPERTY(QStringList activeListIds READ activeListIds NOTIFY currentListChanged)
    Q_PROPERTY(QString databasePath READ databasePath NOTIFY storagePathChanged)
    Q_PROPERTY(QString configFilePath READ configFilePath CONSTANT)
    Q_PROPERTY(int activeTodoRow READ activeTodoRow WRITE setActiveTodoRow NOTIFY activeTodoRowChanged)
    Q_PROPERTY(int unviewedTodoCount READ unviewedTodoCount NOTIFY todoCountsChanged)
    Q_PROPERTY(int totalTodoCount READ totalTodoCount NOTIFY totalTodoCountChanged)
    Q_PROPERTY(QVariantList availableLanguages READ availableLanguages NOTIFY languageChanged)
    Q_PROPERTY(bool isDatabaseReady READ isDatabaseReady CONSTANT)

public:
    explicit AppController(const QString& logFilePath = {},
                           const QString& customDbPath = {},
                           QObject* parent = nullptr);

    [[nodiscard]] bool isDatabaseReady() const noexcept { return m_databaseReady; }

    [[nodiscard]] SettingsManager* settings() noexcept { return &m_settings; }
    [[nodiscard]] ShortcutManager* shortcuts() noexcept { return &m_shortcuts; }
    [[nodiscard]] NotificationService* notifications() noexcept { return &m_notifications; }
    [[nodiscard]] TaskTimer* timer() noexcept { return &m_timer; }
    [[nodiscard]] ListManagerModel* listManager() noexcept { return &m_listManager; }
    [[nodiscard]] IssueTrackerService* issueTracker() noexcept { return &m_issueTracker; }
    [[nodiscard]] TaskFilterProxyModel* todoProxyModel() noexcept { return &m_todoProxy; }
    [[nodiscard]] TaskFilterProxyModel* doneProxyModel() noexcept { return &m_doneProxy; }
    [[nodiscard]] AnalyticsService* analytics() noexcept { return &m_analytics; }
    [[nodiscard]] AmbientSoundService* ambient() noexcept { return &m_ambient; }
    [[nodiscard]] SystemHookService* systemHooks() noexcept { return &m_systemHooks; }

    [[nodiscard]] bool showingOverview() const noexcept { return m_showingOverview; }
    void setShowingOverview(bool value);

    [[nodiscard]] QString currentListId() const { return m_currentListId; }
    [[nodiscard]] QString currentListName() const { return m_currentListName; }
    [[nodiscard]] QString currentListProvider() const { return m_currentListProvider; }
    [[nodiscard]] bool isMultiListView() const { return m_isMultiListView; }
    [[nodiscard]] bool isCurrentListRemote() const;
    [[nodiscard]] QString currentFilterTitle() const;
    [[nodiscard]] QStringList activeListIds() const { return m_activeFilterListIds; }
    [[nodiscard]] QString databasePath() const;
    [[nodiscard]] QString configFilePath() const;

    [[nodiscard]] int activeTodoRow() const noexcept { return m_activeTodoRow; }
    void setActiveTodoRow(int sourceRow);

    [[nodiscard]] int unviewedTodoCount() const;
    [[nodiscard]] int totalTodoCount() const;

    Q_INVOKABLE void selectListById(const QString& listId);
    Q_INVOKABLE void showAllLists();
    Q_INVOKABLE void showProviderLists(const QString& provider);
    Q_INVOKABLE void setMultiListFilter(const QStringList& listIds, const QString& customTitle = QString());
    Q_INVOKABLE void syncCurrentList();
    Q_INVOKABLE void syncAllRemoteLists();
    Q_INVOKABLE void addNewTask(const QString& simpleText);
    Q_INVOKABLE void toggleTaskDone(bool fromDoneList, int proxyRow);
    Q_INVOKABLE void changeTaskStatus(bool fromDoneList, int proxyRow, const QString& newStatus);
    Q_INVOKABLE void changeTaskPriority(bool fromDoneList, int proxyRow, const QString& priorityLetter);
    Q_INVOKABLE void updateTaskText(bool fromDoneList, int proxyRow, const QString& simpleText);
    Q_INVOKABLE void updateTaskDetails(bool fromDoneList, int proxyRow, const QString& details);
    Q_INVOKABLE void addTaskComment(bool fromDoneList, int proxyRow, const QString& commentText);
    Q_INVOKABLE void markTaskViewed(bool fromDoneList, int proxyRow);
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
    Q_INVOKABLE QString readMarkdownFile(const QString& filePath) const;
    Q_INVOKABLE QUrl getTaskBrowserUrl(const QString& listId, const QString& jiraKey, const QString& description) const;
    Q_INVOKABLE QUrl taskBrowserUrl(bool fromDoneList, int proxyRow) const;
    Q_INVOKABLE void openTaskInBrowser(bool fromDoneList, int proxyRow);
    Q_INVOKABLE void openUrlExternally(const QUrl& url);
    Q_INVOKABLE void checkDueReminders();
    Q_INVOKABLE void reloadTranslations();
    [[nodiscard]] QVariantList availableLanguages() const;
    [[nodiscard]] QStringList availableLanguageCodes() const;
    [[nodiscard]] QString languageDisplayName(const QString& code) const;

signals:
    void showingOverviewChanged();
    void currentListChanged();
    void storagePathChanged();
    void languageChanged();
    void activeTodoRowChanged();
    void todoCountsChanged();
    void totalTodoCountChanged();
    void notificationRequested(const QString& title, const QString& body);
    void showWindowRequested();
    void quitRequested();

private slots:
    void onTimerStopped(const QDateTime& startTimeUtc, uint runtimeSeconds);
    void onActiveTaskTimeUpdated(uint totalSeconds);
    void onTodoListCountChanged();

private:
    void syncActiveTaskToTimer();
    int findActiveTodoRowById(qint64 taskId) const;
    void applyLanguageTranslator();
    void updateAutoSyncTimer();

    SettingsManager m_settings;
    UkTranslator m_ukTranslator;
    ShortcutManager m_shortcuts;
    TaskTimer m_timer;
    NotificationService m_notifications;
    bool m_databaseReady = false;
    ListManagerModel m_listManager;
    IssueTrackerService m_issueTracker;
    TaskManager m_taskManager;
    TaskFilterProxyModel m_todoProxy;
    TaskFilterProxyModel m_doneProxy;
    ActivityLog m_activityLog;
    AnalyticsService m_analytics;
    AmbientSoundService m_ambient;
    SystemHookService m_systemHooks;
    DesktopIntegration m_desktopIntegration;
    QTimer m_autoSyncTimer;
    QTimer m_dueReminderTimer;
    QSet<qint64> m_notifiedDueTaskIds;

    QString m_currentListId;
    QString m_currentListName;
    QString m_currentListProvider;
    QString m_currentListActivityLogPath;
    QStringList m_activeFilterListIds;
    QString m_currentFilterTitle;
    int m_activeTodoRow = -1;
    qint64 m_activeTaskId = -1;
    QString m_activeTaskJiraKey;
    QString m_activeTaskListId;
    bool m_showingOverview = false;
    bool m_isMultiListView = false;
    bool m_lastSessionCompletedPomodoro = false;
};

} // namespace gofi
