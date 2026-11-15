#include "trackers/TrackerCommon.h"
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

} // namespace gofi
