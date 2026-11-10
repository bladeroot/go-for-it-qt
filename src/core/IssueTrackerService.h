#pragma once

#include "DatabaseManager.h"
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QJsonObject>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <functional>

class TestTodoTxt;

namespace gofi {

struct RemoteIssueItem {
    QString key;
    QString description;
    QString details;
    QVariantList comments;
    QString remoteStatus;
    QString remoteStatusCategory;
    QString remotePriority;
    quint8 priority = TxtTask::NoPriority;
    quint32 durationSeconds = 0;
};

class IssueTrackerService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool syncing READ isSyncing NOTIFY syncingChanged)
    Q_PROPERTY(QString lastStatusMessage READ lastStatusMessage NOTIFY statusMessageChanged)

public:
    explicit IssueTrackerService(QObject* parent = nullptr);

    [[nodiscard]] bool isSyncing() const noexcept { return m_syncing; }
    [[nodiscard]] QString lastStatusMessage() const { return m_lastStatusMessage; }
    void setStatusMessage(const QString& msg);

    void syncListIssues(const TodoListEntry& listEntry);
    void postWorklog(const TodoListEntry& listEntry,
                     qint64 taskId,
                     qint64 activityLogId,
                     const QString& issueKey,
                     const QDateTime& startTimeUtc,
                     quint32 runtimeSeconds,
                     bool forceBearerAuth = false);
    void updateIssueDescription(const TodoListEntry& listEntry,
                                const QString& issueKey,
                                const QString& markdownDetails,
                                bool forceBearerAuth = false,
                                bool useV2Fallback = false);
    void updateIssueStatusRemote(const TodoListEntry& listEntry,
                                 const QString& issueKey,
                                 bool done,
                                 const QString& localStatus);
    void postIssueComment(const TodoListEntry& listEntry,
                          const QString& issueKey,
                          const QString& markdownComment,
                          bool forceBearerAuth = false,
                          bool useV2Fallback = false);
    static QJsonObject markdownToJiraAdf(const QString& markdown);
    static QUrl buildIssueBrowserUrl(const TodoListEntry& listEntry,
                                     const QString& issueKey,
                                     const QString& taskDescription = QString());

    void setActiveTaskChecker(std::function<bool(const QString& listId, const QString& jiraKey)> checker) {
        m_activeTaskChecker = std::move(checker);
    }

    // Fetch available remote statuses for mapping UI
    Q_INVOKABLE void fetchRemoteStatuses(const QString& provider,
                                         const QString& serverUrl,
                                         const QString& username,
                                         const QString& apiToken,
                                         const QVariantMap& existingMapping = {});

    // Fetch / default remote priorities for mapping UI
    Q_INVOKABLE QVariantList defaultPrioritiesForProvider(const QString& provider,
                                                          const QVariantMap& existingMapping = {}) const;
    Q_INVOKABLE void fetchRemotePriorities(const QString& provider,
                                           const QString& serverUrl,
                                           const QString& username,
                                           const QString& apiToken,
                                           const QVariantMap& existingMapping = {});

    // Pure JSON parsers & status mapper (also used by unit tests)
    static QVector<RemoteIssueItem> parseJiraIssuesJson(const QByteArray& jsonBytes);
    static QVector<RemoteIssueItem> parseYouTrackIssuesJson(const QByteArray& jsonBytes);
    static QVector<RemoteIssueItem> parseGitHubIssuesJson(const QByteArray& jsonBytes);
    static QVector<RemoteIssueItem> parseGitLabIssuesJson(const QByteArray& jsonBytes);
    static QVector<RemoteIssueItem> parseGitLabMergeRequestsJson(const QByteArray& jsonBytes);
    static QVector<RemoteIssueItem> parseRedmineIssuesJson(const QByteArray& jsonBytes);
    static QVector<RemoteIssueItem> parseTodoistTasksJson(const QByteArray& jsonBytes);
    static QVector<RemoteIssueItem> parseNextcloudTasksIcs(const QByteArray& icsOrXmlBytes);
    static QVariantList parseJiraStatusesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping = {});
    static QVariantList parseYouTrackStatusesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping = {});
    static QVariantList parseGitHubStatuses(const QVariantMap& existingMapping = {});
    static QVariantList parseGitLabStatuses(const QVariantMap& existingMapping = {});
    static QVariantList parseRedmineStatusesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping = {});
    static QVariantList parseTodoistStatuses(const QVariantMap& existingMapping = {});
    static QVariantList parseNextcloudStatuses(const QVariantMap& existingMapping = {});
    static QString mapRemoteStatusToLocal(const QString& remoteStatus,
                                          const QString& categoryHint = {},
                                          const QVariantMap& customMapping = {});
    static quint8 mapPriorityName(const QString& priorityName);
    static quint8 mapRemotePriorityToLocal(const QString& remotePriority,
                                           const QVariantMap& customMapping = {});
    static QVariantList parseJiraPrioritiesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping = {});
    static QVariantList parseRedminePrioritiesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping = {});

signals:
    void syncingChanged();
    void statusMessageChanged();
    void listIssuesSynced(const QString& listId, int count);
    void trackerChangesDetected(const QString& listName, const QStringList& newIssues, const QStringList& statusChanges);
    void syncFailed(const QString& listId, const QString& error);
    void worklogPosted(const QString& issueKey, quint32 loggedSeconds);
    void remoteStatusesLoaded(const QVariantList& statuses);
    void remoteStatusesError(const QString& error);
    void remotePrioritiesLoaded(const QVariantList& priorities);
    void remotePrioritiesError(const QString& error);
    void listStatusMappingUpdated(const QString& listId, const QVariantMap& mapping);

private:
    static QString normalizeBaseUrl(const QString& rawUrl);
    static QString gitHubApiBaseUrl(const QString& rawBaseUrl);
    void applyAuthHeader(QNetworkRequest& req, const TodoListEntry& entry, bool forceBearerAuth = false);
    void setSyncing(bool value);

    void fetchJiraIssues(const TodoListEntry& entry, int endpointStage = 0, bool forceBearerAuth = false);
    void fetchYouTrackIssues(const TodoListEntry& entry);
    void fetchGitHubIssues(const TodoListEntry& entry);
    void fetchGitLabIssues(const TodoListEntry& entry);
    void fetchRedmineIssues(const TodoListEntry& entry);
    void fetchTodoistTasks(const TodoListEntry& entry);
    void fetchNextcloudTasks(const TodoListEntry& entry);
    void processSyncedItems(const TodoListEntry& entry, const QVector<RemoteIssueItem>& items, const QString& providerName, bool pageLimitReached = false, bool hasError = false, bool explicitZeroTotal = false);

    struct JiraSyncState;
    struct YouTrackSyncState;
    struct GitHubSyncState;
    struct GitLabCombinedSyncState;
    struct RedmineSyncState;

    void fetchJiraPage(const TodoListEntry& entry, const std::shared_ptr<JiraSyncState>& state, const QString& baseUrl, const QString& path, const QString& jql, int endpointStage, bool forceBearerAuth);
    void fetchYouTrackPage(const TodoListEntry& entry, const std::shared_ptr<YouTrackSyncState>& state, const QString& baseUrl, const QString& ytQuery);
    void fetchGitHubPage(const TodoListEntry& entry, const std::shared_ptr<GitHubSyncState>& state, const QString& apiUrl, const QString& queryStr);
    void fetchGitLabStreamPage(const TodoListEntry& entry, const std::shared_ptr<GitLabCombinedSyncState>& combinedState, const QString& baseUrl, const QString& path, bool isMr, int page);
    void fetchRedminePage(const TodoListEntry& entry, const std::shared_ptr<RedmineSyncState>& state, const QString& baseUrl, const QString& queryStr);

    void startSyncOperation();
    void endSyncOperation();

    QNetworkAccessManager m_nam;
    bool m_syncing = false;
    int m_activeSyncRequests = 0;
    QString m_lastStatusMessage;
    QHash<QString, QString> m_jiraBearerTokens;
    std::function<bool(const QString& listId, const QString& jiraKey)> m_activeTaskChecker;

    friend class ::TestTodoTxt;
};

} // namespace gofi
