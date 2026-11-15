#include "trackers/TrackerCommon.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace gofi {

QString IssueTrackerService::gitHubApiBaseUrl(const QString& rawBaseUrl) {
    const QString u = normalizeBaseUrl(rawBaseUrl);
    if (u.isEmpty()) {
        return QStringLiteral("https://api.github.com");
    }
    const QUrl parsed(u);
    const QString host = parsed.host().toLower();
    if (host == QStringLiteral("github.com") || host == QStringLiteral("api.github.com")) {
        return QStringLiteral("https://api.github.com");
    }
    // Self-hosted GitHub Enterprise Server (GHES)
    if (u.endsWith(QStringLiteral("/api/v3"), Qt::CaseInsensitive)) {
        return u;
    }
    return u + QStringLiteral("/api/v3");
}

QVector<RemoteIssueItem> IssueTrackerService::parseGitHubIssuesJson(const QByteArray& jsonBytes) {
    QVector<RemoteIssueItem> result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    QJsonArray issues;
    if (doc.isObject()) {
        issues = doc.object().value(QStringLiteral("items")).toArray();
    } else if (doc.isArray()) {
        issues = doc.array();
    } else {
        return result;
    }

    result.reserve(issues.size());
    for (const QJsonValue& val : issues) {
        if (!val.isObject()) continue;
        const QJsonObject obj = val.toObject();
        const int number = obj.value(QStringLiteral("number")).toInt();
        if (number <= 0) continue;

        const bool isPr = obj.contains(QStringLiteral("pull_request"));
        const bool isDraft = obj.value(QStringLiteral("draft")).toBool(false);
        const QString state = obj.value(QStringLiteral("state")).toString().trimmed().toLower();
        const QString title = obj.value(QStringLiteral("title")).toString().trimmed();
        const QString body = obj.value(QStringLiteral("body")).toString().trimmed();

        const QString displayKey = isPr ? QStringLiteral("PR#%1").arg(number) : QStringLiteral("GH#%1").arg(number);
        const QString prefix = isPr ? (isDraft ? QStringLiteral("[DRAFT PR] ") : QStringLiteral("[PR] ")) : QString();

        QString fullRepo;
        QString repoTag;
        const QString repoUrl = obj.value(QStringLiteral("repository_url")).toString();
        if (!repoUrl.isEmpty()) {
            const int rIdx = repoUrl.indexOf(QStringLiteral("/repos/"));
            if (rIdx >= 0) {
                fullRepo = repoUrl.mid(rIdx + 7);
            }
            const int idx = repoUrl.lastIndexOf(QLatin1Char('/'));
            if (idx >= 0 && idx + 1 < repoUrl.size()) {
                repoTag = sanitizeTagToken(repoUrl.mid(idx + 1));
            }
        }
        if (fullRepo.isEmpty()) {
            const QString htmlUrl = obj.value(QStringLiteral("html_url")).toString();
            static const QRegularExpression ghRepoRe(QStringLiteral("https?://[^/]+/([^/]+)/([^/]+)"));
            const auto match = ghRepoRe.match(htmlUrl);
            if (match.hasMatch()) {
                fullRepo = match.captured(1) + QLatin1Char('/') + match.captured(2);
                repoTag = sanitizeTagToken(match.captured(2));
            }
        }

        const QString key = !fullRepo.isEmpty()
            ? (isPr ? QStringLiteral("PR#%1#%2").arg(fullRepo).arg(number) : QStringLiteral("GH#%1#%2").arg(fullRepo).arg(number))
            : displayKey;

        quint8 prio = TxtTask::NoPriority;
        QString detectedPriorityName;
        QStringList labelTags;
        const QJsonArray labelsArr = obj.value(QStringLiteral("labels")).toArray();
        for (const QJsonValue& lVal : labelsArr) {
            QString labelName;
            if (lVal.isObject()) {
                labelName = lVal.toObject().value(QStringLiteral("name")).toString().trimmed();
            } else if (lVal.isString()) {
                labelName = lVal.toString().trimmed();
            }
            if (labelName.isEmpty()) continue;

            const quint8 mapped = mapPriorityName(labelName);
            if (mapped != TxtTask::NoPriority && prio == TxtTask::NoPriority) {
                prio = mapped;
                detectedPriorityName = labelName;
            } else if (detectedPriorityName.isEmpty() && (labelName.startsWith(QStringLiteral("p"), Qt::CaseInsensitive) || labelName.contains(QStringLiteral("priority"), Qt::CaseInsensitive))) {
                detectedPriorityName = labelName;
            }
            labelTags.append(QStringLiteral("+") + sanitizeTagToken(labelName));
        }

        QString remoteStatus = state;
        if (isDraft) {
            remoteStatus = QStringLiteral("draft");
        } else if (isPr && !obj.value(QStringLiteral("pull_request")).toObject().value(QStringLiteral("merged_at")).toString().trimmed().isEmpty()) {
            remoteStatus = QStringLiteral("merged");
        }

        QString desc = prefix + displayKey + QLatin1Char(' ') + (title.isEmpty() ? displayKey : title);
        if (!repoTag.isEmpty()) {
            desc += QStringLiteral(" +") + repoTag;
        }
        for (const QString& lt : labelTags) {
            desc += QLatin1Char(' ') + lt;
        }
        if (!remoteStatus.isEmpty()) {
            desc += QStringLiteral(" @") + sanitizeTagToken(remoteStatus);
        }

        RemoteIssueItem item;
        item.key = key;
        item.description = desc;
        item.details = body;
        item.remoteStatus = remoteStatus;
        item.remotePriority = detectedPriorityName;
        item.priority = prio;
        item.durationSeconds = 0;
        result.append(std::move(item));
    }
    return result;
}

QVariantList IssueTrackerService::parseGitHubStatuses(const QVariantMap& existingMapping) {
    const QStringList statuses = {
        QStringLiteral("open"),
        QStringLiteral("closed"),
        QStringLiteral("draft"),
        QStringLiteral("merged")
    };
    QVariantList result;
    for (const QString& st : statuses) {
        result.append(QVariantMap{
            {QStringLiteral("remoteName"), st},
            {QStringLiteral("localStatus"), mapRemoteStatusToLocal(st, {}, existingMapping)}
        });
    }
    return result;
}

struct IssueTrackerService::GitHubSyncState {
    QVector<RemoteIssueItem> allItems;
    int page = 1;
    int pagesFetched = 0;
    int totalCount = -1;
    int initialTotalCount = -1;
    bool limitReached = false;
    bool hasError = false;
    bool explicitZeroTotal = false;
};

void IssueTrackerService::fetchGitHubIssues(const TodoListEntry& entry) {
    const QString apiUrl = gitHubApiBaseUrl(entry.jiraUrl);

    QString queryStr = entry.jiraJql.trimmed();
    if (queryStr.isEmpty()) {
        if (entry.apiToken.trimmed().isEmpty()) {
            const QString errMsg = tr("GitHub sync without an API token requires a repository, user, or organization in the query field (e.g. repo:owner/project, user:username, or org:name).");
            setStatusMessage(errMsg);
            emit syncFailed(entry.id, errMsg);
            return;
        }
        queryStr = QStringLiteral("is:open involves:@me");
    } else {
        if (!queryStr.contains(QStringLiteral("repo:")) && !queryStr.contains(QStringLiteral("org:")) && !queryStr.contains(QStringLiteral("user:"))) {
            QStringList tokens = queryStr.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            for (QString& token : tokens) {
                if (token.contains(QLatin1Char('/')) && !token.contains(QLatin1Char(':'))) {
                    token = QStringLiteral("repo:") + token;
                    break;
                }
            }
            queryStr = tokens.join(QLatin1Char(' '));
        }
        static const QRegularExpression stateRe(QStringLiteral(R"(\b(is|state):(open|closed|merged|unmerged)\b)"), QRegularExpression::CaseInsensitiveOption);
        if (!stateRe.match(queryStr).hasMatch()) {
            queryStr += QStringLiteral(" is:open");
        }
        if (entry.apiToken.trimmed().isEmpty() && !queryStr.contains(QStringLiteral("repo:")) && !queryStr.contains(QStringLiteral("org:")) && !queryStr.contains(QStringLiteral("user:"))) {
            const QString errMsg = tr("GitHub sync without an API token requires a repository, user, or organization in the query field (e.g. repo:owner/project, user:username, or org:name).");
            setStatusMessage(errMsg);
            emit syncFailed(entry.id, errMsg);
            return;
        }
    }

    startSyncOperation();
    setStatusMessage(tr("Syncing issues and pull requests from GitHub..."));

    auto state = std::make_shared<GitHubSyncState>();
    fetchGitHubPage(entry, state, apiUrl, queryStr);
}

void IssueTrackerService::fetchGitHubPage(const TodoListEntry& entry,
                                         const std::shared_ptr<GitHubSyncState>& state,
                                         const QString& apiUrl,
                                         const QString& queryStr)
{
    QUrl url(apiUrl + QStringLiteral("/search/issues"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), queryStr);
    query.addQueryItem(QStringLiteral("sort"), QStringLiteral("updated"));
    query.addQueryItem(QStringLiteral("order"), QStringLiteral("desc"));
    query.addQueryItem(QStringLiteral("per_page"), QStringLiteral("100"));
    if (state->page > 1) {
        query.addQueryItem(QStringLiteral("page"), QString::number(state->page));
    }
    url.setQuery(query);

    QNetworkRequest req(url);
    applyAuthHeader(req, entry);

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry, state, apiUrl, queryStr]() {
        reply->deleteLater();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            if (state->pagesFetched == 0) {
                endSyncOperation();
                const QString errMsg = tr("GitHub sync error (%1): %2").arg(httpStatus).arg(reply->errorString());
                setStatusMessage(errMsg);
                emit syncFailed(entry.id, errMsg);
                return;
            } else {
                state->hasError = true;
                if (state->totalCount >= 0 && state->allItems.size() < state->totalCount) {
                    state->limitReached = true;
                }

                processSyncedItems(entry, state->allItems, QStringLiteral("GitHub"), state->limitReached, state->hasError, state->explicitZeroTotal);
                return;
            }
        }

        const QByteArray bodyBytes = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(bodyBytes);
        bool hasMore = false;

        if (!doc.isObject()) {
            state->hasError = true;
        } else {
            const QJsonObject rootObj = doc.object();
            if (rootObj.contains(QStringLiteral("message"))) {
                state->hasError = true;
            }
            if (rootObj.value(QStringLiteral("incomplete_results")).toBool(false)) {
                state->limitReached = true;
            }
            const int totalCount = rootObj.value(QStringLiteral("total_count")).toInt(-1);
            state->totalCount = totalCount;
            if (state->pagesFetched == 0) {
                state->initialTotalCount = totalCount;
            } else if (state->initialTotalCount >= 0 && totalCount != state->initialTotalCount) {
                state->limitReached = true;
            }
            const QJsonArray itemsArray = rootObj.value(QStringLiteral("items")).toArray();
            if (state->pagesFetched == 0 && totalCount == 0 && !state->hasError && itemsArray.isEmpty()) {
                state->explicitZeroTotal = true;
            }
            if (itemsArray.size() >= 100 && (totalCount < 0 || state->allItems.size() + itemsArray.size() < totalCount)) {
                hasMore = true;
            }
        }

        const QVector<RemoteIssueItem> items = parseGitHubIssuesJson(bodyBytes);
        state->allItems.append(items);
        state->pagesFetched++;
        state->page++;

        if (hasMore && !state->hasError) {
            // GitHub Search API has a hard ceiling of 1000 items (10 pages)
            if (state->pagesFetched < 10) {
                fetchGitHubPage(entry, state, apiUrl, queryStr);
                return;
            } else {
                state->limitReached = true;
            }
        }

        if (state->totalCount >= 0 && state->allItems.size() < state->totalCount) {
            state->limitReached = true;
        }

        processSyncedItems(entry, state->allItems, QStringLiteral("GitHub"), state->limitReached, state->hasError, state->explicitZeroTotal);
    });
}

} // namespace gofi
