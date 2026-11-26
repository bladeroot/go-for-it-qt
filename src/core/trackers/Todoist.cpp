#include "trackers/TodoistProvider.h"
#include "trackers/TrackerCommon.h"
#include "trackers/TrackerPaginator.h"
#include "DatabaseManager.h"
#include <QRegularExpression>
#include <QUrlQuery>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QUrl>

namespace gofi {

// Todoist API v1 (REST v2 and Sync v9 were shut down): https://developer.todoist.com/api/v1/
// Task ids are opaque strings; the old numeric REST v2 ids are rejected and are translated
// with /api/v1/id_mappings. Lists are paginated with `cursor` / `next_cursor`.
static QString todoistApiBase(const TodoListEntry& entry) {
    QString base = entry.jiraUrl.trimmed();
    if (base.isEmpty()) base = QStringLiteral("https://api.todoist.com");
    if (!base.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
        && !base.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        base.prepend(QStringLiteral("https://"));
    }
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    return base + QStringLiteral("/api/v1");
}

static QString todoistTaskId(const QString& issueKey) {
    return issueKey.startsWith(QStringLiteral("TD#")) ? issueKey.mid(3) : issueKey;
}

static bool isLegacyTodoistId(const QString& id) {
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    return digits.match(id).hasMatch();
}

QVector<RemoteIssueItem> IssueTrackerService::parseTodoistTasksJson(const QByteArray& jsonBytes) {
    QVector<RemoteIssueItem> result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    QJsonArray tasks;
    if (doc.isObject()) {
        tasks = doc.object().value(QStringLiteral("results")).toArray();  // API v1 page
    } else if (doc.isArray()) {
        tasks = doc.array();  // REST v2 shape, kept for stored samples
    } else {
        return result;
    }
    result.reserve(tasks.size());

    for (const QJsonValue& val : tasks) {
        if (!val.isObject()) continue;
        const QJsonObject obj = val.toObject();
        const QString id = obj.value(QStringLiteral("id")).toVariant().toString().trimmed();
        if (id.isEmpty()) continue;

        const QString content = obj.value(QStringLiteral("content")).toString().trimmed();
        const QString description = obj.value(QStringLiteral("description")).toString().trimmed();
        const bool isCompleted = obj.contains(QStringLiteral("checked"))
            ? obj.value(QStringLiteral("checked")).toBool(false)
            : obj.value(QStringLiteral("is_completed")).toBool(false);
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

struct IssueTrackerService::TodoistIdBatch {
    QString kind;        // id_mappings object type: tasks, projects, sections
    QString queryParam;  // empty: stored task keys; else the list query parameter to replace
    QStringList ids;
};

struct IssueTrackerService::TodoistSyncState {
    QString endpoint;   // tasks or tasks/filter
    QUrlQuery params;   // without cursor / limit
    QVector<RemoteIssueItem> allItems;
    TrackerPaginator paginator{50};
};

void IssueTrackerService::fetchTodoistTasks(const TodoListEntry& entry) {
    startSyncOperation();
    setStatusMessage(tr("Syncing tasks from Todoist..."));

    // The list query keeps its REST v2 form ("filter=today | overdue", "project_id=…", or a bare
    // filter such as "today"): a filter goes to /tasks/filter?query=…, other parameters to /tasks.
    auto state = std::make_shared<TodoistSyncState>();
    QString queryStr = entry.jiraJql.trimmed();
    if (queryStr.startsWith(QLatin1Char('?'))) queryStr.remove(0, 1);
    const QUrlQuery given(queryStr);
    if (!queryStr.isEmpty() && !queryStr.contains(QLatin1Char('='))) {
        state->endpoint = QStringLiteral("tasks/filter");
        state->params.addQueryItem(QStringLiteral("query"), queryStr);
    } else if (given.hasQueryItem(QStringLiteral("filter"))) {
        state->endpoint = QStringLiteral("tasks/filter");
        state->params.addQueryItem(QStringLiteral("query"), given.queryItemValue(QStringLiteral("filter"), QUrl::FullyDecoded));
        if (given.hasQueryItem(QStringLiteral("lang"))) {
            state->params.addQueryItem(QStringLiteral("lang"), given.queryItemValue(QStringLiteral("lang"), QUrl::FullyDecoded));
        }
    } else {
        state->endpoint = QStringLiteral("tasks");
        for (const auto& item : given.queryItems(QUrl::FullyDecoded)) {
            if (item.first == QLatin1String("cursor") || item.first == QLatin1String("limit")) continue;
            state->params.addQueryItem(item.first, item.second);
        }
    }

    migrateTodoistIds(entry, state, [this, entry, state](bool ok, const QString& error) {
        if (!ok) {
            endSyncOperation();
            const QString errMsg = tr("Todoist sync error: %1").arg(error);
            setStatusMessage(errMsg);
            emit syncFailed(entry.id, errMsg);
            return;
        }
        fetchTodoistPage(entry, state, QString());
    });
}

// Old numeric ids (stored keys "TD#<id>", and project/section ids in the list query) are rejected
// by API v1. Translate them once with /id_mappings before the first page, so existing tasks keep
// their rows (timer data, comments) instead of being re-created and the old ones closed.
void IssueTrackerService::migrateTodoistIds(const TodoListEntry& entry,
                                            const std::shared_ptr<TodoistSyncState>& state,
                                            const std::function<void(bool, const QString&)>& done) {
    auto batches = std::make_shared<QList<TodoistIdBatch>>();

    QStringList legacyTaskIds;
    DatabaseManager& db = DatabaseManager::instance();
    for (const bool doneTasks : {false, true}) {
        for (const TxtTask& t : db.loadTasks(entry.id, doneTasks)) {
            const QString id = todoistTaskId(t.jiraKey());
            if (t.jiraKey().startsWith(QStringLiteral("TD#")) && isLegacyTodoistId(id) && !legacyTaskIds.contains(id)) {
                legacyTaskIds.append(id);
            }
        }
    }
    for (int i = 0; i < legacyTaskIds.size(); i += 100) {  // the endpoint takes up to 100 ids
        batches->append({QStringLiteral("tasks"), QString(), legacyTaskIds.mid(i, 100)});
    }
    for (const auto& [param, kind] : {std::pair{QStringLiteral("project_id"), QStringLiteral("projects")},
                                      std::pair{QStringLiteral("section_id"), QStringLiteral("sections")},
                                      std::pair{QStringLiteral("parent_id"), QStringLiteral("tasks")}}) {
        const QString value = state->params.queryItemValue(param);
        if (isLegacyTodoistId(value)) batches->append({kind, param, {value}});
    }
    migrateTodoistIdBatch(entry, state, batches, done);
}

void IssueTrackerService::migrateTodoistIdBatch(const TodoListEntry& entry,
                                                const std::shared_ptr<TodoistSyncState>& state,
                                                const std::shared_ptr<QList<TodoistIdBatch>>& batches,
                                                const std::function<void(bool, const QString&)>& done) {
    if (batches->isEmpty()) {
        done(true, QString());
        return;
    }
    const TodoistIdBatch batch = batches->takeFirst();
    QUrl url(todoistApiBase(entry) + QStringLiteral("/id_mappings/") + batch.kind + QLatin1Char('/') + batch.ids.join(QLatin1Char(',')));
    QNetworkRequest req(url);
    applyAuthHeader(req, entry);
    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry, state, batches, batch, done]() {
        reply->deleteLater();
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (reply->error() != QNetworkReply::NoError || !doc.isArray()) {
            done(false, tr("cannot translate old Todoist ids (%1)").arg(reply->errorString()));
            return;
        }
        QHash<QString, QString> mapped;
        for (const QJsonValue& v : doc.array()) {
            const QJsonObject o = v.toObject();
            const QString oldId = o.value(QStringLiteral("old_id")).toVariant().toString();
            const QString newId = o.value(QStringLiteral("new_id")).toVariant().toString();
            if (!oldId.isEmpty() && !newId.isEmpty()) mapped.insert(oldId, newId);
        }
        if (!batch.queryParam.isEmpty()) {
            // A list query parameter: use the new id for this sync (the stored query stays as typed).
            const QString newId = mapped.value(batch.ids.first());
            if (!newId.isEmpty()) {
                state->params.removeAllQueryItems(batch.queryParam);
                state->params.addQueryItem(batch.queryParam, newId);
            }
        } else {
            // Stored task keys: rename them, all or nothing. Ids without a mapping (deleted tasks)
            // keep their key and are closed by the normal reconciliation.
            DatabaseManager& db = DatabaseManager::instance();
            bool ok = db.beginTransaction();
            for (auto it = mapped.cbegin(); ok && it != mapped.cend(); ++it) {
                ok = db.renameRemoteIssueKey(entry.id, QStringLiteral("TD#") + it.key(), QStringLiteral("TD#") + it.value());
            }
            if (ok) ok = db.commitTransaction();
            if (!ok) {
                db.rollbackTransaction();
                done(false, tr("database error"));
                return;
            }
        }
        migrateTodoistIdBatch(entry, state, batches, done);
    });
}

void IssueTrackerService::fetchTodoistPage(const TodoListEntry& entry,
                                           const std::shared_ptr<TodoistSyncState>& state,
                                           const QString& cursor) {
    QUrl url(todoistApiBase(entry) + QLatin1Char('/') + state->endpoint);
    QUrlQuery query = state->params;
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("200"));
    if (!cursor.isEmpty()) query.addQueryItem(QStringLiteral("cursor"), cursor);
    url.setQuery(query);

    QNetworkRequest req(url);
    applyAuthHeader(req, entry);
    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry, state]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            if (state->paginator.pagesFetched() == 0) {
                endSyncOperation();
                const QString errMsg = tr("Todoist sync error: %1").arg(reply->errorString());
                setStatusMessage(errMsg);
                emit syncFailed(entry.id, errMsg);
                return;
            }
            state->paginator.recordError();
            const auto outcome = state->paginator.finalize(state->allItems);
            processSyncedItems(entry, outcome.items, QStringLiteral("Todoist"), outcome.limitReached, outcome.hasError, outcome.explicitZeroTotal);
            return;
        }

        const QByteArray bodyBytes = reply->readAll();
        const QJsonDocument doc = QJsonDocument::fromJson(bodyBytes);
        const QJsonObject root = doc.object();
        QString nextCursor;
        if (!doc.isObject() || !root.value(QStringLiteral("results")).isArray()) {
            state->paginator.recordError();
        } else {
            const QVector<RemoteIssueItem> items = parseTodoistTasksJson(bodyBytes);
            nextCursor = root.value(QStringLiteral("next_cursor")).toString();
            state->paginator.recordPage(static_cast<int>(items.size()));
            if (state->paginator.pagesFetched() == 1 && items.isEmpty() && nextCursor.isEmpty()) {
                state->paginator.setExplicitZeroTotal(true);
            }
            state->allItems.append(items);
        }

        if (!state->paginator.hasError() && state->paginator.shouldFetchMore(!nextCursor.isEmpty())) {
            fetchTodoistPage(entry, state, nextCursor);
            return;
        }
        if (!nextCursor.isEmpty() && !state->paginator.hasError()) {
            state->paginator.setLimitReached(true);  // stopped at the page cap with more to come
        }
        const auto outcome = state->paginator.finalize(state->allItems);
        processSyncedItems(entry, outcome.items, QStringLiteral("Todoist"), outcome.limitReached, outcome.hasError, outcome.explicitZeroTotal);
    });
}

void IssueTrackerService::postTodoistWorklog(const TodoListEntry& listEntry, qint64 activityLogId, const QString& issueKey, quint32 runtimeSeconds) {
    const QString tdTaskId = todoistTaskId(issueKey);
    if (tdTaskId.isEmpty()) return;

    const quint32 minutes = qMax(1U, (runtimeSeconds + 30U) / 60U);
    QUrl url(todoistApiBase(listEntry) + QStringLiteral("/comments"));
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
    const QString taskId = todoistTaskId(issueKey);
    if (taskId.isEmpty()) return;

    QUrl url(todoistApiBase(listEntry) + QStringLiteral("/tasks/") + taskId);
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
    const QString tdTaskId = todoistTaskId(issueKey);
    if (tdTaskId.isEmpty()) return;

    const QString action = done ? QStringLiteral("close") : QStringLiteral("reopen");
    QUrl url(todoistApiBase(listEntry) + QStringLiteral("/tasks/") + tdTaskId + QLatin1Char('/') + action);
    QNetworkRequest req(url);
    applyAuthHeader(req, listEntry);
    QNetworkReply* reply = m_nam.post(req, QByteArray());
    handleReply(reply);
}

void IssueTrackerService::postTodoistComment(const TodoListEntry& listEntry, const QString& issueKey, const QString& markdownComment) {
    const QString taskId = todoistTaskId(issueKey);
    if (taskId.isEmpty()) return;

    QUrl url(todoistApiBase(listEntry) + QStringLiteral("/comments"));
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
    // API v1 ids are opaque strings (letters and digits); the web app opens both them and old
    // numeric ids under /app/task/<id>.
    static const QRegularExpression idChars(QStringLiteral("^[A-Za-z0-9_-]+$"));
    const QString id = todoistTaskId(issueKey.trimmed());
    if (!id.isEmpty() && idChars.match(id).hasMatch()) {
        return QUrl(QStringLiteral("https://app.todoist.com/app/task/") + id);
    }
    return QUrl(QStringLiteral("https://app.todoist.com/app/today"));
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
