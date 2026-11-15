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
        QUrl url(baseUrl + QStringLiteral("/rest/api/2/priority"));
        QNetworkRequest req(url);
        applyAuthHeader(req, tempEntry, false);

        QNetworkReply* reply = m_nam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, tempEntry, baseUrl, existingMapping]() {
            reply->deleteLater();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray seraphReason = reply->rawHeader("X-Seraph-Loginreason");
            const bool seraphAuthFailed = seraphReason.contains("AUTHENTICATED_FAILED") || seraphReason.contains("AUTHENTICATION_DENIED");
            if (reply->error() != QNetworkReply::NoError || seraphAuthFailed) {
                if (!tempEntry.jiraUser.trimmed().isEmpty() && (httpStatus == 401 || httpStatus == 403 || seraphAuthFailed)) {
                    // Retry with Bearer token (Jira Server/DC PAT)
                    QNetworkRequest bearerReq(QUrl(baseUrl + QStringLiteral("/rest/api/2/priority")));
                    applyAuthHeader(bearerReq, tempEntry, true);
                    QNetworkReply* retryReply = m_nam.get(bearerReq);
                    connect(retryReply, &QNetworkReply::finished, this, [this, retryReply, existingMapping, seraphAuthFailed]() {
                        retryReply->deleteLater();
                        const QByteArray retrySeraph = retryReply->rawHeader("X-Seraph-Loginreason");
                        const bool retrySeraphFailed = retrySeraph.contains("AUTHENTICATED_FAILED") || retrySeraph.contains("AUTHENTICATION_DENIED");
                        if (retryReply->error() != QNetworkReply::NoError || retrySeraphFailed) {
                            if (seraphAuthFailed || retrySeraphFailed) {
                                emit remotePrioritiesError(tr("Jira authentication failed: please check your Jira Email (e.g. corporate email) and API Token."));
                            } else {
                                emit remotePrioritiesError(tr("Failed to load Jira priorities: %1").arg(retryReply->errorString()));
                            }
                            return;
                        }
                        const QVariantList list = parseJiraPrioritiesJson(retryReply->readAll(), existingMapping);
                        if (!list.isEmpty()) {
                            emit remotePrioritiesLoaded(list);
                            return;
                        }
                        const int retryHttpStatus = retryReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                        emit remotePrioritiesError(tr("Failed to load Jira priorities (%1): %2").arg(retryHttpStatus).arg(retryReply->errorString()));
                    });
                    return;
                }
                if (seraphAuthFailed) {
                    emit remotePrioritiesError(tr("Jira authentication failed: please check your Jira Email and API Token."));
                } else {
                    emit remotePrioritiesError(tr("Failed to load Jira priorities (%1): %2").arg(httpStatus).arg(reply->errorString()));
                }
                return;
            }
            const QVariantList list = parseJiraPrioritiesJson(reply->readAll(), existingMapping);
            if (!list.isEmpty()) {
                emit remotePrioritiesLoaded(list);
                return;
            }
            emit remotePrioritiesError(tr("Failed to load Jira priorities (%1): %2").arg(httpStatus).arg(reply->errorString()));
        });
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
        QUrl url(baseUrl + QStringLiteral("/rest/api/2/status"));
        QNetworkRequest req(url);
        applyAuthHeader(req, tempEntry, false);

        QNetworkReply* reply = m_nam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, tempEntry, baseUrl, existingMapping]() {
            reply->deleteLater();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray seraphReason = reply->rawHeader("X-Seraph-Loginreason");
            const bool seraphAuthFailed = seraphReason.contains("AUTHENTICATED_FAILED") || seraphReason.contains("AUTHENTICATION_DENIED");
            if (reply->error() != QNetworkReply::NoError || seraphAuthFailed) {
                if (!tempEntry.jiraUser.trimmed().isEmpty() && (httpStatus == 401 || httpStatus == 403 || seraphAuthFailed)) {
                    // Retry with Bearer token (Jira Server/DC PAT)
                    QNetworkRequest bearerReq(QUrl(baseUrl + QStringLiteral("/rest/api/2/status")));
                    applyAuthHeader(bearerReq, tempEntry, true);
                    QNetworkReply* retryReply = m_nam.get(bearerReq);
                    connect(retryReply, &QNetworkReply::finished, this, [this, retryReply, existingMapping, seraphAuthFailed]() {
                        retryReply->deleteLater();
                        const QByteArray retrySeraph = retryReply->rawHeader("X-Seraph-Loginreason");
                        const bool retrySeraphFailed = retrySeraph.contains("AUTHENTICATED_FAILED") || retrySeraph.contains("AUTHENTICATION_DENIED");
                        if (retryReply->error() != QNetworkReply::NoError || retrySeraphFailed) {
                            if (seraphAuthFailed || retrySeraphFailed) {
                                emit remoteStatusesError(tr("Jira authentication failed: please check your Jira Email (e.g. corporate email) and API Token."));
                            } else {
                                emit remoteStatusesError(tr("Failed to load Jira statuses: %1").arg(retryReply->errorString()));
                            }
                            return;
                        }
                        emit remoteStatusesLoaded(parseJiraStatusesJson(retryReply->readAll(), existingMapping));
                    });
                    return;
                }
                if (seraphAuthFailed) {
                    emit remoteStatusesError(tr("Jira authentication failed: please check your Jira Email and API Token."));
                } else {
                    emit remoteStatusesError(tr("Failed to load Jira statuses (%1): %2").arg(httpStatus).arg(reply->errorString()));
                }
                return;
            }
            emit remoteStatusesLoaded(parseJiraStatusesJson(reply->readAll(), existingMapping));
        });
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
        const bool effectiveBearer = forceBearerAuth || (m_jiraBearerTokens.contains(listEntry.id) && m_jiraBearerTokens.value(listEntry.id) == listEntry.apiToken);
        const quint32 loggedSeconds = qMax(60U, runtimeSeconds);
        const QString encodedKey = QString::fromUtf8(QUrl::toPercentEncoding(issueKey));
        QUrl url(baseUrl + QStringLiteral("/rest/api/2/issue/") + encodedKey + QStringLiteral("/worklog"));
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry, effectiveBearer);

        QJsonObject payload;
        payload[QStringLiteral("timeSpentSeconds")] = static_cast<qint64>(loggedSeconds);
        payload[QStringLiteral("started")] = startTimeUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.000'+0000'"));
        payload[QStringLiteral("comment")] = QStringLiteral("Logged via Go for It Qt Pomodoro timer");

        QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, taskId, activityLogId, issueKey, startTimeUtc, runtimeSeconds, loggedSeconds, effectiveBearer]() {
            reply->deleteLater();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray seraph = reply->rawHeader("X-Seraph-Loginreason");
            const bool authErr = (httpStatus == 401 || httpStatus == 403 || seraph.contains("AUTHENTICATED_FAILED"));

            if (reply->error() != QNetworkReply::NoError || authErr) {
                if (effectiveBearer) {
                    m_jiraBearerTokens.remove(listEntry.id);
                }
                if (!effectiveBearer && !listEntry.jiraUser.trimmed().isEmpty() && authErr) {
                    postWorklog(listEntry, taskId, activityLogId, issueKey, startTimeUtc, runtimeSeconds, true);
                    return;
                }
                const QString errMsg = tr("Failed to post worklog to Jira (%1): %2")
                    .arg(httpStatus)
                    .arg(reply->errorString());
                setStatusMessage(errMsg);
                emit syncFailed(listEntry.id, errMsg);
                return;
            }

            if (effectiveBearer) {
                m_jiraBearerTokens[listEntry.id] = listEntry.apiToken;
            }

            if (DatabaseManager::instance().isOpen() && !DatabaseManager::instance().markActivityWorklogSynced(activityLogId)) {
                const QString dbErr = tr("Database error: failed to mark activity worklog as synced for %1.").arg(issueKey);
                setStatusMessage(dbErr);
                emit syncFailed(listEntry.id, dbErr);
            } else {
                setStatusMessage(tr("Logged %1m to Jira issue %2").arg(loggedSeconds / 60U).arg(issueKey));
                emit worklogPosted(issueKey, loggedSeconds);
            }
        });
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
        const bool isCloud = baseUrl.contains(QStringLiteral(".atlassian.net"), Qt::CaseInsensitive);
        const bool useV3 = isCloud && !useV2Fallback;
        const QString apiVer = useV3 ? QStringLiteral("3") : QStringLiteral("2");
        QUrl url(baseUrl + QStringLiteral("/rest/api/") + apiVer + QStringLiteral("/issue/") + issueKey.trimmed());
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry, forceBearerAuth);

        QJsonObject fieldsObj;
        if (useV3) {
            fieldsObj[QStringLiteral("description")] = markdownToJiraAdf(markdownDetails);
        } else {
            fieldsObj[QStringLiteral("description")] = markdownDetails;
        }
        QJsonObject payload{{QStringLiteral("fields"), fieldsObj}};

        QNetworkReply* reply = m_nam.put(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, issueKey, markdownDetails, forceBearerAuth, useV3, useV2Fallback]() {
            reply->deleteLater();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray seraph = reply->rawHeader("X-Seraph-Loginreason");
            const bool authErr = (httpStatus == 401 || httpStatus == 403 || seraph.contains("AUTHENTICATED_FAILED"));
            if (reply->error() != QNetworkReply::NoError || authErr) {
                if (forceBearerAuth) {
                    m_jiraBearerTokens.remove(listEntry.id);
                }
                if (!forceBearerAuth && !listEntry.jiraUser.trimmed().isEmpty() && authErr) {
                    updateIssueDescription(listEntry, issueKey, markdownDetails, true, useV2Fallback);
                    return;
                }
                if (useV3 && !useV2Fallback && (httpStatus == 400 || httpStatus == 404)) {
                    updateIssueDescription(listEntry, issueKey, markdownDetails, forceBearerAuth, true);
                    return;
                }
                setStatusMessage(tr("Failed to update description on Jira (%1): %2").arg(issueKey, reply->errorString()));
                return;
            }
            if (forceBearerAuth) {
                m_jiraBearerTokens[listEntry.id] = listEntry.apiToken;
            }
            setStatusMessage(tr("Updated description on Jira issue %1.").arg(issueKey));
        });
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
        const bool isCloud = baseUrl.contains(QStringLiteral(".atlassian.net"), Qt::CaseInsensitive);
        const QString apiVer = isCloud ? QStringLiteral("3") : QStringLiteral("2");
        const QUrl transUrl(baseUrl + QStringLiteral("/rest/api/") + apiVer + QStringLiteral("/issue/") + issueKey.trimmed() + QStringLiteral("/transitions"));
        QNetworkRequest req(transUrl);
        applyAuthHeader(req, listEntry, forceBearerAuth);

        QNetworkReply* reply = m_nam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, issueKey, done, localStatus, transUrl, forceBearerAuth]() {
            reply->deleteLater();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray seraph = reply->rawHeader("X-Seraph-Loginreason");
            const bool authErr = (httpStatus == 401 || httpStatus == 403 || seraph.contains("AUTHENTICATED_FAILED"));
            auto fail = [this, &listEntry](const QString& errMsg) {
                setStatusMessage(errMsg);
                emit syncFailed(listEntry.id, errMsg);
            };
            if (reply->error() != QNetworkReply::NoError || authErr) {
                if (forceBearerAuth) {
                    m_jiraBearerTokens.remove(listEntry.id);
                }
                if (!forceBearerAuth && !listEntry.jiraUser.trimmed().isEmpty() && authErr) {
                    updateIssueStatusRemote(listEntry, issueKey, done, localStatus, true);
                    return;
                }
                fail(tr("Failed to update status for %1 on %2 (%3): %4")
                         .arg(issueKey, listEntry.provider).arg(httpStatus).arg(reply->errorString()));
                return;
            }

            // Reopening goes to the "To Do" category (or "In Progress" when the local status is
            // in_progress); completing goes to the "done" category. Category matches win over
            // name matches, and there is deliberately no fallback to another category.
            const QString wantedCategory = done ? QStringLiteral("done")
                : (localStatus == QStringLiteral("in_progress") ? QStringLiteral("indeterminate") : QStringLiteral("new"));
            const QStringList toNames = done
                ? QStringList{QStringLiteral("done"), QStringLiteral("closed"), QStringLiteral("resolved")}
                : (wantedCategory == QStringLiteral("indeterminate")
                       ? QStringList{QStringLiteral("in progress")}
                       : QStringList{QStringLiteral("to do"), QStringLiteral("open"), QStringLiteral("reopened"), QStringLiteral("backlog")});

            const QJsonArray transitions = QJsonDocument::fromJson(reply->readAll()).object()
                                               .value(QStringLiteral("transitions")).toArray();
            QString selectedTransitionId;
            for (const QJsonValue& val : transitions) {
                const QJsonObject toObj = val.toObject().value(QStringLiteral("to")).toObject();
                const QString catKey = toObj.value(QStringLiteral("statusCategory")).toObject()
                                           .value(QStringLiteral("key")).toString().toLower();
                if (catKey == wantedCategory) {
                    selectedTransitionId = val.toObject().value(QStringLiteral("id")).toString();
                    break;
                }
            }
            if (selectedTransitionId.isEmpty()) {
                for (const QJsonValue& val : transitions) {
                    const QString toName = val.toObject().value(QStringLiteral("to")).toObject()
                                               .value(QStringLiteral("name")).toString().toLower();
                    if (toNames.contains(toName)) {
                        selectedTransitionId = val.toObject().value(QStringLiteral("id")).toString();
                        break;
                    }
                }
            }
            if (selectedTransitionId.isEmpty()) {
                fail(tr("No matching Jira transition to update status of %1; status was not changed on %2.")
                         .arg(issueKey, listEntry.provider));
                return;
            }

            QNetworkRequest postReq(transUrl);
            applyAuthHeader(postReq, listEntry, forceBearerAuth);
            postReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            const QJsonObject payload{{QStringLiteral("transition"), QJsonObject{{QStringLiteral("id"), selectedTransitionId}}}};
            QNetworkReply* postReply = m_nam.post(postReq, QJsonDocument(payload).toJson(QJsonDocument::Compact));
            connect(postReply, &QNetworkReply::finished, this, [this, postReply, listEntry, issueKey, done, forceBearerAuth]() {
                postReply->deleteLater();
                const int status = postReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (postReply->error() != QNetworkReply::NoError || (status >= 400 && status < 600)) {
                    const QString errMsg = tr("Failed to update status for %1 on %2 (%3): %4")
                        .arg(issueKey, listEntry.provider).arg(status).arg(postReply->errorString());
                    setStatusMessage(errMsg);
                    emit syncFailed(listEntry.id, errMsg);
                    return;
                }
                if (forceBearerAuth) {
                    m_jiraBearerTokens[listEntry.id] = listEntry.apiToken;
                }
                setStatusMessage(done ? tr("Updated %1 on %2: closed").arg(issueKey, listEntry.provider)
                                      : tr("Updated %1 on %2: open").arg(issueKey, listEntry.provider));
            });
        });
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
        const bool isCloud = baseUrl.contains(QStringLiteral(".atlassian.net"), Qt::CaseInsensitive);
        const bool useV3 = isCloud && !useV2Fallback;
        const QString apiVer = useV3 ? QStringLiteral("3") : QStringLiteral("2");
        QUrl url(baseUrl + QStringLiteral("/rest/api/") + apiVer + QStringLiteral("/issue/") + issueKey.trimmed() + QStringLiteral("/comment"));
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry, forceBearerAuth);

        QJsonObject payload;
        if (useV3) {
            payload[QStringLiteral("body")] = markdownToJiraAdf(markdownComment);
        } else {
            payload[QStringLiteral("body")] = markdownComment;
        }

        QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, issueKey, markdownComment, forceBearerAuth, useV3, useV2Fallback]() {
            reply->deleteLater();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray seraph = reply->rawHeader("X-Seraph-Loginreason");
            const bool authErr = (httpStatus == 401 || httpStatus == 403 || seraph.contains("AUTHENTICATED_FAILED"));
            if (reply->error() != QNetworkReply::NoError || authErr) {
                if (forceBearerAuth) {
                    m_jiraBearerTokens.remove(listEntry.id);
                }
                if (!forceBearerAuth && !listEntry.jiraUser.trimmed().isEmpty() && authErr) {
                    postIssueComment(listEntry, issueKey, markdownComment, true, useV2Fallback);
                    return;
                }
                if (useV3 && !useV2Fallback && (httpStatus == 400 || httpStatus == 404)) {
                    postIssueComment(listEntry, issueKey, markdownComment, forceBearerAuth, true);
                    return;
                }
                setStatusMessage(tr("Failed to post comment to Jira (%1): %2").arg(issueKey, reply->errorString()));
                return;
            }
            if (forceBearerAuth) {
                m_jiraBearerTokens[listEntry.id] = listEntry.apiToken;
            }
            setStatusMessage(tr("Posted comment to Jira issue %1.").arg(issueKey));
        });
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
        const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
        const QString key = issueKey.trimmed();
        if (!baseUrl.isEmpty() && !key.isEmpty()) {
            return QUrl(baseUrl + QStringLiteral("/browse/") + key);
        }
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
