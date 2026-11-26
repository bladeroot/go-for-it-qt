#include "trackers/TodoistProvider.h"
#include "trackers/TrackerCommon.h"
#include "DatabaseManager.h"
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
        item.detailsFetched = true;
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

void IssueTrackerService::postTodoistWorklog(const TodoListEntry& listEntry, qint64 activityLogId, const QString& issueKey, quint32 runtimeSeconds) {
    QString tdTaskId = issueKey;
    if (tdTaskId.startsWith(QStringLiteral("TD#"))) tdTaskId = tdTaskId.mid(3);
    if (tdTaskId.isEmpty()) return;

    const quint32 minutes = qMax(1U, (runtimeSeconds + 30U) / 60U);
    QUrl url(QStringLiteral("https://api.todoist.com/rest/v2/comments"));
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);

    QJsonObject payload{
        {QStringLiteral("task_id"), tdTaskId},
        {QStringLiteral("content"), QStringLiteral("⏱️ Logged %1m via Go for It Qt Pomodoro timer").arg(minutes)}
    };

    QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, activityLogId, issueKey, minutes]() {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            if (!DatabaseManager::instance().markActivityWorklogSynced(activityLogId)) {
                const QString dbErr = tr("Database error: failed to mark activity worklog as synced for %1.").arg(issueKey);
                setStatusMessage(dbErr);
                emit syncFailed(listEntry.id, dbErr);
            } else {
                setStatusMessage(tr("Logged %1m to Todoist task %2").arg(minutes).arg(issueKey));
                emit worklogPosted(issueKey, minutes * 60U);
            }
        } else {
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString errMsg = tr("Failed to post worklog to Todoist (%1): %2")
                .arg(statusCode)
                .arg(reply->errorString());
            setStatusMessage(errMsg);
            emit syncFailed(listEntry.id, errMsg);
        }
    });
}

void IssueTrackerService::updateTodoistDescription(const TodoListEntry& listEntry, const QString& issueKey, const QString& markdownDetails) {
    QString taskId = issueKey;
    if (taskId.startsWith(QStringLiteral("TD#"))) taskId = taskId.mid(3);
    if (taskId.isEmpty()) return;

    QUrl url(QStringLiteral("https://api.todoist.com/rest/v2/tasks/") + taskId);
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);

    QJsonObject payload{
        {QStringLiteral("description"), markdownDetails}
    };
    QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, issueKey]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setStatusMessage(tr("Failed to update description on Todoist (%1): %2").arg(issueKey, reply->errorString()));
            return;
        }
        setStatusMessage(tr("Updated description on Todoist task %1.").arg(issueKey));
    });
}

void IssueTrackerService::updateTodoistStatusRemote(const TodoListEntry& listEntry, const QString& issueKey, bool done, const std::function<void(QNetworkReply*)>& handleReply) {
    QString tdTaskId = issueKey;
    if (tdTaskId.startsWith(QStringLiteral("TD#"))) tdTaskId = tdTaskId.mid(3);
    if (tdTaskId.isEmpty()) return;

    const QString action = done ? QStringLiteral("close") : QStringLiteral("reopen");
    QUrl url(QStringLiteral("https://api.todoist.com/rest/v2/tasks/") + tdTaskId + QLatin1Char('/') + action);
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);
    QNetworkReply* reply = m_nam.post(req, QByteArray());
    handleReply(reply);
}

void IssueTrackerService::postTodoistComment(const TodoListEntry& listEntry, const QString& issueKey, const QString& markdownComment) {
    QString taskId = issueKey;
    if (taskId.startsWith(QStringLiteral("TD#"))) taskId = taskId.mid(3);
    if (taskId.isEmpty()) return;

    QUrl url(QStringLiteral("https://api.todoist.com/rest/v2/comments"));
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);

    QJsonObject payload{
        {QStringLiteral("task_id"), taskId},
        {QStringLiteral("content"), markdownComment}
    };
    QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, issueKey]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setStatusMessage(tr("Failed to post comment to Todoist (%1): %2").arg(issueKey, reply->errorString()));
            return;
        }
        setStatusMessage(tr("Posted comment to Todoist task %1.").arg(issueKey));
    });
}

QUrl IssueTrackerService::buildTodoistBrowserUrl(const TodoListEntry& /*listEntry*/, const QString& issueKey) {
    QString numStr;
    for (const QChar& c : issueKey) {
        if (c.isDigit()) numStr.append(c);
    }
    if (!numStr.isEmpty()) {
        return QUrl(QStringLiteral("https://todoist.com/app/task/") + numStr);
    }
    return QUrl(QStringLiteral("https://todoist.com/app/today"));
}

void TodoistProvider::syncList(const TodoListEntry& entry) {
    service()->fetchTodoistTasks(entry);
}

void TodoistProvider::postWorklog(const TodoListEntry& entry,
                                  qint64,
                                  qint64 activityLogId,
                                  const QString& issueKey,
                                  const QDateTime&,
                                  quint32 runtimeSeconds,
                                  bool,
                                  const QString&) {
    service()->postTodoistWorklog(entry, activityLogId, issueKey, runtimeSeconds);
}

void TodoistProvider::updateDescription(const TodoListEntry& entry,
                                        const QString& issueKey,
                                        const QString& markdownDetails,
                                        bool,
                                        bool,
                                        const QString&) {
    service()->updateTodoistDescription(entry, issueKey, markdownDetails);
}

void TodoistProvider::updateStatusRemote(const TodoListEntry& entry,
                                         const QString& issueKey,
                                         bool done,
                                         const QString&,
                                         bool,
                                         const QString&,
                                         const StatusReplyHandler& handleReply,
                                         const StatusErrorHandler&) {
    service()->updateTodoistStatusRemote(entry, issueKey, done, handleReply);
}

void TodoistProvider::postComment(const TodoListEntry& entry,
                                  const QString& issueKey,
                                  const QString& markdownComment,
                                  bool,
                                  bool,
                                  const QString&) {
    service()->postTodoistComment(entry, issueKey, markdownComment);
}

void TodoistProvider::fetchRemoteStatuses(const QString&,
                                          const TodoListEntry&,
                                          const QVariantMap& existingMapping) {
    emit service()->remoteStatusesLoaded(IssueTrackerService::parseTodoistStatuses(existingMapping));
}

void TodoistProvider::fetchRemotePriorities(const QString&,
                                            const TodoListEntry&,
                                            const QVariantMap& existingMapping) {
    emit service()->remotePrioritiesLoaded(defaultPriorities(existingMapping));
}

QVariantList TodoistProvider::defaultPriorities(const QVariantMap& existingMapping) const {
    return service()->defaultPrioritiesForProvider(name(), existingMapping);
}

QUrl TodoistProvider::buildBrowserUrl(const TodoListEntry& entry, const QString& issueKey) const {
    return IssueTrackerService::buildTodoistBrowserUrl(entry, issueKey);
}

} // namespace gofi
