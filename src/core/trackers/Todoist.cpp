#include "trackers/TrackerCommon.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrl>

namespace gofi {

QVector<RemoteIssueItem> IssueTrackerService::parseTodoistTasksJson(const QByteArray& jsonBytes) {
    QVector<RemoteIssueItem> result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isArray()) return result;

    const QJsonArray tasks = doc.array();
    result.reserve(tasks.size());

    for (const QJsonValue& val : tasks) {
        if (!val.isObject()) continue;
        const QJsonObject obj = val.toObject();
        const QString id = obj.value(QStringLiteral("id")).toVariant().toString().trimmed();
        if (id.isEmpty()) continue;

        const QString content = obj.value(QStringLiteral("content")).toString().trimmed();
        const QString description = obj.value(QStringLiteral("description")).toString().trimmed();
        const bool isCompleted = obj.value(QStringLiteral("is_completed")).toBool(false);
        const int priorityNum = obj.value(QStringLiteral("priority")).toInt(1);

        quint8 prio = TxtTask::NoPriority;
        if (priorityNum >= 4) prio = 0;      // (A) - p1
        else if (priorityNum == 3) prio = 1; // (B) - p2
        else if (priorityNum == 2) prio = 2; // (C) - p3

        QStringList labels;
        const QJsonArray labelsArr = obj.value(QStringLiteral("labels")).toArray();
        for (const QJsonValue& lVal : labelsArr) {
            const QString lStr = lVal.toString().trimmed();
            if (!lStr.isEmpty()) {
                labels.append(QStringLiteral("+") + sanitizeTagToken(lStr));
            }
        }

        const QString displayKey = QStringLiteral("TD#%1").arg(id);
        QString desc = displayKey + QLatin1Char(' ') + (content.isEmpty() ? displayKey : content);
        for (const QString& lb : labels) {
            desc += QLatin1Char(' ') + lb;
        }
        desc += isCompleted ? QStringLiteral(" @completed") : QStringLiteral(" @open");

        RemoteIssueItem item;
        item.key = displayKey;
        item.description = desc;
        item.details = description;
        item.remoteStatus = isCompleted ? QStringLiteral("completed") : QStringLiteral("open");
        item.remotePriority = QStringLiteral("Priority %1").arg(priorityNum);
        item.priority = prio;
        item.durationSeconds = 0;
        result.append(std::move(item));
    }
    return result;
}

QVariantList IssueTrackerService::parseTodoistStatuses(const QVariantMap& existingMapping) {
    QVariantList list;
    const QStringList standard = {
        QStringLiteral("open"),
        QStringLiteral("completed")
    };
    for (const QString& s : standard) {
        QVariantMap entry;
        entry[QStringLiteral("remoteName")] = s;
        entry[QStringLiteral("localStatus")] = mapRemoteStatusToLocal(s, {}, existingMapping);
        list.append(entry);
    }
    return list;
}

void IssueTrackerService::fetchTodoistTasks(const TodoListEntry& entry) {
    startSyncOperation();
    setStatusMessage(tr("Syncing tasks from Todoist..."));

    const QString apiUrl = QStringLiteral("https://api.todoist.com/rest/v2/tasks");
    const QString queryStr = entry.jiraJql.trimmed();
    QUrl url(apiUrl);
    if (!queryStr.isEmpty()) {
        url.setQuery(queryStr.startsWith(QLatin1Char('?')) ? queryStr.mid(1) : queryStr);
    }

    QNetworkRequest req(url);
    applyAuthHeader(req, entry);

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            endSyncOperation();
            const QString errMsg = tr("Todoist sync error: %1").arg(reply->errorString());
            setStatusMessage(errMsg);
            emit syncFailed(entry.id, errMsg);
            return;
        }

        const QByteArray bodyBytes = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(bodyBytes);
        const bool hasError = !doc.isArray();
        const QVector<RemoteIssueItem> items = parseTodoistTasksJson(bodyBytes);
        const bool explicitZero = !hasError && items.isEmpty();
        processSyncedItems(entry, items, QStringLiteral("Todoist"), false, hasError, explicitZero);
    });
}

} // namespace gofi
