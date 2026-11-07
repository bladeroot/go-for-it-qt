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

namespace gofi {

struct RemoteIssueItem {
    QString key;
    QString description;
    QString details;
    QVariantList comments;
    QString remoteStatus;
    QString remoteStatusCategory;
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

    void syncListIssues(const TodoListEntry& listEntry);
    void postWorklog(const TodoListEntry& listEntry,
                     qint64 taskId,
                     qint64 activityLogId,
                     const QString& issueKey,
                     const QDateTime& startTimeUtc,
                     quint32 runtimeSeconds);
    void updateIssueDescription(const TodoListEntry& listEntry,
                                const QString& issueKey,
                                const QString& markdownDetails,
                                bool forceBearerAuth = false,
                                bool useV2Fallback = false);
    void postIssueComment(const TodoListEntry& listEntry,
                          const QString& issueKey,
                          const QString& markdownComment,
                          bool forceBearerAuth = false,
                          bool useV2Fallback = false);
    static QJsonObject markdownToJiraAdf(const QString& markdown);

    // Fetch available remote statuses for mapping UI
    Q_INVOKABLE void fetchRemoteStatuses(const QString& provider,
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
    static QVariantList parseJiraStatusesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping = {});
    static QVariantList parseYouTrackStatusesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping = {});
    static QVariantList parseGitHubStatuses(const QVariantMap& existingMapping = {});
    static QVariantList parseGitLabStatuses(const QVariantMap& existingMapping = {});
    static QVariantList parseRedmineStatusesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping = {});
    static QVariantList parseTodoistStatuses(const QVariantMap& existingMapping = {});
    static QString mapRemoteStatusToLocal(const QString& remoteStatus,
                                          const QString& categoryHint = {},
                                          const QVariantMap& customMapping = {});
    static quint8 mapPriorityName(const QString& priorityName);

signals:
    void syncingChanged();
    void statusMessageChanged();
    void listIssuesSynced(const QString& listId, int count);
    void trackerChangesDetected(const QString& listName, const QStringList& newIssues, const QStringList& statusChanges);
    void syncFailed(const QString& listId, const QString& error);
    void worklogPosted(const QString& issueKey, quint32 loggedSeconds);
    void remoteStatusesLoaded(const QVariantList& statuses);
    void remoteStatusesError(const QString& error);

private:
    static QString normalizeBaseUrl(const QString& rawUrl);
    static QString gitHubApiBaseUrl(const QString& rawBaseUrl);
    static void applyAuthHeader(QNetworkRequest& req, const TodoListEntry& entry, bool forceBearerAuth = false);
    void setSyncing(bool value);
    void setStatusMessage(const QString& msg);

    void fetchJiraIssues(const TodoListEntry& entry, int endpointStage = 0, bool forceBearerAuth = false);
    void fetchYouTrackIssues(const TodoListEntry& entry);
    void fetchGitHubIssues(const TodoListEntry& entry);
    void fetchGitLabIssues(const TodoListEntry& entry);
    void fetchRedmineIssues(const TodoListEntry& entry);
    void fetchTodoistTasks(const TodoListEntry& entry);
    void processSyncedItems(const TodoListEntry& entry, const QVector<RemoteIssueItem>& items, const QString& providerName);

    QNetworkAccessManager m_nam;
    bool m_syncing = false;
    QString m_lastStatusMessage;
};

} // namespace gofi
