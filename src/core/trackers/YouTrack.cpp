#include "trackers/YouTrackProvider.h"
#include "trackers/TrackerCommon.h"
#include "DatabaseManager.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace gofi {

QVector<RemoteIssueItem> IssueTrackerService::parseYouTrackIssuesJson(const QByteArray& jsonBytes) {
    QVector<RemoteIssueItem> result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isArray()) return result;

    const QJsonArray issues = doc.array();
    result.reserve(issues.size());

    for (const QJsonValue& val : issues) {
        const QJsonObject issueObj = val.toObject();
        const QString key = issueObj.value(QStringLiteral("idReadable")).toString().trimmed();
        if (key.isEmpty()) continue;

        const QString summary = issueObj.value(QStringLiteral("summary")).toString().trimmed();
        const QString projectKey = sanitizeTagToken(
            issueObj.value(QStringLiteral("project")).toObject().value(QStringLiteral("shortName")).toString()
        );

        QString rawStateName;
        bool isResolved = false;
        QString priorityName;
        quint32 durationSec = 0;

        const QJsonArray customFields = issueObj.value(QStringLiteral("customFields")).toArray();
        for (const QJsonValue& cfVal : customFields) {
            const QJsonObject cfObj = cfVal.toObject();
            const QString cfName = cfObj.value(QStringLiteral("name")).toString().toLower();
            const QJsonValue cfValue = cfObj.value(QStringLiteral("value"));
            if (cfValue.isObject()) {
                const QJsonObject vObj = cfValue.toObject();
                if (cfName == QStringLiteral("priority") && priorityName.isEmpty()) {
                    priorityName = vObj.value(QStringLiteral("name")).toString();
                } else if ((cfName == QStringLiteral("state") || cfName == QStringLiteral("stage")) && rawStateName.isEmpty()) {
                    rawStateName = vObj.value(QStringLiteral("name")).toString().trimmed();
                    isResolved = vObj.value(QStringLiteral("isResolved")).toBool(false);
                } else if (cfName == QStringLiteral("estimation") && vObj.contains(QStringLiteral("minutes"))) {
                    durationSec = vObj.value(QStringLiteral("minutes")).toVariant().toUInt() * 60U;
                }
            }
        }

        const QString stateTag = sanitizeTagToken(rawStateName);
        QString desc = key + QLatin1Char(' ') + (summary.isEmpty() ? key : summary);
        if (!projectKey.isEmpty()) {
            desc += QStringLiteral(" +") + projectKey;
        }
        if (!stateTag.isEmpty()) {
            desc += QStringLiteral(" @") + stateTag;
        }

        const QString fullDetails = issueObj.value(QStringLiteral("description")).toString().trimmed();
        QVariantList parsedComments;
        const QJsonArray commentsArr = issueObj.value(QStringLiteral("comments")).toArray();
        for (const QJsonValue& cVal : commentsArr) {
            const QJsonObject cObj = cVal.toObject();
            const QJsonObject authorObj = cObj.value(QStringLiteral("author")).toObject();
            QString author = authorObj.value(QStringLiteral("fullName")).toString().trimmed();
            if (author.isEmpty()) author = authorObj.value(QStringLiteral("login")).toString().trimmed();
            const qint64 createdMs = cObj.value(QStringLiteral("created")).toVariant().toLongLong();
            const QString createdStr = createdMs > 0
                ? QDateTime::fromMSecsSinceEpoch(createdMs).toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                : QString();
            const QString commentBody = cObj.value(QStringLiteral("text")).toString().trimmed();
            if (!commentBody.isEmpty()) {
                parsedComments.append(QVariantMap{
                    {QStringLiteral("author"), author.isEmpty() ? QStringLiteral("YouTrack User") : author},
                    {QStringLiteral("created"), createdStr},
                    {QStringLiteral("body"), commentBody}
                });
            }
        }

        RemoteIssueItem item;
        item.key = key;
        item.description = desc;
        item.details = fullDetails;
        item.comments = parsedComments;
        item.remoteStatus = rawStateName;
        item.remoteStatusCategory = isResolved ? QStringLiteral("done") : QStringLiteral("");
        item.remotePriority = priorityName;
        item.priority = mapPriorityName(priorityName);
        item.durationSeconds = durationSec;
        result.append(std::move(item));
    }
    return result;
}

QVariantList IssueTrackerService::parseYouTrackStatusesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping) {
    QVariantList result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isArray()) return result;

    QSet<QString> seen;
    for (const QJsonValue& bundleVal : doc.array()) {
        const QJsonObject bundleObj = bundleVal.toObject();
        // Either a bundle with "values" array or an issue with "customFields"
        if (bundleObj.contains(QStringLiteral("values"))) {
            for (const QJsonValue& v : bundleObj.value(QStringLiteral("values")).toArray()) {
                const QJsonObject vObj = v.toObject();
                const QString name = vObj.value(QStringLiteral("name")).toString().trimmed();
                if (name.isEmpty() || seen.contains(name.toLower())) continue;
                seen.insert(name.toLower());
                const bool resolved = vObj.value(QStringLiteral("isResolved")).toBool(false);
                result.append(QVariantMap{
                    {QStringLiteral("remoteName"), name},
                    {QStringLiteral("localStatus"), mapRemoteStatusToLocal(name, resolved ? QStringLiteral("done") : QStringLiteral(""), existingMapping)}
                });
            }
        } else if (bundleObj.contains(QStringLiteral("customFields"))) {
            for (const QJsonValue& cfVal : bundleObj.value(QStringLiteral("customFields")).toArray()) {
                const QJsonObject cfObj = cfVal.toObject();
                const QString cfName = cfObj.value(QStringLiteral("name")).toString().toLower();
                if (cfName != QStringLiteral("state") && cfName != QStringLiteral("stage")) continue;
                const QJsonObject vObj = cfObj.value(QStringLiteral("value")).toObject();
                const QString name = vObj.value(QStringLiteral("name")).toString().trimmed();
                if (name.isEmpty() || seen.contains(name.toLower())) continue;
                seen.insert(name.toLower());
                const bool resolved = vObj.value(QStringLiteral("isResolved")).toBool(false);
                result.append(QVariantMap{
                    {QStringLiteral("remoteName"), name},
                    {QStringLiteral("localStatus"), mapRemoteStatusToLocal(name, resolved ? QStringLiteral("done") : QStringLiteral(""), existingMapping)}
                });
            }
        }
    }
    return result;
}

struct IssueTrackerService::YouTrackSyncState {
    QVector<RemoteIssueItem> allItems;
    QStringList firstPassKeys;
    int skip = 0;
    int pagesFetched = 0;
    int nonEmptyPages = 0;
    bool limitReached = false;
    bool hasError = false;
    bool explicitZeroTotal = false;
    bool isSecondPass = false;
};

void IssueTrackerService::fetchYouTrackIssues(const TodoListEntry& entry) {
    const QString baseUrl = normalizeBaseUrl(entry.jiraUrl);
    if (baseUrl.isEmpty()) {
        emit syncFailed(entry.id, tr("YouTrack Server URL is empty."));
        return;
    }

    startSyncOperation();
    setStatusMessage(tr("Syncing issues from YouTrack..."));

    const QString ytQuery = entry.jiraJql.trimmed().isEmpty()
        ? QStringLiteral("for: me #Unresolved sort by: created asc")
        : entry.jiraJql.trimmed();

    auto state = std::make_shared<YouTrackSyncState>();
    fetchYouTrackPage(entry, state, baseUrl, ytQuery);
}

void IssueTrackerService::fetchYouTrackPage(const TodoListEntry& entry,
                                           const std::shared_ptr<YouTrackSyncState>& state,
                                           const QString& baseUrl,
                                           const QString& ytQuery)
{
    QUrl url(baseUrl + QStringLiteral("/api/issues"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("query"), ytQuery);
    query.addQueryItem(QStringLiteral("$top"), QStringLiteral("100"));
    if (state->skip > 0) {
        query.addQueryItem(QStringLiteral("$skip"), QString::number(state->skip));
    }
    query.addQueryItem(
        QStringLiteral("fields"),
        QStringLiteral("idReadable,summary,description,project(shortName),customFields(name,value(name,isResolved,minutes)),comments(author(fullName,login),created,text)")
    );
    url.setQuery(query);

    QNetworkRequest req(url);
    applyAuthHeader(req, entry);

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry, state, baseUrl, ytQuery]() {
        reply->deleteLater();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError) {
            if (state->pagesFetched == 0 && !state->isSecondPass) {
                endSyncOperation();
                const QString errMsg = tr("YouTrack sync error (%1): %2").arg(httpStatus).arg(reply->errorString());
                setStatusMessage(errMsg);
                emit syncFailed(entry.id, errMsg);
                return;
            } else {
                state->hasError = true;
                processSyncedItems(entry, state->allItems, QStringLiteral("YouTrack"), state->limitReached, state->hasError);
                return;
            }
        }

        const QByteArray bodyBytes = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(bodyBytes);
        if (!doc.isArray()) {
            state->hasError = true;
        }

        const int rawCount = doc.isArray() ? doc.array().size() : 0;
        const QVector<RemoteIssueItem> items = parseYouTrackIssuesJson(bodyBytes);
        state->allItems.append(items);
        state->pagesFetched++;
        if (rawCount > 0) {
            state->nonEmptyPages++;
        }
        state->skip += rawCount;

        if (state->pagesFetched == 1 && rawCount == 0 && !state->hasError) {
            state->explicitZeroTotal = true;
        }

        const bool hasMore = (rawCount >= 100);
        if (hasMore && !state->hasError) {
            if (state->pagesFetched < 50) {
                fetchYouTrackPage(entry, state, baseUrl, ytQuery);
                return;
            } else {
                state->limitReached = true;
            }
        }

        if (!state->isSecondPass) {
            if (state->nonEmptyPages > 1 && !state->hasError && !state->limitReached) {
                state->firstPassKeys.clear();
                for (const auto& it : state->allItems) {
                    state->firstPassKeys.append(it.key);
                }
                state->allItems.clear();
                state->skip = 0;
                state->pagesFetched = 0;
                state->nonEmptyPages = 0;
                state->isSecondPass = true;
                fetchYouTrackPage(entry, state, baseUrl, ytQuery);
                return;
            }
        } else {
            QStringList secondPassKeys;
            for (const auto& it : state->allItems) {
                secondPassKeys.append(it.key);
            }
            if (secondPassKeys != state->firstPassKeys) {
                state->limitReached = true;
            }
        }

        processSyncedItems(entry, state->allItems, QStringLiteral("YouTrack"), state->limitReached, state->hasError, state->explicitZeroTotal);
    });
}

void IssueTrackerService::fetchYouTrackStatuses(const QString& baseUrl, const TodoListEntry& tempEntry, const QVariantMap& existingMapping) {
    QUrl url(baseUrl + QStringLiteral("/api/admin/customFieldSettings/bundles/state?fields=values(name,isResolved)"));
    QNetworkRequest req(url);
    applyAuthHeader(req, tempEntry);

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, tempEntry, existingMapping]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            const QVariantList list = parseYouTrackStatusesJson(reply->readAll(), existingMapping);
            if (!list.isEmpty()) {
                emit remoteStatusesLoaded(list);
                return;
            }
        }
        // Fallback for non-admin YouTrack users: extract states from issues
        QUrl fallbackUrl(tempEntry.jiraUrl + QStringLiteral("/api/issues?$top=100&fields=customFields(name,value(name,isResolved))"));
        QNetworkRequest fbReq(fallbackUrl);
        applyAuthHeader(fbReq, tempEntry);
        QNetworkReply* fbReply = m_nam.get(fbReq);
        connect(fbReply, &QNetworkReply::finished, this, [this, fbReply, existingMapping]() {
            fbReply->deleteLater();
            if (fbReply->error() != QNetworkReply::NoError) {
                emit remoteStatusesError(tr("Failed to load YouTrack states: %1").arg(fbReply->errorString()));
                return;
            }
            emit remoteStatusesLoaded(parseYouTrackStatusesJson(fbReply->readAll(), existingMapping));
        });
    });
}

void IssueTrackerService::postYouTrackWorklog(const TodoListEntry& listEntry,
                                              qint64 activityLogId,
                                              const QString& issueKey,
                                              quint32 runtimeSeconds,
                                              const QDateTime& startTimeUtc,
                                              const QString& baseUrl) {
    const quint32 minutes = qMax(1U, (runtimeSeconds + 30U) / 60U);
    const QString encodedKey = QString::fromUtf8(QUrl::toPercentEncoding(issueKey));
    QUrl url(baseUrl + QStringLiteral("/api/issues/") + encodedKey + QStringLiteral("/timeTracking/workItems?fields=id"));
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);

    QJsonObject durationObj;
    durationObj[QStringLiteral("minutes")] = static_cast<qint64>(minutes);

    QJsonObject payload;
    payload[QStringLiteral("duration")] = durationObj;
    payload[QStringLiteral("date")] = startTimeUtc.toMSecsSinceEpoch();
    payload[QStringLiteral("text")] = QStringLiteral("Logged via Go for It Qt Pomodoro timer");
    payload[QStringLiteral("usesMarkdown")] = true;

    QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, activityLogId, issueKey, minutes]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            if (!DatabaseManager::instance().markActivityWorklogSynced(activityLogId)) {
                const QString dbErr = tr("Database error: failed to mark activity worklog as synced for %1.").arg(issueKey);
                setStatusMessage(dbErr);
                emit syncFailed(listEntry.id, dbErr);
            } else {
                setStatusMessage(tr("Logged %1m to YouTrack issue %2").arg(minutes).arg(issueKey));
                emit worklogPosted(issueKey, minutes * 60U);
            }
        } else {
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString errMsg = tr("Failed to post worklog to YouTrack (%1): %2")
                .arg(statusCode)
                .arg(reply->errorString());
            setStatusMessage(errMsg);
            emit syncFailed(listEntry.id, errMsg);
        }
    });
}

void IssueTrackerService::updateYouTrackDescription(const TodoListEntry& listEntry,
                                                    const QString& issueKey,
                                                    const QString& markdownDetails,
                                                    const QString& baseUrl) {
    QUrl url(baseUrl + QStringLiteral("/api/issues/") + issueKey.trimmed() + QStringLiteral("?fields=id"));
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);

    QJsonObject payload{{QStringLiteral("description"), markdownDetails}};
    QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, issueKey]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setStatusMessage(tr("Failed to update description on YouTrack (%1): %2").arg(issueKey, reply->errorString()));
            return;
        }
        setStatusMessage(tr("Updated description on YouTrack issue %1.").arg(issueKey));
    });
}

void IssueTrackerService::updateYouTrackStatusRemote(const TodoListEntry& listEntry,
                                                     const QString& issueKey,
                                                     bool done,
                                                     const QString& baseUrl,
                                                     const std::function<void(QNetworkReply*)>& handleReply) {
    QUrl url(baseUrl + QStringLiteral("/api/commands"));
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QString statusCmd = done ? QStringLiteral("State Fixed") : QStringLiteral("State Open");
    {
        // Pick the remote state mapped to the wanted local status; reopening prefers "todo",
        // then "in_progress", and never a canceled/paused state. Keys starting with '_' are
        // metadata entries, not remote state names.
        const QStringList wanted = done
            ? QStringList{QStringLiteral("done")}
            : QStringList{QStringLiteral("todo"), QStringLiteral("in_progress")};
        QString chosenState;
        for (const QString& category : wanted) {
            for (auto it = listEntry.statusMapping.constBegin(); it != listEntry.statusMapping.constEnd(); ++it) {
                if (it.key().startsWith(QLatin1Char('_'))) continue;
                if (it.value().toString().compare(category, Qt::CaseInsensitive) == 0) {
                    chosenState = it.key();
                    break;
                }
            }
            if (!chosenState.isEmpty()) break;
        }
        if (!chosenState.isEmpty()) {
            statusCmd = QStringLiteral("State {%1}").arg(chosenState);
        }
    }

    QJsonObject payload{
        {QStringLiteral("issues"), QJsonArray{QJsonObject{{QStringLiteral("idReadable"), issueKey.trimmed()}}}},
        {QStringLiteral("query"), statusCmd}
    };
    QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    handleReply(reply);
}

void IssueTrackerService::postYouTrackComment(const TodoListEntry& listEntry,
                                              const QString& issueKey,
                                              const QString& markdownComment,
                                              const QString& baseUrl) {
    QUrl url(baseUrl + QStringLiteral("/api/issues/") + issueKey.trimmed() + QStringLiteral("/comments?fields=id"));
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);

    QJsonObject payload{
        {QStringLiteral("text"), markdownComment},
        {QStringLiteral("usesMarkdown"), true}
    };
    QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, issueKey]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setStatusMessage(tr("Failed to post comment to YouTrack (%1): %2").arg(issueKey, reply->errorString()));
            return;
        }
        setStatusMessage(tr("Posted comment to YouTrack issue %1.").arg(issueKey));
    });
}

QUrl IssueTrackerService::buildYouTrackBrowserUrl(const TodoListEntry& listEntry, const QString& issueKey) {
    const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
    const QString key = issueKey.trimmed();
    if (!baseUrl.isEmpty() && !key.isEmpty()) {
        return QUrl(baseUrl + QStringLiteral("/issue/") + key);
    }
    return QUrl();
}

void YouTrackProvider::syncList(const TodoListEntry& entry) {
    service()->fetchYouTrackIssues(entry);
}

void YouTrackProvider::postWorklog(const TodoListEntry& entry,
                                   qint64,
                                   qint64 activityLogId,
                                   const QString& issueKey,
                                   const QDateTime& startTimeUtc,
                                   quint32 runtimeSeconds,
                                   bool,
                                   const QString& baseUrl) {
    service()->postYouTrackWorklog(entry, activityLogId, issueKey, runtimeSeconds, startTimeUtc, baseUrl);
}

void YouTrackProvider::updateDescription(const TodoListEntry& entry,
                                         const QString& issueKey,
                                         const QString& markdownDetails,
                                         bool,
                                         bool,
                                         const QString& baseUrl) {
    service()->updateYouTrackDescription(entry, issueKey, markdownDetails, baseUrl);
}

void YouTrackProvider::updateStatusRemote(const TodoListEntry& entry,
                                          const QString& issueKey,
                                          bool done,
                                          const QString&,
                                          bool,
                                          const QString& baseUrl,
                                          const StatusReplyHandler& handleReply,
                                          const StatusErrorHandler&) {
    service()->updateYouTrackStatusRemote(entry, issueKey, done, baseUrl, handleReply);
}

void YouTrackProvider::postComment(const TodoListEntry& entry,
                                   const QString& issueKey,
                                   const QString& markdownComment,
                                   bool,
                                   bool,
                                   const QString& baseUrl) {
    service()->postYouTrackComment(entry, issueKey, markdownComment, baseUrl);
}

void YouTrackProvider::fetchRemoteStatuses(const QString& baseUrl,
                                           const TodoListEntry& tempEntry,
                                           const QVariantMap& existingMapping) {
    service()->fetchYouTrackStatuses(baseUrl, tempEntry, existingMapping);
}

void YouTrackProvider::fetchRemotePriorities(const QString&,
                                             const TodoListEntry&,
                                             const QVariantMap& existingMapping) {
    emit service()->remotePrioritiesLoaded(defaultPriorities(existingMapping));
}

QVariantList YouTrackProvider::defaultPriorities(const QVariantMap& existingMapping) const {
    return service()->defaultPrioritiesForProvider(name(), existingMapping);
}

QUrl YouTrackProvider::buildBrowserUrl(const TodoListEntry& entry, const QString& issueKey) const {
    return IssueTrackerService::buildYouTrackBrowserUrl(entry, issueKey);
}

} // namespace gofi
