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
#include "JsonTranslator.h"
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
    Q_PROPERTY(QString activeListFilter READ activeListFilter NOTIFY currentListChanged)
    Q_PROPERTY(QString currentListFilter READ activeListFilter NOTIFY currentListChanged)
    Q_PROPERTY(QString databasePath READ databasePath NOTIFY storagePathChanged)
    Q_PROPERTY(QString configFilePath READ configFilePath CONSTANT)
    Q_PROPERTY(int activeTodoRow READ activeTodoRow WRITE setActiveTodoRow NOTIFY activeTodoRowChanged)
    // False on Wayland desktops other than KDE Plasma (GNOME, Sway…): they ignore requests to keep a
    // window above others, so the mini player hides its pin button there.
    Q_PROPERTY(bool windowPinningSupported READ windowPinningSupported CONSTANT)
    Q_PROPERTY(bool hasNextTask READ hasNextTask NOTIFY taskNavigationChanged)
    Q_PROPERTY(bool hasPreviousTask READ hasPreviousTask NOTIFY taskNavigationChanged)
    Q_PROPERTY(int unviewedTodoCount READ unviewedTodoCount NOTIFY todoCountsChanged)
    Q_PROPERTY(int totalTodoCount READ totalTodoCount NOTIFY totalTodoCountChanged)
    Q_PROPERTY(QVariantList availableLanguages READ availableLanguages NOTIFY languageChanged)
    Q_PROPERTY(bool isDatabaseReady READ isDatabaseReady CONSTANT)

public:
    explicit AppController(const QString& logFilePath = {},
                           const QString& customDbPath = {},
                           QObject* parent = nullptr);
    ~AppController() override;

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
    [[nodiscard]] DesktopIntegration* desktopIntegration() noexcept { return &m_desktopIntegration; }

    [[nodiscard]] bool showingOverview() const noexcept { return m_showingOverview; }
    void setShowingOverview(bool value);

    [[nodiscard]] QString currentListId() const { return m_currentListId; }
    [[nodiscard]] QString currentListName() const { return m_currentListName; }
    [[nodiscard]] QString currentListProvider() const { return m_currentListProvider; }
    [[nodiscard]] bool isMultiListView() const { return m_isMultiListView; }
    [[nodiscard]] bool isCurrentListRemote() const;
    [[nodiscard]] QString currentFilterTitle() const;
    [[nodiscard]] QStringList activeListIds() const { return m_activeFilterListIds; }
    [[nodiscard]] QString activeListFilter() const;
    [[nodiscard]] QString databasePath() const;
    [[nodiscard]] QString configFilePath() const;

    [[nodiscard]] int activeTodoRow() const noexcept { return m_activeTodoRow; }
    // Whether switchToNextTask/switchToPreviousTask would move within the visible (filtered) to-do list.
    [[nodiscard]] bool hasNextTask() const { return adjacentVisibleTodoRow(1) >= 0; }
    [[nodiscard]] bool hasPreviousTask() const { return adjacentVisibleTodoRow(-1) >= 0; }
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
    Q_INVOKABLE bool toggleTaskDone(bool fromDoneList, int proxyRow);
    Q_INVOKABLE bool changeTaskStatus(bool fromDoneList, int proxyRow, const QString& newStatus);
    Q_INVOKABLE bool changeTaskPriority(bool fromDoneList, int proxyRow, const QString& priorityLetter);
    Q_INVOKABLE bool updateTaskText(bool fromDoneList, int proxyRow, const QString& simpleText);
    // Returns true if local SQLite persistence succeeded. Remote issue tracker sync is dispatched asynchronously.
    Q_INVOKABLE bool updateTaskDetails(bool fromDoneList, int proxyRow, const QString& details);
    Q_INVOKABLE qint64 getTaskIdAt(bool fromDoneList, int proxyRow) const;
    Q_INVOKABLE int findProxyRowByTaskId(bool fromDoneList, qint64 taskId) const;
    Q_INVOKABLE QVariantMap getTaskDetailsAt(bool fromDoneList, int proxyRow) const;
    Q_INVOKABLE QVariantMap getActiveTaskDetails();
    Q_INVOKABLE bool updateTaskDetailsById(qint64 taskId, const QString& details);
    Q_INVOKABLE bool addTaskCommentById(qint64 taskId, const QString& commentText);
    Q_INVOKABLE bool changeTaskPriorityById(qint64 taskId, const QString& priorityLetter);
    // Returns true if local SQLite persistence succeeded. Remote issue tracker sync is dispatched asynchronously.
    Q_INVOKABLE bool addTaskComment(bool fromDoneList, int proxyRow, const QString& commentText);
    Q_INVOKABLE bool markTaskViewed(bool fromDoneList, int proxyRow);
    Q_INVOKABLE bool removeTask(bool fromDoneList, int proxyRow);
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
    // Names of tracker lists that have no API token, e.g. after importing a backup made on another
    // machine (backups never contain tokens; they live in the keyring, scoped to the database).
    Q_INVOKABLE QStringList remoteListsWithoutToken() const;
    Q_INVOKABLE QString readMarkdownFile(const QString& filePath) const;
    Q_INVOKABLE QUrl getTaskBrowserUrl(const QString& listId, const QString& jiraKey, const QString& description) const;
    Q_INVOKABLE QUrl taskBrowserUrl(bool fromDoneList, int proxyRow) const;
    Q_INVOKABLE void openTaskInBrowser(bool fromDoneList, int proxyRow);
    Q_INVOKABLE void openUrlExternally(const QUrl& url);
    Q_INVOKABLE void setMiniPlayerVisible(bool visible);
    Q_INVOKABLE void configureMiniPlayerWindow(QObject* windowObject, bool alwaysOnTop);
    // Mini player position: restored on show and saved after moves. On Wayland a client can neither
    // read nor set its global position, so there it goes through a KWin script (when KWin runs).
    Q_INVOKABLE void miniPlayerShown(QObject* windowObject);
    // Main window: on Wayland + KWin its position is restored and saved through a KWin script;
    // elsewhere Main.qml sets x/y itself. persistMainWindowGeometry stores the size, and the
    // position only where Qt knows it (not on Wayland, where it reads 0,0).
    Q_INVOKABLE void mainWindowShown(QObject* windowObject);
    Q_INVOKABLE void mainWindowHidden();
    Q_INVOKABLE void persistMainWindowGeometry(QObject* windowObject);
    // False on Wayland, where a client can neither read nor set its global window position.
    [[nodiscard]] static bool platformReportsWindowPosition(const QString& platformName);
    [[nodiscard]] static bool windowPinningSupportedOn(const QString& platformName, bool kwinAvailable);
    [[nodiscard]] bool windowPinningSupported() const;
    Q_INVOKABLE void miniPlayerHidden();
    Q_INVOKABLE void miniPlayerMoved(QObject* windowObject);
    Q_INVOKABLE void checkDueReminders();
    // checkDueReminders() as of `today`: each (task, due date, overdue/due-today) is reminded once per day.
    void checkDueRemindersOn(const QDate& today);
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
    void taskNavigationChanged();
    void todoCountsChanged();
    void totalTodoCountChanged();
    void notificationRequested(const QString& title, const QString& body);
    void databaseError(const QString& message);
    void showWindowRequested();
    void toggleMiniPlayerRequested();
    void quitRequested();

private slots:
    void onTimerStopped(const QDateTime& startTimeUtc, uint runtimeSeconds);
    void onActiveTaskTimeUpdated(uint totalSeconds);
    void onTodoListCountChanged();

private:
    // Source row of the visible to-do task next to the active one (`step` +1 / -1), or -1.
    [[nodiscard]] int adjacentVisibleTodoRow(int step) const;
    void syncActiveTaskToTimer();
    int findActiveTodoRowById(qint64 taskId) const;
    void applyLanguageTranslator();
    void updateAutoSyncTimer();
    bool toggleTaskDoneSource(bool fromDoneList, int sourceRow);
    bool changeTaskStatusSource(bool fromDoneList, int sourceRow, const QString& newStatus);

    SettingsManager m_settings;
    JsonTranslator m_translator;
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
    QSet<QString> m_notifiedDueReminders;  // "<task id>|<due date>|<kind>" reminded on m_dueRemindersDay
    QDate m_dueRemindersDay;

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
