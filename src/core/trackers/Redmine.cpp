#include "trackers/TrackerCommon.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrl>

namespace gofi {

QVariantList IssueTrackerService::parseRedminePrioritiesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping) {
    QVariantList result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject()) return result;

    const auto prioToStr = [](quint8 p) -> QString {
        if (p == 0) return QStringLiteral("A");
        if (p == 1) return QStringLiteral("B");
        if (p == 2) return QStringLiteral("C");
        if (p == 3) return QStringLiteral("D");
        if (p == 4) return QStringLiteral("E");
        return QStringLiteral("none");
    };

    const QJsonArray prios = doc.object().value(QStringLiteral("issue_priorities")).toArray();
    for (const QJsonValue& val : prios) {
        const QJsonObject obj = val.toObject();
        const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) continue;
        QString localP = prioToStr(mapPriorityName(name));
        if (existingMapping.contains(name)) {
            localP = existingMapping.value(name).toString();
        }
        result.append(QVariantMap{
            {QStringLiteral("remoteName"), name},
            {QStringLiteral("localPriority"), localP}
        });
    }
    return result;
}

QVector<RemoteIssueItem> IssueTrackerService::parseRedmineIssuesJson(const QByteArray& jsonBytes) {
    QVector<RemoteIssueItem> result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject()) return result;

    const QJsonArray issues = doc.object().value(QStringLiteral("issues")).toArray();
    result.reserve(issues.size());

    for (const QJsonValue& val : issues) {
        if (!val.isObject()) continue;
        const QJsonObject obj = val.toObject();
        const int id = obj.value(QStringLiteral("id")).toInt();
        if (id <= 0) continue;

        const QString subject = obj.value(QStringLiteral("subject")).toString().trimmed();
        const QString description = obj.value(QStringLiteral("description")).toString().trimmed();

        QString statusName;
        const QJsonObject statusObj = obj.value(QStringLiteral("status")).toObject();
        statusName = statusObj.value(QStringLiteral("name")).toString().trimmed();
        const bool isClosed = statusObj.value(QStringLiteral("is_closed")).toBool(false);

        QString projectName;
        QString projectIdent;
        const QJsonObject projObj = obj.value(QStringLiteral("project")).toObject();
        projectName = projObj.value(QStringLiteral("name")).toString().trimmed();
        projectIdent = projObj.value(QStringLiteral("identifier")).toString().trimmed();
        if (projectIdent.isEmpty()) {
            projectIdent = sanitizeTagToken(projectName);
        }

        QString trackerName;
        const QJsonObject trackerObj = obj.value(QStringLiteral("tracker")).toObject();
        trackerName = trackerObj.value(QStringLiteral("name")).toString().trimmed();

        QString priorityName;
        const QJsonObject prioObj = obj.value(QStringLiteral("priority")).toObject();
        priorityName = prioObj.value(QStringLiteral("name")).toString().trimmed();
        const quint8 prio = mapPriorityName(priorityName);

        const double estHours = obj.value(QStringLiteral("estimated_hours")).toDouble(0.0);
        const quint32 durationSeconds = static_cast<quint32>(qMax(0.0, estHours * 3600.0));

        const QString displayKey = QStringLiteral("RM#%1").arg(id);
        const QString key = !projectIdent.isEmpty()
            ? QStringLiteral("RM#%1#%2").arg(projectIdent).arg(id)
            : displayKey;

        QString desc = displayKey + QLatin1Char(' ') + (subject.isEmpty() ? displayKey : subject);
        if (!projectIdent.isEmpty()) {
            desc += QStringLiteral(" +") + sanitizeTagToken(projectIdent);
        }
        if (!trackerName.isEmpty()) {
            desc += QStringLiteral(" +") + sanitizeTagToken(trackerName);
        }
        if (!statusName.isEmpty()) {
            desc += QStringLiteral(" @") + sanitizeTagToken(statusName);
        }

        RemoteIssueItem item;
        item.key = key;
        item.description = desc;
        item.details = description;
        item.remoteStatus = statusName.isEmpty() ? (isClosed ? QStringLiteral("Closed") : QStringLiteral("open")) : statusName;
        if (isClosed) {
            item.remoteStatusCategory = QStringLiteral("done");
        }
        item.remotePriority = priorityName;
        item.priority = prio;
        item.durationSeconds = durationSeconds;
        result.append(std::move(item));
    }
    return result;
}

QVariantList IssueTrackerService::parseRedmineStatusesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping) {
    QVariantList list;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject()) return list;

    const QJsonArray arr = doc.object().value(QStringLiteral("issue_statuses")).toArray();
    for (const QJsonValue& val : arr) {
        if (!val.isObject()) continue;
        const QJsonObject obj = val.toObject();
        const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) continue;
        const bool isClosed = obj.value(QStringLiteral("is_closed")).toBool(false);
        const int id = obj.value(QStringLiteral("id")).toInt(0);

        QVariantMap entry;
        entry[QStringLiteral("remoteName")] = name;
        entry[QStringLiteral("remoteId")] = id;
        entry[QStringLiteral("localStatus")] = mapRemoteStatusToLocal(
            name,
            isClosed ? QStringLiteral("Done") : QString(),
            existingMapping
        );
        list.append(entry);
    }
    return list;
}

struct IssueTrackerService::RedmineSyncState {
    QVector<RemoteIssueItem> allItems;
    int offset = 0;
    int pagesFetched = 0;
    int totalCount = -1;
    int initialTotalCount = -1;
    bool limitReached = false;
    bool hasError = false;
    bool explicitZeroTotal = false;
};

void IssueTrackerService::fetchRedmineIssues(const TodoListEntry& entry) {
    const QString baseUrl = normalizeBaseUrl(entry.jiraUrl);
    if (baseUrl.isEmpty()) {
        const QString msg = tr("Redmine Server URL is empty.");
        setStatusMessage(msg);
        emit syncFailed(entry.id, msg);
        return;
    }

    startSyncOperation();
    setStatusMessage(tr("Syncing issues from Redmine..."));

    QString queryStr = entry.jiraJql.trimmed();
    if (queryStr.isEmpty()) {
        queryStr = QStringLiteral("assigned_to_id=me&status_id=open&limit=100&sort=id:asc");
    } else {
        if (!queryStr.contains(QStringLiteral("limit="))) {
            queryStr += QStringLiteral("&limit=100");
        }
    }
    if (queryStr.startsWith(QLatin1Char('?'))) {
        queryStr.remove(0, 1);
    }

    auto state = std::make_shared<RedmineSyncState>();
    fetchRedminePage(entry, state, baseUrl, queryStr);
}

void IssueTrackerService::fetchRedminePage(const TodoListEntry& entry,
                                          const std::shared_ptr<RedmineSyncState>& state,
                                          const QString& baseUrl,
                                          const QString& queryStr)
{
    QString pagedQuery = queryStr;
    if (state->offset > 0) {
        pagedQuery += QStringLiteral("&offset=%1").arg(state->offset);
    }
    QUrl url(baseUrl + QStringLiteral("/issues.json?") + pagedQuery);
    QNetworkRequest req(url);
    applyAuthHeader(req, entry);

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry, state, baseUrl, queryStr]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (state->pagesFetched == 0) {
                endSyncOperation();
                const QString errMsg = tr("Redmine sync error: %1").arg(reply->errorString());
                setStatusMessage(errMsg);
                emit syncFailed(entry.id, errMsg);
                return;
            } else {
                state->hasError = true;
                if (state->totalCount >= 0 && state->allItems.size() < state->totalCount) {
                    state->limitReached = true;
                }

                processSyncedItems(entry, state->allItems, QStringLiteral("Redmine"), state->limitReached, state->hasError, state->explicitZeroTotal);
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
            const int totalCount = rootObj.value(QStringLiteral("total_count")).toInt(-1);
            state->totalCount = totalCount;
            if (state->pagesFetched == 0) {
                state->initialTotalCount = totalCount;
            } else if (state->initialTotalCount >= 0 && totalCount != state->initialTotalCount) {
                state->limitReached = true;
            }
            const int limit = rootObj.value(QStringLiteral("limit")).toInt(100);
            const QJsonArray issuesArr = rootObj.value(QStringLiteral("issues")).toArray();
            if (state->pagesFetched == 0 && totalCount == 0 && issuesArr.isEmpty()) {
                state->explicitZeroTotal = true;
            }

            if (issuesArr.size() >= limit && (totalCount < 0 || state->allItems.size() + issuesArr.size() < totalCount)) {
                hasMore = true;
            }
            state->offset += issuesArr.size();

            if (state->pagesFetched == 0) {
                // Auto-learn status IDs from Redmine server statuses if closed ID is missing
                const bool statusesFetched = entry.statusMapping.contains(QStringLiteral("_statuses_fetched"));
                bool hasClosedId = statusesFetched;
                if (!hasClosedId) {
                    for (auto it = entry.statusMapping.begin(); it != entry.statusMapping.end(); ++it) {
                        if (it.key().startsWith(QStringLiteral("_id_"))) {
                            const QString stName = it.key().mid(4).toLower();
                            if (stName == QStringLiteral("closed") || stName == QStringLiteral("resolved") || stName == QStringLiteral("done")) {
                                hasClosedId = true;
                                break;
                            }
                        }
                    }
                }
                if (!hasClosedId) {
                    QUrl statusUrl(baseUrl + QStringLiteral("/issue_statuses.json"));
                    QNetworkRequest statusReq(statusUrl);
                    applyAuthHeader(statusReq, entry, false);
                    QNetworkReply* stReply = m_nam.get(statusReq);
                    connect(stReply, &QNetworkReply::finished, this, [this, stReply, entry]() {
                        stReply->deleteLater();
                        if (stReply->error() != QNetworkReply::NoError) return;
                        const QJsonDocument sDoc = QJsonDocument::fromJson(stReply->readAll());
                        if (!sDoc.isObject()) return;
                        const QJsonArray sArr = sDoc.object().value(QStringLiteral("issue_statuses")).toArray();
                        QVariantMap newMapping = entry.statusMapping;
                        bool changed = false;
                        for (const auto& val : sArr) {
                            const QJsonObject sObj = val.toObject();
                            const QString name = sObj.value(QStringLiteral("name")).toString().trimmed();
                            const int id = sObj.value(QStringLiteral("id")).toInt(0);
                            const bool isClosed = sObj.value(QStringLiteral("is_closed")).toBool(false);
                            if (!name.isEmpty() && id > 0) {
                                const QString idKey = QStringLiteral("_id_") + name;
                                if (!newMapping.contains(idKey)) {
                                    newMapping[idKey] = id;
                                    changed = true;
                                }
                                if (isClosed) {
                                    const QString closedKey = QStringLiteral("_is_closed_") + QString::number(id);
                                    if (!newMapping.contains(closedKey)) {
                                        newMapping[closedKey] = 1;
                                        changed = true;
                                    }
                                }
                            }
                        }
                        if (!newMapping.contains(QStringLiteral("_statuses_fetched"))) {
                            newMapping[QStringLiteral("_statuses_fetched")] = 1;
                            changed = true;
                        }
                        if (changed) {
                            emit listStatusMappingUpdated(entry.id, newMapping);
                        }
                    });
                }

                // Auto-learn status IDs from returned issues for existing lists
                bool mappingChanged = false;
                QVariantMap updatedMapping = entry.statusMapping;
                for (const auto& val : issuesArr) {
                    const QJsonObject statusObj = val.toObject().value(QStringLiteral("status")).toObject();
                    const QString sName = statusObj.value(QStringLiteral("name")).toString().trimmed();
                    const int sId = statusObj.value(QStringLiteral("id")).toInt(0);
                    if (!sName.isEmpty() && sId > 0) {
                        const QString idKey = QStringLiteral("_id_") + sName;
                        if (!updatedMapping.contains(idKey)) {
                            updatedMapping[idKey] = sId;
                            mappingChanged = true;
                        }
                    }
                }
                if (mappingChanged) {
                    emit listStatusMappingUpdated(entry.id, updatedMapping);
                }
            }
        }

        const QVector<RemoteIssueItem> items = parseRedmineIssuesJson(bodyBytes);
        state->allItems.append(items);
        state->pagesFetched++;

        if (hasMore && !state->hasError) {
            if (state->pagesFetched < 50) {
                fetchRedminePage(entry, state, baseUrl, queryStr);
                return;
            } else {
                state->limitReached = true;
            }
        }

        if (state->totalCount >= 0 && state->allItems.size() < state->totalCount) {
            state->limitReached = true;
        }

        processSyncedItems(entry, state->allItems, QStringLiteral("Redmine"), state->limitReached, state->hasError, state->explicitZeroTotal);
    });
}

} // namespace gofi
