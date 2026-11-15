#include "IssueTrackerService.h"
#include "trackers/TrackerCommon.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace gofi {


IssueTrackerService::IssueTrackerService(QObject* parent)
    : QObject(parent)
{
}

QString IssueTrackerService::normalizeBaseUrl(const QString& rawUrl) {
    QString u = rawUrl.trimmed();
    if (u.isEmpty()) return u;
    if (!u.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
        && !u.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        u = QStringLiteral("https://") + u;
    }
    // Strip common web UI suffixes if the user pasted a board/project/issue link
    const QStringList uiSuffixes = {
        QStringLiteral("/jira/software"),
        QStringLiteral("/jira/core"),
        QStringLiteral("/jira/servicedesk"),
        QStringLiteral("/secure/"),
        QStringLiteral("/browse/"),
        QStringLiteral("/projects/"),
        QStringLiteral("/issues")
    };
    for (const QString& marker : uiSuffixes) {
        const int pos = u.indexOf(marker, 8, Qt::CaseInsensitive);
        if (pos > 0) {
            u = u.left(pos);
            break;
        }
    }
    while (u.endsWith(QLatin1Char('/'))) {
        u.chop(1);
    }
    return u;
}

void IssueTrackerService::applyAuthHeader(QNetworkRequest& req, const TodoListEntry& entry, bool forceBearerAuth) {
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("User-Agent", "Go-For-It-Qt");
    req.setTransferTimeout(25000); // 25 seconds timeout to prevent hanging connections

    const QUrl reqUrl = req.url();
    if (reqUrl.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 && !entry.apiToken.trimmed().isEmpty()) {
        const QString host = reqUrl.host();
        if (host != QStringLiteral("localhost") && host != QStringLiteral("127.0.0.1")) {
            const QString warnMsg = QStringLiteral("Security warning: Transmitting API token over unencrypted HTTP to %1!").arg(host);
            qWarning() << warnMsg;
            setStatusMessage(warnMsg);
        }
    }

    const QString user = entry.jiraUser.trimmed();
    const QString token = entry.apiToken.trimmed();
    if (entry.provider == QStringLiteral("GitHub")) {
        req.setRawHeader("Accept", "application/vnd.github+json");
        if (!token.isEmpty()) {
            req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
        }
    } else if (entry.provider == QStringLiteral("GitLab")) {
        if (!token.isEmpty()) {
            req.setRawHeader("PRIVATE-TOKEN", token.toUtf8());
            req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
        }
    } else if (entry.provider == QStringLiteral("Redmine")) {
        if (!token.isEmpty()) {
            req.setRawHeader("X-Redmine-API-Key", token.toUtf8());
        }
        if (!user.isEmpty() && !token.isEmpty()) {
            const QByteArray credentials = (user + QLatin1Char(':') + token).toUtf8().toBase64();
            req.setRawHeader("Authorization", QByteArray("Basic ") + credentials);
        }
    } else if (entry.provider == QStringLiteral("Todoist")) {
        if (!token.isEmpty()) {
            req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
        }
    } else if (entry.provider == QStringLiteral("Nextcloud")) {
        if (!user.isEmpty() && !token.isEmpty()) {
            const QByteArray credentials = (user + QLatin1Char(':') + token).toUtf8().toBase64();
            req.setRawHeader("Authorization", QByteArray("Basic ") + credentials);
        } else if (!token.isEmpty()) {
            req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
        }
    } else if (!forceBearerAuth && !(m_jiraBearerTokens.contains(entry.id) && m_jiraBearerTokens.value(entry.id) == entry.apiToken) && entry.provider == QStringLiteral("Jira") && !user.isEmpty()) {
        const QByteArray credentials = (user + QLatin1Char(':') + token).toUtf8().toBase64();
        req.setRawHeader("Authorization", QByteArray("Basic ") + credentials);
    } else if (!token.isEmpty()) {
        req.setRawHeader("Authorization", QByteArray("Bearer ") + token.toUtf8());
    }
}

void IssueTrackerService::setSyncing(bool value) {
    if (!value) {
        m_activeSyncRequests = 0;
    }
    if (m_syncing == value) return;
    m_syncing = value;
    emit syncingChanged();
}

void IssueTrackerService::startSyncOperation() {
    m_activeSyncRequests++;
    if (!m_syncing) {
        m_syncing = true;
        emit syncingChanged();
    }
}

void IssueTrackerService::endSyncOperation() {
    m_activeSyncRequests = qMax(0, m_activeSyncRequests - 1);
    if (m_activeSyncRequests == 0 && m_syncing) {
        m_syncing = false;
        emit syncingChanged();
    }
}

void IssueTrackerService::setStatusMessage(const QString& msg) {
    if (m_lastStatusMessage == msg) return;
    m_lastStatusMessage = msg;
    emit statusMessageChanged();
}



void IssueTrackerService::fetchRemotePriorities(const QString& provider,
                                                const QString& serverUrl,
                                                const QString& username,
                                                const QString& apiToken,
                                                const QVariantMap& existingMapping)
{
    if (provider == QStringLiteral("GitHub") || provider == QStringLiteral("GitLab")
        || provider == QStringLiteral("Todoist") || provider == QStringLiteral("Nextcloud")) {
        emit remotePrioritiesLoaded(defaultPrioritiesForProvider(provider, existingMapping));
        return;
    }

    const QString baseUrl = normalizeBaseUrl(serverUrl);
    if (baseUrl.isEmpty()) {
        emit remotePrioritiesError(tr("Please enter the Server URL first."));
        return;
    }

    TodoListEntry tempEntry;
    tempEntry.provider = provider;
    tempEntry.jiraUrl = baseUrl;
    tempEntry.jiraUser = username;
    tempEntry.apiToken = apiToken;

    if (provider == QStringLiteral("Jira")) {
        fetchJiraPriorities(baseUrl, tempEntry, existingMapping);
    } else if (provider == QStringLiteral("Redmine")) {
        fetchRedminePriorities(baseUrl, tempEntry, existingMapping);
    } else {
        emit remotePrioritiesLoaded(defaultPrioritiesForProvider(provider, existingMapping));
    }
}



void IssueTrackerService::fetchRemoteStatuses(const QString& provider,
                                              const QString& serverUrl,
                                              const QString& username,
                                              const QString& apiToken,
                                              const QVariantMap& existingMapping)
{
    if (provider == QStringLiteral("GitHub")) {
        emit remoteStatusesLoaded(parseGitHubStatuses(existingMapping));
        return;
    }
    if (provider == QStringLiteral("GitLab")) {
        emit remoteStatusesLoaded(parseGitLabStatuses(existingMapping));
        return;
    }
    if (provider == QStringLiteral("Todoist")) {
        emit remoteStatusesLoaded(parseTodoistStatuses(existingMapping));
        return;
    }
    if (provider == QStringLiteral("Nextcloud")) {
        emit remoteStatusesLoaded(parseNextcloudStatuses(existingMapping));
        return;
    }

    const QString baseUrl = normalizeBaseUrl(serverUrl);
    if (baseUrl.isEmpty()) {
        emit remoteStatusesError(tr("Please enter the Server URL first."));
        return;
    }

    TodoListEntry tempEntry;
    tempEntry.provider = provider;
    tempEntry.jiraUrl = baseUrl;
    tempEntry.jiraUser = username;
    tempEntry.apiToken = apiToken;

    if (provider == QStringLiteral("Jira")) {
        fetchJiraStatuses(baseUrl, tempEntry, existingMapping);
    } else if (provider == QStringLiteral("YouTrack")) {
        fetchYouTrackStatuses(baseUrl, tempEntry, existingMapping);
    } else if (provider == QStringLiteral("Redmine")) {
        fetchRedmineStatuses(baseUrl, tempEntry, existingMapping);
    }
}

void IssueTrackerService::syncListIssues(const TodoListEntry& listEntry) {
    if (listEntry.provider == QStringLiteral("Jira")) {
        fetchJiraIssues(listEntry, 0, false);
    } else if (listEntry.provider == QStringLiteral("YouTrack")) {
        fetchYouTrackIssues(listEntry);
    } else if (listEntry.provider == QStringLiteral("GitHub")) {
        fetchGitHubIssues(listEntry);
    } else if (listEntry.provider == QStringLiteral("GitLab")) {
        fetchGitLabIssues(listEntry);
    } else if (listEntry.provider == QStringLiteral("Redmine")) {
        fetchRedmineIssues(listEntry);
    } else if (listEntry.provider == QStringLiteral("Todoist")) {
        fetchTodoistTasks(listEntry);
    } else if (listEntry.provider == QStringLiteral("Nextcloud")) {
        fetchNextcloudTasks(listEntry);
    }
}



void IssueTrackerService::processSyncedItems(const TodoListEntry& entry,
                                             const QVector<RemoteIssueItem>& items,
                                             const QString& providerName,
                                             bool pageLimitReached,
                                             bool hasError,
                                             bool explicitZeroTotal)
{
    DatabaseManager& db = DatabaseManager::instance();
    QStringList newIssues;
    QStringList statusChanges;
    QSet<QString> remoteKeys;

    if (!db.beginTransaction()) {
        endSyncOperation();
        const QString errMsg = tr("Failed to start database transaction for synchronized issues from %1.").arg(providerName);
        setStatusMessage(errMsg);
        emit syncFailed(entry.id, errMsg);
        return;
    }
    bool hasDbError = false;
    for (const RemoteIssueItem& item : items) {
        if (!item.key.isEmpty()) {
            remoteKeys.insert(item.key);
        }
        const QString localStatus = mapRemoteStatusToLocal(
            item.remoteStatus, item.remoteStatusCategory, entry.statusMapping
        );
        quint8 localPriority = item.priority;
        if (!entry.priorityMapping.isEmpty()) {
            const quint8 mapped = mapRemotePriorityToLocal(item.remotePriority, entry.priorityMapping);
            if (mapped != TxtTask::NoPriority || entry.priorityMapping.contains(item.remotePriority)) {
                localPriority = mapped;
            }
        }
        bool isNew = false;
        QString oldStatus;
        const bool ok = db.upsertRemoteIssueTask(
            entry.id, item.key, item.description, localStatus, localPriority,
            item.durationSeconds, item.details, item.comments, &isNew, &oldStatus
        );
        if (!ok) {
            hasDbError = true;
            break;
        }
        if (isNew) {
            if (!TxtTask::isClosedStatus(localStatus)) {
                newIssues.append(item.description);
            }
        } else if (!oldStatus.isEmpty() && oldStatus != localStatus) {
            statusChanges.append(QStringLiteral("%1: %2 → %3").arg(item.key, oldStatus, localStatus));
        }
    }

    // Reconcile: Only reconcile closed issues if:
    // 1. Page limit was NOT reached (pageLimitReached is false).
    // 2. No partial or parser error occurred (!hasError).
    // 3. No database write error occurred (!hasDbError).
    // 4. Remote items list is non-empty (!items.isEmpty()), OR the tracker explicitly reported total == 0 (explicitZeroTotal)
    //    in a valid error-free response (e.g. GitHub/Jira/Redmine total_count == 0).
    const bool limitReached = pageLimitReached;
    const bool canReconcile = !limitReached && !hasError && !hasDbError && (!items.isEmpty() || explicitZeroTotal);
    if (canReconcile) {
        const auto localOpenTasks = db.loadTasks(entry.id, false);
        for (const TxtTask& t : localOpenTasks) {
            if (!t.jiraKey().isEmpty() && !remoteKeys.contains(t.jiraKey())) {
                if (m_activeTaskChecker && m_activeTaskChecker(entry.id, t.jiraKey())) {
                    // Do not close a task that currently has an active running timer!
                    continue;
                }
                bool isNew = false;
                QString oldStatus;
                if (db.upsertRemoteIssueTask(
                    entry.id, t.jiraKey(), t.description(), QStringLiteral("done"),
                    t.priority(), t.duration(), t.details(), t.comments(), &isNew, &oldStatus)) {
                    statusChanges.append(QStringLiteral("%1: %2 → done (closed remotely)").arg(t.jiraKey(), t.status()));
                } else {
                    hasDbError = true;
                    break;
                }
            }
        }
    }

    if (hasDbError) {
        db.rollbackTransaction();
        endSyncOperation();
        const QString errMsg = tr("Database error occurred while saving synchronized issues from %1.").arg(providerName);
        setStatusMessage(errMsg);
        emit syncFailed(entry.id, errMsg);
        return;
    }

    if (!db.commitTransaction()) {
        db.rollbackTransaction();
        endSyncOperation();
        const QString errMsg = tr("Database transaction error while saving synchronized issues from %1.").arg(providerName);
        setStatusMessage(errMsg);
        emit syncFailed(entry.id, errMsg);
        return;
    }

    endSyncOperation();
    const QUrl reqUrl(entry.jiraUrl);
    const bool isInsecureHttp = reqUrl.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 &&
                                reqUrl.host() != QStringLiteral("localhost") &&
                                reqUrl.host() != QStringLiteral("127.0.0.1") &&
                                !entry.apiToken.trimmed().isEmpty();

    QString statusText;
    if (limitReached) {
        statusText = tr("Synced %1 issues from %2 (maximum page limit reached; older items omitted).").arg(items.size()).arg(providerName);
    } else {
        statusText = tr("Synced %1 issues from %2.").arg(items.size()).arg(providerName);
    }
    if (isInsecureHttp) {
        statusText += tr(" ⚠️ Insecure HTTP!");
    }
    setStatusMessage(statusText);
    emit listIssuesSynced(entry.id, static_cast<int>(items.size()));
    if (!newIssues.isEmpty() || !statusChanges.isEmpty()) {
        emit trackerChangesDetected(entry.name, newIssues, statusChanges);
    }
}

void IssueTrackerService::postWorklog(const TodoListEntry& listEntry,
                                      qint64 taskId,
                                      qint64 activityLogId,
                                      const QString& issueKey,
                                      const QDateTime& startTimeUtc,
                                      quint32 runtimeSeconds,
                                      bool forceBearerAuth)
{
    Q_UNUSED(taskId)
    if (QStandardPaths::isTestModeEnabled()) return;
    if (!listEntry.autoWorklog || issueKey.isEmpty() || runtimeSeconds == 0) {
        return;
    }
    const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
    if (baseUrl.isEmpty()) {
        return;
    }

    if (listEntry.provider == QStringLiteral("Jira")) {
        postJiraWorklog(listEntry, taskId, activityLogId, issueKey, startTimeUtc, runtimeSeconds, forceBearerAuth, baseUrl);
    } else if (listEntry.provider == QStringLiteral("YouTrack")) {
        postYouTrackWorklog(listEntry, activityLogId, issueKey, runtimeSeconds, startTimeUtc, baseUrl);
    } else if (listEntry.provider == QStringLiteral("GitHub")) {
        postGitHubWorklog(listEntry, activityLogId, issueKey, runtimeSeconds, baseUrl);
    } else if (listEntry.provider == QStringLiteral("GitLab")) {
        postGitLabWorklog(listEntry, activityLogId, issueKey, runtimeSeconds, baseUrl);
    } else if (listEntry.provider == QStringLiteral("Redmine")) {
        postRedmineWorklog(listEntry, activityLogId, issueKey, runtimeSeconds, startTimeUtc, baseUrl);
    } else if (listEntry.provider == QStringLiteral("Todoist")) {
        postTodoistWorklog(listEntry, activityLogId, issueKey, runtimeSeconds);
    } else if (listEntry.provider == QStringLiteral("Nextcloud")) {
        postNextcloudWorklog(listEntry, activityLogId, issueKey, runtimeSeconds);
    }
}



void IssueTrackerService::updateIssueDescription(const TodoListEntry& listEntry,
                                                 const QString& issueKey,
                                                 const QString& markdownDetails,
                                                 bool forceBearerAuth,
                                                 bool useV2Fallback)
{
    if (QStandardPaths::isTestModeEnabled()) return;
    if (issueKey.trimmed().isEmpty()) return;
    const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
    if (baseUrl.isEmpty()) return;

    if (listEntry.provider == QStringLiteral("Jira")) {
        updateJiraDescription(listEntry, issueKey, markdownDetails, forceBearerAuth, useV2Fallback, baseUrl);
    } else if (listEntry.provider == QStringLiteral("YouTrack")) {
        updateYouTrackDescription(listEntry, issueKey, markdownDetails, baseUrl);
    } else if (listEntry.provider == QStringLiteral("GitHub")) {
        updateGitHubDescription(listEntry, issueKey, markdownDetails, baseUrl);
    } else if (listEntry.provider == QStringLiteral("GitLab")) {
        updateGitLabDescription(listEntry, issueKey, markdownDetails, baseUrl);
    } else if (listEntry.provider == QStringLiteral("Redmine")) {
        updateRedmineDescription(listEntry, issueKey, markdownDetails, baseUrl);
    } else if (listEntry.provider == QStringLiteral("Todoist")) {
        updateTodoistDescription(listEntry, issueKey, markdownDetails);
    } else if (listEntry.provider == QStringLiteral("Nextcloud")) {
        updateNextcloudDescription(listEntry, issueKey, markdownDetails);
    }
}

void IssueTrackerService::updateIssueStatusRemote(const TodoListEntry& listEntry,
                                                  const QString& issueKey,
                                                  bool done,
                                                  const QString& localStatus,
                                                  bool forceBearerAuth)
{
    if (!listEntry.syncStatusToRemote) return;
    if (QStandardPaths::isTestModeEnabled()) return;
    if (issueKey.trimmed().isEmpty() || listEntry.apiToken.trimmed().isEmpty()) return;
    const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);

    auto failWithError = [this, listEntry, issueKey](int statusCode, const QString& errStr) {
        const QString errMsg = tr("Failed to update status for %1 on %2 (%3): %4")
            .arg(issueKey, listEntry.provider)
            .arg(statusCode)
            .arg(errStr);
        setStatusMessage(errMsg);
        emit syncFailed(listEntry.id, errMsg);
    };

    auto handleReply = [this, listEntry, issueKey, done, failWithError](QNetworkReply* reply) {
        if (!reply) return;
        connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, issueKey, done, failWithError]() {
            reply->deleteLater();
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || (statusCode >= 400 && statusCode < 600)) {
                failWithError(statusCode, reply->errorString());
            } else {
                setStatusMessage(done ? tr("Updated %1 on %2: closed").arg(issueKey, listEntry.provider)
                                      : tr("Updated %1 on %2: open").arg(issueKey, listEntry.provider));
            }
        });
    };

    if (listEntry.provider == QStringLiteral("GitHub")) {
        updateGitHubStatusRemote(listEntry, issueKey, done, baseUrl, handleReply);
    } else if (listEntry.provider == QStringLiteral("GitLab")) {
        updateGitLabStatusRemote(listEntry, issueKey, done, baseUrl, handleReply);
    } else if (listEntry.provider == QStringLiteral("Todoist")) {
        updateTodoistStatusRemote(listEntry, issueKey, done, handleReply);
    } else if (listEntry.provider == QStringLiteral("Redmine")) {
        updateRedmineStatusRemote(listEntry, issueKey, done, baseUrl, handleReply);
    } else if (listEntry.provider == QStringLiteral("Jira")) {
        updateJiraStatusRemote(listEntry, issueKey, done, localStatus, forceBearerAuth, baseUrl);
    } else if (listEntry.provider == QStringLiteral("YouTrack")) {
        updateYouTrackStatusRemote(listEntry, issueKey, done, baseUrl, handleReply);
    } else if (listEntry.provider == QStringLiteral("Nextcloud")) {
        updateNextcloudStatusRemote(listEntry, issueKey, done, handleReply, failWithError);
    }
}

void IssueTrackerService::postIssueComment(const TodoListEntry& listEntry,
                                           const QString& issueKey,
                                           const QString& markdownComment,
                                           bool forceBearerAuth,
                                           bool useV2Fallback)
{
    if (QStandardPaths::isTestModeEnabled()) return;
    if (issueKey.trimmed().isEmpty() || markdownComment.trimmed().isEmpty()) return;
    const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
    if (baseUrl.isEmpty()) return;

    if (listEntry.provider == QStringLiteral("Jira")) {
        postJiraComment(listEntry, issueKey, markdownComment, forceBearerAuth, useV2Fallback, baseUrl);
    } else if (listEntry.provider == QStringLiteral("YouTrack")) {
        postYouTrackComment(listEntry, issueKey, markdownComment, baseUrl);
    } else if (listEntry.provider == QStringLiteral("GitHub")) {
        postGitHubComment(listEntry, issueKey, markdownComment, baseUrl);
    } else if (listEntry.provider == QStringLiteral("GitLab")) {
        postGitLabComment(listEntry, issueKey, markdownComment, baseUrl);
    } else if (listEntry.provider == QStringLiteral("Redmine")) {
        postRedmineComment(listEntry, issueKey, markdownComment, baseUrl);
    } else if (listEntry.provider == QStringLiteral("Todoist")) {
        postTodoistComment(listEntry, issueKey, markdownComment);
    } else if (listEntry.provider == QStringLiteral("Nextcloud")) {
        postNextcloudComment(listEntry, issueKey, markdownComment);
    }
}

QUrl IssueTrackerService::buildIssueBrowserUrl(const TodoListEntry& listEntry,
                                               const QString& issueKey,
                                               const QString& taskDescription) {
    const QString provider = listEntry.provider.trimmed();

    if (provider == QStringLiteral("Jira")) {
        const QUrl url = buildJiraBrowserUrl(listEntry, issueKey);
        if (!url.isEmpty()) return url;
    } else if (provider == QStringLiteral("YouTrack")) {
        const QUrl url = buildYouTrackBrowserUrl(listEntry, issueKey);
        if (!url.isEmpty()) return url;
    } else if (provider == QStringLiteral("GitHub")) {
        return buildGitHubBrowserUrl(listEntry, issueKey);
    } else if (provider == QStringLiteral("GitLab")) {
        return buildGitLabBrowserUrl(listEntry, issueKey);
    } else if (provider == QStringLiteral("Redmine")) {
        const QUrl url = buildRedmineBrowserUrl(listEntry, issueKey);
        if (!url.isEmpty()) return url;
    } else if (provider == QStringLiteral("Todoist")) {
        return buildTodoistBrowserUrl(listEntry, issueKey);
    } else if (provider == QStringLiteral("Nextcloud")) {
        const QUrl url = buildNextcloudBrowserUrl(listEntry, issueKey);
        if (!url.isEmpty()) return url;
    }

    // Fallback: search taskDescription for an explicit HTTP/HTTPS link
    if (!taskDescription.isEmpty()) {
        static const QRegularExpression urlRe(QStringLiteral("https?://[^\\s<>\"'\\]\\)]+"));
        const auto match = urlRe.match(taskDescription);
        if (match.hasMatch()) {
            return QUrl(match.captured(0));
        }
    }

    return QUrl();
}

} // namespace gofi
