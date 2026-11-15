#include "trackers/TrackerCommon.h"
#include "DatabaseManager.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace gofi {

QVector<RemoteIssueItem> IssueTrackerService::parseGitLabIssuesJson(const QByteArray& jsonBytes) {
    QVector<RemoteIssueItem> result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isArray()) return result;

    const QJsonArray issues = doc.array();
    result.reserve(issues.size());

    for (const QJsonValue& val : issues) {
        if (!val.isObject()) continue;
        const QJsonObject obj = val.toObject();
        const int iid = obj.value(QStringLiteral("iid")).toInt();
        if (iid <= 0) continue;

        const QString title = obj.value(QStringLiteral("title")).toString().trimmed();
        const QString description = obj.value(QStringLiteral("description")).toString().trimmed();
        const QString state = obj.value(QStringLiteral("state")).toString().trimmed().toLower();

        QString fullProjectPath;
        QString projTag;
        const QString webUrl = obj.value(QStringLiteral("web_url")).toString();
        const int dashIdx = webUrl.indexOf(QStringLiteral("/-/"));
        if (dashIdx > 0) {
            const int schemeIdx = webUrl.indexOf(QStringLiteral("://"));
            const int startSlash = (schemeIdx >= 0) ? webUrl.indexOf(QLatin1Char('/'), schemeIdx + 3) : webUrl.indexOf(QLatin1Char('/'));
            if (startSlash >= 0 && dashIdx > startSlash) {
                fullProjectPath = webUrl.mid(startSlash + 1, dashIdx - startSlash - 1);
            }
            const int lastSlash = webUrl.lastIndexOf(QLatin1Char('/'), dashIdx - 1);
            if (lastSlash >= 0 && dashIdx > lastSlash + 1) {
                projTag = sanitizeTagToken(webUrl.mid(lastSlash + 1, dashIdx - lastSlash - 1));
            }
        }
        const qint64 projId = obj.value(QStringLiteral("project_id")).toVariant().toLongLong();
        const QString projectIdent = !fullProjectPath.isEmpty() ? fullProjectPath : (projId > 0 ? QString::number(projId) : QString());

        const QString displayKey = QStringLiteral("GL#%1").arg(iid);
        const QString key = !projectIdent.isEmpty()
            ? QStringLiteral("GL#%1#%2").arg(projectIdent).arg(iid)
            : displayKey;

        quint8 prio = TxtTask::NoPriority;
        QString detectedPriorityName;
        QStringList labelTags;
        const QJsonArray labels = obj.value(QStringLiteral("labels")).toArray();
        for (const QJsonValue& lVal : labels) {
            const QString lName = lVal.isString() ? lVal.toString().trimmed() : lVal.toObject().value(QStringLiteral("name")).toString().trimmed();
            if (lName.isEmpty()) continue;
            const quint8 mapped = mapPriorityName(lName);
            if (mapped != TxtTask::NoPriority && prio == TxtTask::NoPriority) {
                prio = mapped;
                detectedPriorityName = lName;
            } else if (detectedPriorityName.isEmpty() && (lName.startsWith(QStringLiteral("p"), Qt::CaseInsensitive) || lName.contains(QStringLiteral("priority"), Qt::CaseInsensitive))) {
                detectedPriorityName = lName;
            }
            labelTags.append(QStringLiteral("+") + sanitizeTagToken(lName));
        }

        const quint32 estSec = obj.value(QStringLiteral("time_stats")).toObject().value(QStringLiteral("time_estimate")).toVariant().toUInt();

        QString desc = displayKey + QLatin1Char(' ') + (title.isEmpty() ? displayKey : title);
        if (!projTag.isEmpty()) {
            desc += QStringLiteral(" +") + projTag;
        }
        for (const QString& lt : labelTags) {
            desc += QLatin1Char(' ') + lt;
        }
        if (!state.isEmpty()) {
            desc += QStringLiteral(" @") + sanitizeTagToken(state);
        }

        RemoteIssueItem item;
        item.key = key;
        item.description = desc;
        item.details = description;
        item.remoteStatus = state;
        item.remotePriority = detectedPriorityName;
        item.priority = prio;
        item.durationSeconds = estSec;
        result.append(std::move(item));
    }
    return result;
}

QVector<RemoteIssueItem> IssueTrackerService::parseGitLabMergeRequestsJson(const QByteArray& jsonBytes) {
    QVector<RemoteIssueItem> result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isArray()) return result;

    const QJsonArray mrs = doc.array();
    result.reserve(mrs.size());

    for (const QJsonValue& val : mrs) {
        if (!val.isObject()) continue;
        const QJsonObject obj = val.toObject();
        const int iid = obj.value(QStringLiteral("iid")).toInt();
        if (iid <= 0) continue;

        const QString rawTitle = obj.value(QStringLiteral("title")).toString().trimmed();
        const QString description = obj.value(QStringLiteral("description")).toString().trimmed();
        const QString state = obj.value(QStringLiteral("state")).toString().trimmed().toLower();

        const bool isDraft = obj.value(QStringLiteral("draft")).toBool(false)
            || obj.value(QStringLiteral("work_in_progress")).toBool(false)
            || rawTitle.startsWith(QStringLiteral("Draft:"), Qt::CaseInsensitive)
            || rawTitle.startsWith(QStringLiteral("WIP:"), Qt::CaseInsensitive);

        const QString prefix = isDraft ? QStringLiteral("[DRAFT MR] ") : QStringLiteral("[MR] ");

        QString fullProjectPath;
        QString projTag;
        const QString webUrl = obj.value(QStringLiteral("web_url")).toString();
        const int dashIdx = webUrl.indexOf(QStringLiteral("/-/"));
        if (dashIdx > 0) {
            const int schemeIdx = webUrl.indexOf(QStringLiteral("://"));
            const int startSlash = (schemeIdx >= 0) ? webUrl.indexOf(QLatin1Char('/'), schemeIdx + 3) : webUrl.indexOf(QLatin1Char('/'));
            if (startSlash >= 0 && dashIdx > startSlash) {
                fullProjectPath = webUrl.mid(startSlash + 1, dashIdx - startSlash - 1);
            }
            const int lastSlash = webUrl.lastIndexOf(QLatin1Char('/'), dashIdx - 1);
            if (lastSlash >= 0 && dashIdx > lastSlash + 1) {
                projTag = sanitizeTagToken(webUrl.mid(lastSlash + 1, dashIdx - lastSlash - 1));
            }
        }
        const qint64 projId = obj.value(QStringLiteral("project_id")).toVariant().toLongLong();
        const QString projectIdent = !fullProjectPath.isEmpty() ? fullProjectPath : (projId > 0 ? QString::number(projId) : QString());

        const QString displayKey = QStringLiteral("MR!%1").arg(iid);
        const QString key = !projectIdent.isEmpty()
            ? QStringLiteral("MR!%1!%2").arg(projectIdent).arg(iid)
            : displayKey;

        quint8 prio = TxtTask::NoPriority;
        QString detectedPriorityName;
        QStringList labelTags;
        const QJsonArray labels = obj.value(QStringLiteral("labels")).toArray();
        for (const QJsonValue& lVal : labels) {
            const QString lName = lVal.isString() ? lVal.toString().trimmed() : lVal.toObject().value(QStringLiteral("name")).toString().trimmed();
            if (lName.isEmpty()) continue;
            const quint8 mapped = mapPriorityName(lName);
            if (mapped != TxtTask::NoPriority && prio == TxtTask::NoPriority) {
                prio = mapped;
                detectedPriorityName = lName;
            } else if (detectedPriorityName.isEmpty() && (lName.startsWith(QStringLiteral("p"), Qt::CaseInsensitive) || lName.contains(QStringLiteral("priority"), Qt::CaseInsensitive))) {
                detectedPriorityName = lName;
            }
            labelTags.append(QStringLiteral("+") + sanitizeTagToken(lName));
        }

        const quint32 estSec = obj.value(QStringLiteral("time_stats")).toObject().value(QStringLiteral("time_estimate")).toVariant().toUInt();

        const QString remoteStatus = (isDraft && state == QStringLiteral("opened")) ? QStringLiteral("draft") : state;

        QString desc = prefix + displayKey + QLatin1Char(' ') + (rawTitle.isEmpty() ? displayKey : rawTitle);
        if (!projTag.isEmpty()) {
            desc += QStringLiteral(" +") + projTag;
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
        item.details = description;
        item.remoteStatus = remoteStatus;
        item.remotePriority = detectedPriorityName;
        item.priority = prio;
        item.durationSeconds = estSec;
        result.append(std::move(item));
    }
    return result;
}

QVariantList IssueTrackerService::parseGitLabStatuses(const QVariantMap& existingMapping) {
    const QStringList statuses = {
        QStringLiteral("opened"),
        QStringLiteral("closed"),
        QStringLiteral("merged"),
        QStringLiteral("draft"),
        QStringLiteral("locked")
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

struct IssueTrackerService::GitLabCombinedSyncState {
    QVector<RemoteIssueItem> items;
    int completedRequests = 0;
    int initialTotal = -1;
    int initialIssuesTotal = -1;
    int initialMrTotal = -1;
    int issuesParsedCount = 0;
    int mrParsedCount = 0;
    bool hasError = false;
    QString errorMsg;
    bool limitReached = false;
};

void IssueTrackerService::fetchGitLabIssues(const TodoListEntry& entry) {
    QString baseUrl = normalizeBaseUrl(entry.jiraUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QStringLiteral("https://gitlab.com");
    }

    startSyncOperation();
    setStatusMessage(tr("Syncing issues and merge requests from GitLab..."));

    const QString projectSpec = entry.jiraJql.trimmed();
    QString issuesPath;
    QString mrPath;
    if (projectSpec.startsWith(QStringLiteral("group:"), Qt::CaseInsensitive)) {
        const QString groupName = projectSpec.mid(6).trimmed();
        const QString encodedGroup = QString::fromUtf8(QUrl::toPercentEncoding(groupName));
        issuesPath = QStringLiteral("/api/v4/groups/%1/issues?state=opened&order_by=created_at&sort=asc&per_page=100").arg(encodedGroup);
        mrPath = QStringLiteral("/api/v4/groups/%1/merge_requests?state=opened&order_by=created_at&sort=asc&per_page=100").arg(encodedGroup);
    } else if (!projectSpec.isEmpty() && !projectSpec.contains(QLatin1Char('=')) && !projectSpec.contains(QLatin1Char('&'))) {
        const QString encodedProj = QString::fromUtf8(QUrl::toPercentEncoding(projectSpec));
        issuesPath = QStringLiteral("/api/v4/projects/%1/issues?state=opened&order_by=created_at&sort=asc&per_page=100").arg(encodedProj);
        mrPath = QStringLiteral("/api/v4/projects/%1/merge_requests?state=opened&order_by=created_at&sort=asc&per_page=100").arg(encodedProj);
    } else {
        issuesPath = QStringLiteral("/api/v4/issues?state=opened&scope=assigned_to_me&order_by=created_at&sort=asc&per_page=100");
        mrPath = QStringLiteral("/api/v4/merge_requests?state=opened&scope=assigned_to_me&order_by=created_at&sort=asc&per_page=100");
        if (!projectSpec.isEmpty()) {
            issuesPath += QLatin1Char('&') + projectSpec;
            mrPath += QLatin1Char('&') + projectSpec;
        }
    }

    auto combinedState = std::make_shared<GitLabCombinedSyncState>();
    fetchGitLabStreamPage(entry, combinedState, baseUrl, issuesPath, false, 1);
    fetchGitLabStreamPage(entry, combinedState, baseUrl, mrPath, true, 1);
}

void IssueTrackerService::fetchGitLabStreamPage(const TodoListEntry& entry,
                                                const std::shared_ptr<GitLabCombinedSyncState>& combinedState,
                                                const QString& baseUrl,
                                                const QString& path,
                                                bool isMr,
                                                int page)
{
    QString pageParam = path.contains(QLatin1Char('?'))
        ? QStringLiteral("&page=%1").arg(page)
        : QStringLiteral("?page=%1").arg(page);
    QUrl url(baseUrl + path + pageParam);
    QNetworkRequest req(url);
    applyAuthHeader(req, entry);
    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry, combinedState, baseUrl, path, isMr, page]() {
        reply->deleteLater();

        auto checkDone = [this, entry, combinedState]() {
            if (combinedState->completedRequests < 2) return;
            if (combinedState->hasError && combinedState->items.isEmpty()) {
                endSyncOperation();
                setStatusMessage(combinedState->errorMsg);
                emit syncFailed(entry.id, combinedState->errorMsg);
                return;
            }
            if (combinedState->initialIssuesTotal >= 0 && combinedState->issuesParsedCount < combinedState->initialIssuesTotal) {
                combinedState->limitReached = true;
            }
            if (combinedState->initialMrTotal >= 0 && combinedState->mrParsedCount < combinedState->initialMrTotal) {
                combinedState->limitReached = true;
            }
            const bool explicitZero = !combinedState->hasError && combinedState->items.isEmpty();
            processSyncedItems(entry, combinedState->items, QStringLiteral("GitLab"), combinedState->limitReached, combinedState->hasError, explicitZero);
        };

        if (reply->error() != QNetworkReply::NoError) {
            combinedState->hasError = true;
            if (combinedState->errorMsg.isEmpty()) {
                // GitLab explains rejected requests in the JSON body ({"error": "..."} or {"message": ...});
                // QNetworkReply::errorString() only says "Bad Request".
                QString detail = reply->errorString();
                const QJsonDocument errDoc = QJsonDocument::fromJson(reply->readAll());
                if (errDoc.isObject()) {
                    const QJsonObject errObj = errDoc.object();
                    QString serverMsg = errObj.value(QStringLiteral("error")).toString().trimmed();
                    if (serverMsg.isEmpty()) {
                        const QJsonValue msgVal = errObj.value(QStringLiteral("message"));
                        if (msgVal.isString()) {
                            serverMsg = msgVal.toString().trimmed();
                        } else if (msgVal.isObject() || msgVal.isArray()) {
                            serverMsg = QString::fromUtf8(
                                msgVal.isObject() ? QJsonDocument(msgVal.toObject()).toJson(QJsonDocument::Compact)
                                                  : QJsonDocument(msgVal.toArray()).toJson(QJsonDocument::Compact));
                        }
                    }
                    if (!serverMsg.isEmpty()) {
                        detail += QStringLiteral(" (") + serverMsg + QLatin1Char(')');
                    }
                }
                combinedState->errorMsg = isMr
                    ? tr("GitLab merge requests error: %1").arg(detail)
                    : tr("GitLab issues error: %1").arg(detail);
            }
            combinedState->completedRequests++;
            checkDone();
            return;
        }

        const QByteArray bodyBytes = reply->readAll();
        QJsonParseError parseErr;
        const QJsonDocument doc = QJsonDocument::fromJson(bodyBytes, &parseErr);
        if (parseErr.error != QJsonParseError::NoError || !doc.isArray()) {
            combinedState->hasError = true;
            if (combinedState->errorMsg.isEmpty()) {
                combinedState->errorMsg = isMr
                    ? tr("GitLab merge requests returned invalid response: %1").arg(reply->errorString())
                    : tr("GitLab issues returned invalid response: %1").arg(reply->errorString());
            }
            combinedState->completedRequests++;
            checkDone();
            return;
        }

        const auto parsed = isMr ? parseGitLabMergeRequestsJson(bodyBytes) : parseGitLabIssuesJson(bodyBytes);
        combinedState->items.append(parsed);

        const QByteArray totalHdr = reply->rawHeader("x-total").trimmed();
        const int streamTotal = totalHdr.isEmpty() ? -1 : totalHdr.toInt();
        if (page == 1) {
            if (isMr) {
                combinedState->initialMrTotal = streamTotal;
            } else {
                combinedState->initialIssuesTotal = streamTotal;
            }
        } else {
            const int initial = isMr ? combinedState->initialMrTotal : combinedState->initialIssuesTotal;
            if (initial >= 0) {
                if (streamTotal != initial) {
                    combinedState->limitReached = true;
                }
            } else {
                combinedState->limitReached = true;
            }
        }

        if (isMr) {
            combinedState->mrParsedCount += parsed.size();
        } else {
            combinedState->issuesParsedCount += parsed.size();
        }

        const QByteArray nextPageHdr = reply->rawHeader("x-next-page").trimmed();
        const int nextPage = nextPageHdr.toInt();
        const bool hasMore = (nextPage > 0) || (parsed.size() >= 100);

        if (hasMore && !parsed.isEmpty()) {
            if (page < 50) {
                const int nextP = (nextPage > 0) ? nextPage : (page + 1);
                fetchGitLabStreamPage(entry, combinedState, baseUrl, path, isMr, nextP);
                return;
            } else {
                combinedState->limitReached = true;
            }
        }
        combinedState->completedRequests++;
        checkDone();
    });
}

void IssueTrackerService::postGitLabWorklog(const TodoListEntry& listEntry, qint64 activityLogId, const QString& issueKey, quint32 runtimeSeconds, const QString& baseUrl) {
    const quint32 minutes = qMax(1U, (runtimeSeconds + 30U) / 60U);
    QString proj;
    QString iid;
    bool isMr = false;
    if (issueKey.contains(QLatin1Char('!'))) {
        isMr = true;
        const QStringList parts = issueKey.split(QLatin1Char('!'));
        if (parts.size() >= 3) {
            proj = parts.at(1);
            iid = parts.at(2);
        } else if (parts.size() == 2) {
            iid = parts.at(1);
        }
    } else if (issueKey.contains(QLatin1Char('#'))) {
        const QStringList parts = issueKey.split(QLatin1Char('#'));
        if (parts.size() >= 3) {
            proj = parts.at(1);
            iid = parts.at(2);
        } else if (parts.size() == 2) {
            iid = parts.at(1);
        }
    }
    if (iid.isEmpty()) {
        for (const QChar& c : issueKey) if (c.isDigit()) iid.append(c);
    }
    if (iid.isEmpty()) return;

    const QString resource = isMr ? QStringLiteral("merge_requests") : QStringLiteral("issues");

    if (proj.isEmpty()) {
        proj = listEntry.jiraJql.trimmed();
        if (proj.contains(QLatin1Char('=')) || proj.contains(QLatin1Char('&'))) proj.clear();
    }
    if (proj.isEmpty()) {
        static const QRegularExpression glUrlRe(QStringLiteral("https?://[^/]+/([a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+)"));
        const auto m = glUrlRe.match(baseUrl);
        if (m.hasMatch()) proj = m.captured(1);
    }
    if (proj.isEmpty()) return;

    const QString encodedProj = QString::fromUtf8(QUrl::toPercentEncoding(proj));
    QUrl url(baseUrl + QStringLiteral("/api/v4/projects/%1/%2/%3/add_spent_time?duration=%4m").arg(encodedProj, resource, iid).arg(minutes));
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);

    QNetworkReply* reply = m_nam.post(req, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, activityLogId, issueKey, minutes]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            if (!DatabaseManager::instance().markActivityWorklogSynced(activityLogId)) {
                const QString dbErr = tr("Database error: failed to mark activity worklog as synced for %1.").arg(issueKey);
                setStatusMessage(dbErr);
                emit syncFailed(listEntry.id, dbErr);
            } else {
                setStatusMessage(tr("Logged %1m to GitLab %2").arg(minutes).arg(issueKey));
                emit worklogPosted(issueKey, minutes * 60U);
            }
        } else {
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString errMsg = tr("Failed to post worklog to GitLab (%1): %2")
                .arg(statusCode)
                .arg(reply->errorString());
            setStatusMessage(errMsg);
            emit syncFailed(listEntry.id, errMsg);
        }
    });
}

void IssueTrackerService::updateGitLabDescription(const TodoListEntry& listEntry, const QString& issueKey, const QString& markdownDetails, const QString& baseUrl) {
    QString proj;
    QString iid;
    bool isMr = false;
    if (issueKey.contains(QLatin1Char('!'))) {
        isMr = true;
        const QStringList parts = issueKey.split(QLatin1Char('!'));
        if (parts.size() >= 3) {
            proj = parts.at(1);
            iid = parts.at(2);
        } else if (parts.size() == 2) {
            iid = parts.at(1);
        }
    } else if (issueKey.contains(QLatin1Char('#'))) {
        const QStringList parts = issueKey.split(QLatin1Char('#'));
        if (parts.size() >= 3) {
            proj = parts.at(1);
            iid = parts.at(2);
        } else if (parts.size() == 2) {
            iid = parts.at(1);
        }
    }
    if (iid.isEmpty()) {
        for (const QChar& c : issueKey) if (c.isDigit()) iid.append(c);
    }
    if (iid.isEmpty()) return;

    const QString resource = isMr ? QStringLiteral("merge_requests") : QStringLiteral("issues");

    if (proj.isEmpty()) {
        proj = listEntry.jiraJql.trimmed();
        if (proj.contains(QLatin1Char('=')) || proj.contains(QLatin1Char('&'))) proj.clear();
    }
    if (proj.isEmpty()) {
        static const QRegularExpression glUrlRe(QStringLiteral("https?://[^/]+/([a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+)"));
        const auto m = glUrlRe.match(baseUrl);
        if (m.hasMatch()) proj = m.captured(1);
    }
    if (proj.isEmpty()) return;

    const QString encodedProj = QString::fromUtf8(QUrl::toPercentEncoding(proj));
    QUrl url(baseUrl + QStringLiteral("/api/v4/projects/%1/%2/%3").arg(encodedProj, resource, iid));
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);

    QJsonObject payload{{QStringLiteral("description"), markdownDetails}};
    QNetworkReply* reply = m_nam.put(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, issueKey]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setStatusMessage(tr("Failed to update description on GitLab (%1): %2").arg(issueKey, reply->errorString()));
            return;
        }
        setStatusMessage(tr("Updated description on GitLab %1.").arg(issueKey));
    });
}

void IssueTrackerService::updateGitLabStatusRemote(const TodoListEntry& listEntry, const QString& issueKey, bool done, const QString& baseUrl, const std::function<void(QNetworkReply*)>& handleReply) {
    // Never send status updates for Merge Requests
    if (issueKey.contains(QLatin1Char('!')) ||
        issueKey.startsWith(QStringLiteral("MR#"), Qt::CaseInsensitive) ||
        issueKey.contains(QStringLiteral("#MR#"), Qt::CaseInsensitive)) {
        return;
    }

    QString proj;
    QString iid;
    if (issueKey.contains(QLatin1Char('#'))) {
        const QStringList parts = issueKey.split(QLatin1Char('#'));
        if (parts.size() >= 3) { proj = parts.at(1); iid = parts.at(2); }
        else if (parts.size() == 2) { iid = parts.at(1); }
    }
    if (iid.isEmpty()) {
        for (const QChar& c : issueKey) if (c.isDigit()) iid.append(c);
    }
    if (iid.isEmpty()) return;
    if (proj.isEmpty()) {
        proj = listEntry.jiraJql.trimmed();
        if (proj.startsWith(QStringLiteral("group:"), Qt::CaseInsensitive) ||
            proj.contains(QLatin1Char('=')) || proj.contains(QLatin1Char('&'))) {
            proj.clear();
        }
    }
    if (proj.isEmpty()) {
        static const QRegularExpression glUrlRe(QStringLiteral("https?://[^/]+/([a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+)"));
        const auto m = glUrlRe.match(baseUrl);
        if (m.hasMatch()) proj = m.captured(1);
    }
    if (proj.isEmpty() || proj.startsWith(QStringLiteral("group:"), Qt::CaseInsensitive)) return;

    const QString resource = QStringLiteral("issues");
    const QString encodedProj = QString::fromUtf8(QUrl::toPercentEncoding(proj));
    QUrl url(baseUrl + QStringLiteral("/api/v4/projects/%1/%2/%3").arg(encodedProj, resource, iid));
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);

    QJsonObject payload{{QStringLiteral("state_event"), done ? QStringLiteral("close") : QStringLiteral("reopen")}};
    QNetworkReply* reply = m_nam.put(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    handleReply(reply);
}

void IssueTrackerService::postGitLabComment(const TodoListEntry& listEntry, const QString& issueKey, const QString& markdownComment, const QString& baseUrl) {
    QString proj;
    QString iid;
    bool isMr = false;
    if (issueKey.contains(QLatin1Char('!'))) {
        isMr = true;
        const QStringList parts = issueKey.split(QLatin1Char('!'));
        if (parts.size() >= 3) {
            proj = parts.at(1);
            iid = parts.at(2);
        } else if (parts.size() == 2) {
            iid = parts.at(1);
        }
    } else if (issueKey.contains(QLatin1Char('#'))) {
        const QStringList parts = issueKey.split(QLatin1Char('#'));
        if (parts.size() >= 3) {
            proj = parts.at(1);
            iid = parts.at(2);
        } else if (parts.size() == 2) {
            iid = parts.at(1);
        }
    }
    if (iid.isEmpty()) {
        for (const QChar& c : issueKey) if (c.isDigit()) iid.append(c);
    }
    if (iid.isEmpty()) return;

    const QString resource = isMr ? QStringLiteral("merge_requests") : QStringLiteral("issues");

    if (proj.isEmpty()) {
        proj = listEntry.jiraJql.trimmed();
        if (proj.contains(QLatin1Char('=')) || proj.contains(QLatin1Char('&'))) proj.clear();
    }
    if (proj.isEmpty()) {
        static const QRegularExpression glUrlRe(QStringLiteral("https?://[^/]+/([a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+)"));
        const auto m = glUrlRe.match(baseUrl);
        if (m.hasMatch()) proj = m.captured(1);
    }
    if (proj.isEmpty()) return;

    const QString encodedProj = QString::fromUtf8(QUrl::toPercentEncoding(proj));
    QUrl url(baseUrl + QStringLiteral("/api/v4/projects/%1/%2/%3/notes").arg(encodedProj, resource, iid));
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);

    QJsonObject payload{{QStringLiteral("body"), markdownComment}};
    QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, issueKey]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setStatusMessage(tr("Failed to post comment to GitLab (%1): %2").arg(issueKey, reply->errorString()));
            return;
        }
        setStatusMessage(tr("Posted comment to GitLab %1.").arg(issueKey));
    });
}

QUrl IssueTrackerService::buildGitLabBrowserUrl(const TodoListEntry& listEntry, const QString& issueKey) {
    QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
    if (baseUrl.isEmpty()) {
        baseUrl = QStringLiteral("https://gitlab.com");
    }

    const bool isMr = issueKey.startsWith(QStringLiteral("MR!"), Qt::CaseInsensitive)
                   || issueKey.contains(QLatin1Char('!'));
    QString project;
    QString iid;

    if (issueKey.contains(QLatin1Char('!'))) {
        const QStringList parts = issueKey.split(QLatin1Char('!'));
        if (parts.size() >= 3) {
            project = parts.at(1);
            iid = parts.at(2);
        } else if (parts.size() == 2) {
            if (parts.at(0).contains(QLatin1Char('/'))) {
                project = parts.at(0);
            }
            iid = parts.at(1);
        }
    } else if (issueKey.contains(QLatin1Char('#'))) {
        const QStringList parts = issueKey.split(QLatin1Char('#'));
        if (parts.size() >= 3) {
            project = parts.at(1);
            iid = parts.at(2);
        } else if (parts.size() == 2) {
            if (parts.at(0).contains(QLatin1Char('/'))) {
                project = parts.at(0);
            }
            iid = parts.at(1);
        }
    }

    if (project.isEmpty()) {
        const QString spec = listEntry.jiraJql.trimmed();
        if (!spec.contains(QLatin1Char('=')) && !spec.contains(QLatin1Char('&')) && spec.contains(QLatin1Char('/'))) {
            project = spec;
        }
    }

    if (iid.isEmpty()) {
        for (const QChar& c : issueKey) {
            if (c.isDigit()) iid.append(c);
        }
    }

    if (!project.isEmpty() && !iid.isEmpty()) {
        const QString itemType = isMr ? QStringLiteral("merge_requests") : QStringLiteral("issues");
        return QUrl(QStringLiteral("%1/%2/-/%3/%4").arg(baseUrl, project, itemType, iid));
    }
    if (!baseUrl.isEmpty() && !listEntry.jiraUrl.trimmed().isEmpty()) {
        return QUrl(baseUrl);
    }
    return {};
}

} // namespace gofi
