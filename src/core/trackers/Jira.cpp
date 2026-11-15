#include "trackers/TrackerCommon.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace gofi {

namespace {

// Extracts Markdown from either a Jira v2 string or Jira v3 Atlassian Document Format (ADF) JSON object
QString extractJiraRichText(const QJsonValue& val) {
    if (val.isString()) {
        return val.toString().trimmed();
    }
    if (val.isArray()) {
        QStringList parts;
        for (const QJsonValue& child : val.toArray()) {
            const QString sub = extractJiraRichText(child);
            if (!sub.isEmpty()) parts.append(sub);
        }
        return parts.join(QString());
    }
    if (val.isObject()) {
        const QJsonObject obj = val.toObject();
        const QString nodeType = obj.value(QStringLiteral("type")).toString();
        if (nodeType == QStringLiteral("text")) {
            QString txt = obj.value(QStringLiteral("text")).toString();
            const QJsonArray marks = obj.value(QStringLiteral("marks")).toArray();
            for (const QJsonValue& mVal : marks) {
                const QJsonObject mObj = mVal.toObject();
                const QString mType = mObj.value(QStringLiteral("type")).toString();
                if (mType == QStringLiteral("code")) {
                    txt = QLatin1Char('`') + txt + QLatin1Char('`');
                } else if (mType == QStringLiteral("strong")) {
                    txt = QStringLiteral("**") + txt + QStringLiteral("**");
                } else if (mType == QStringLiteral("em")) {
                    txt = QLatin1Char('*') + txt + QLatin1Char('*');
                } else if (mType == QStringLiteral("strike")) {
                    txt = QStringLiteral("~~") + txt + QStringLiteral("~~");
                } else if (mType == QStringLiteral("link")) {
                    const QString href = mObj.value(QStringLiteral("attrs")).toObject().value(QStringLiteral("href")).toString();
                    if (!href.isEmpty()) {
                        txt = QStringLiteral("[%1](%2)").arg(txt, href);
                    }
                }
            }
            return txt;
        }
        if (nodeType == QStringLiteral("hardBreak")) {
            return QStringLiteral("\n");
        }
        if (nodeType == QStringLiteral("rule")) {
            return QStringLiteral("\n---\n");
        }
        if (nodeType == QStringLiteral("inlineCard")) {
            const QString url = obj.value(QStringLiteral("attrs")).toObject().value(QStringLiteral("url")).toString();
            return url.isEmpty() ? QString() : QStringLiteral("<%1>").arg(url);
        }
        QString inner;
        if (obj.contains(QStringLiteral("content"))) {
            const QJsonArray contentArr = obj.value(QStringLiteral("content")).toArray();
            QStringList blockParts;
            for (const QJsonValue& child : contentArr) {
                blockParts.append(extractJiraRichText(child));
            }
            if (nodeType == QStringLiteral("doc") || nodeType == QStringLiteral("bulletList") || nodeType == QStringLiteral("orderedList")) {
                inner = blockParts.join(QStringLiteral("\n"));
            } else if (nodeType == QStringLiteral("listItem")) {
                inner = QStringLiteral("- ") + blockParts.join(QString()).trimmed();
            } else if (nodeType == QStringLiteral("blockquote")) {
                QStringList qLines;
                for (const QString& l : blockParts.join(QString()).trimmed().split(QLatin1Char('\n'))) {
                    qLines.append(QStringLiteral("> ") + l);
                }
                inner = qLines.join(QStringLiteral("\n")) + QStringLiteral("\n");
            } else {
                inner = blockParts.join(QString());
            }
        }
        if (nodeType == QStringLiteral("heading")) {
            const int level = qBound(1, obj.value(QStringLiteral("attrs")).toObject().value(QStringLiteral("level")).toInt(2), 6);
            return QString(level, QLatin1Char('#')) + QLatin1Char(' ') + inner.trimmed() + QStringLiteral("\n\n");
        }
        if (nodeType == QStringLiteral("codeBlock")) {
            const QString lang = obj.value(QStringLiteral("attrs")).toObject().value(QStringLiteral("language")).toString();
            return QStringLiteral("```") + lang + QStringLiteral("\n") + inner.trimmed() + QStringLiteral("\n```\n\n");
        }
        if (nodeType == QStringLiteral("paragraph")) {
            return inner + QStringLiteral("\n");
        }
        return inner;
    }
    return {};
}

QJsonArray parseInlineMarkdownToAdfNodes(const QString& line) {
    QJsonArray nodes;
    if (line.isEmpty()) {
        return nodes;
    }
    // Tokenize inline `code`, **bold**, *italic*, and [label](url)
    static const QRegularExpression tokenRe(
        QStringLiteral("(`[^`]+`|\\*\\*[^\\*]+\\*\\*|(?<!\\*)\\*[^\\*]+\\*(?!\\*)|\\[[^\\]]+\\]\\([^\\)]+\\))")
    );
    int lastIdx = 0;
    auto it = tokenRe.globalMatch(line);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        if (m.capturedStart() > lastIdx) {
            nodes.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), line.mid(lastIdx, m.capturedStart() - lastIdx)}
            });
        }
        const QString tok = m.captured(0);
        if (tok.startsWith(QLatin1Char('`')) && tok.endsWith(QLatin1Char('`'))) {
            nodes.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), tok.mid(1, tok.size() - 2)},
                {QStringLiteral("marks"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("code")}}}}
            });
        } else if (tok.startsWith(QStringLiteral("**")) && tok.endsWith(QStringLiteral("**"))) {
            nodes.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), tok.mid(2, tok.size() - 4)},
                {QStringLiteral("marks"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("strong")}}}}
            });
        } else if (tok.startsWith(QLatin1Char('*')) && tok.endsWith(QLatin1Char('*'))) {
            nodes.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), tok.mid(1, tok.size() - 2)},
                {QStringLiteral("marks"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("em")}}}}
            });
        } else if (tok.startsWith(QLatin1Char('['))) {
            const int closeBracket = tok.indexOf(QStringLiteral("]("));
            if (closeBracket > 1 && tok.endsWith(QLatin1Char(')'))) {
                const QString label = tok.mid(1, closeBracket - 1);
                const QString href = tok.mid(closeBracket + 2, tok.size() - closeBracket - 3);
                nodes.append(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), label},
                    {QStringLiteral("marks"), QJsonArray{QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("link")},
                        {QStringLiteral("attrs"), QJsonObject{{QStringLiteral("href"), href}}}
                    }}}
                });
            }
        }
        lastIdx = m.capturedEnd();
    }
    if (lastIdx < line.size()) {
        nodes.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("text")},
            {QStringLiteral("text"), line.mid(lastIdx)}
        });
    }
    return nodes;
}

} // namespace

QJsonObject IssueTrackerService::markdownToJiraAdf(const QString& markdown) {
    QJsonArray docContent;
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    bool inCodeBlock = false;
    QString codeLang;
    QStringList codeLines;
    QJsonArray pendingListItems;

    auto flushList = [&]() {
        if (!pendingListItems.isEmpty()) {
            docContent.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("bulletList")},
                {QStringLiteral("content"), pendingListItems}
            });
            pendingListItems = QJsonArray();
        }
    };

    auto emitCodeBlock = [&]() {
        const QString codeText = codeLines.join(QStringLiteral("\n"));
        QJsonObject cb{
            {QStringLiteral("type"), QStringLiteral("codeBlock")}
        };
        if (!codeText.isEmpty()) {
            cb[QStringLiteral("content")] = QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), codeText}
            }};
        }
        if (!codeLang.isEmpty()) {
            cb[QStringLiteral("attrs")] = QJsonObject{{QStringLiteral("language"), codeLang}};
        }
        docContent.append(cb);
    };

    for (const QString& rawLine : lines) {
        const QString trimmed = rawLine.trimmed();
        if (trimmed.startsWith(QStringLiteral("```"))) {
            flushList();
            if (!inCodeBlock) {
                inCodeBlock = true;
                codeLang = trimmed.mid(3).trimmed();
                codeLines.clear();
            } else {
                inCodeBlock = false;
                emitCodeBlock();
            }
            continue;
        }
        if (inCodeBlock) {
            codeLines.append(rawLine);
            continue;
        }
        if (trimmed.isEmpty()) {
            flushList();
            continue;
        }
        if (trimmed == QStringLiteral("---") || trimmed == QStringLiteral("***")) {
            flushList();
            docContent.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("rule")}});
            continue;
        }
        if (trimmed.startsWith(QLatin1Char('#'))) {
            flushList();
            int level = 0;
            while (level < trimmed.size() && trimmed.at(level) == QLatin1Char('#')) ++level;
            const QString hText = trimmed.mid(level).trimmed();
            docContent.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("heading")},
                {QStringLiteral("attrs"), QJsonObject{{QStringLiteral("level"), qBound(1, level, 6)}}},
                {QStringLiteral("content"), parseInlineMarkdownToAdfNodes(hText)}
            });
            continue;
        }
        if (trimmed.startsWith(QStringLiteral("- ")) || trimmed.startsWith(QStringLiteral("* ")) || trimmed.startsWith(QStringLiteral("• "))) {
            const QString itemText = trimmed.mid(2).trimmed();
            pendingListItems.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("listItem")},
                {QStringLiteral("content"), QJsonArray{QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("paragraph")},
                    {QStringLiteral("content"), parseInlineMarkdownToAdfNodes(itemText)}
                }}}
            });
            continue;
        }
        flushList();
        if (trimmed.startsWith(QStringLiteral("> "))) {
            docContent.append(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("blockquote")},
                {QStringLiteral("content"), QJsonArray{QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("paragraph")},
                    {QStringLiteral("content"), parseInlineMarkdownToAdfNodes(trimmed.mid(2))}
                }}}
            });
            continue;
        }
        docContent.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("paragraph")},
            {QStringLiteral("content"), parseInlineMarkdownToAdfNodes(rawLine)}
        });
    }
    flushList();
    if (inCodeBlock) {
        emitCodeBlock();
        inCodeBlock = false;
    }

    if (docContent.isEmpty()) {
        docContent.append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("paragraph")},
            {QStringLiteral("content"), QJsonArray()}
        });
    }

    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("doc")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("content"), docContent}
    };
}

QVector<RemoteIssueItem> IssueTrackerService::parseJiraIssuesJson(const QByteArray& jsonBytes) {
    QVector<RemoteIssueItem> result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject()) return result;

    const QJsonArray issues = doc.object().value(QStringLiteral("issues")).toArray();
    result.reserve(issues.size());

    for (const QJsonValue& val : issues) {
        const QJsonObject issueObj = val.toObject();
        const QString key = issueObj.value(QStringLiteral("key")).toString().trimmed();
        if (key.isEmpty()) continue;

        const QJsonObject fields = issueObj.value(QStringLiteral("fields")).toObject();
        const QString summary = fields.value(QStringLiteral("summary")).toString().trimmed();
        const QString projectKey = sanitizeTagToken(
            fields.value(QStringLiteral("project")).toObject().value(QStringLiteral("key")).toString()
        );
        const QJsonObject statusObj = fields.value(QStringLiteral("status")).toObject();
        const QString rawStatusName = statusObj.value(QStringLiteral("name")).toString().trimmed();
        const QString statusCategoryKey = statusObj.value(QStringLiteral("statusCategory")).toObject().value(QStringLiteral("key")).toString().trimmed();
        const QString statusTag = sanitizeTagToken(rawStatusName);
        const QString priorityName =
            fields.value(QStringLiteral("priority")).toObject().value(QStringLiteral("name")).toString();
        const quint32 origEstimate =
            fields.value(QStringLiteral("timetracking")).toObject().value(QStringLiteral("originalEstimateSeconds")).toVariant().toUInt();

        QString desc = key + QLatin1Char(' ') + (summary.isEmpty() ? key : summary);
        if (!projectKey.isEmpty()) {
            desc += QStringLiteral(" +") + projectKey;
        }
        if (!statusTag.isEmpty()) {
            desc += QStringLiteral(" @") + statusTag;
        }

        const QString fullDetails = extractJiraRichText(fields.value(QStringLiteral("description"))).trimmed();
        QVariantList parsedComments;
        const QJsonArray commentsArr = fields.value(QStringLiteral("comment")).toObject().value(QStringLiteral("comments")).toArray();
        for (const QJsonValue& cVal : commentsArr) {
            const QJsonObject cObj = cVal.toObject();
            const QJsonObject authorObj = cObj.value(QStringLiteral("author")).toObject();
            QString author = authorObj.value(QStringLiteral("displayName")).toString().trimmed();
            if (author.isEmpty()) author = authorObj.value(QStringLiteral("name")).toString().trimmed();
            const QString created = cObj.value(QStringLiteral("created")).toString().left(16).replace(QLatin1Char('T'), QLatin1Char(' '));
            const QString commentBody = extractJiraRichText(cObj.value(QStringLiteral("body"))).trimmed();
            if (!commentBody.isEmpty()) {
                parsedComments.append(QVariantMap{
                    {QStringLiteral("author"), author.isEmpty() ? QStringLiteral("Jira User") : author},
                    {QStringLiteral("created"), created},
                    {QStringLiteral("body"), commentBody}
                });
            }
        }

        RemoteIssueItem item;
        item.key = key;
        item.description = desc;
        item.details = fullDetails;
        item.comments = parsedComments;
        item.remoteStatus = rawStatusName;
        item.remoteStatusCategory = statusCategoryKey;
        item.remotePriority = priorityName;
        item.priority = mapPriorityName(priorityName);
        item.durationSeconds = origEstimate;
        result.append(std::move(item));
    }
    return result;
}

QVariantList IssueTrackerService::parseJiraStatusesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping) {
    QVariantList result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isArray()) return result;

    QSet<QString> seen;
    for (const QJsonValue& val : doc.array()) {
        const QJsonObject obj = val.toObject();
        const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty() || seen.contains(name.toLower())) continue;
        seen.insert(name.toLower());

        const QString catKey = obj.value(QStringLiteral("statusCategory")).toObject().value(QStringLiteral("key")).toString();
        result.append(QVariantMap{
            {QStringLiteral("remoteName"), name},
            {QStringLiteral("localStatus"), mapRemoteStatusToLocal(name, catKey, existingMapping)}
        });
    }
    return result;
}

QVariantList IssueTrackerService::parseJiraPrioritiesJson(const QByteArray& jsonBytes, const QVariantMap& existingMapping) {
    QVariantList result;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isArray()) return result;

    const auto prioToStr = [](quint8 p) -> QString {
        if (p == 0) return QStringLiteral("A");
        if (p == 1) return QStringLiteral("B");
        if (p == 2) return QStringLiteral("C");
        if (p == 3) return QStringLiteral("D");
        if (p == 4) return QStringLiteral("E");
        return QStringLiteral("none");
    };

    QSet<QString> seen;
    for (const QJsonValue& val : doc.array()) {
        const QJsonObject obj = val.toObject();
        const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty() || seen.contains(name.toLower())) continue;
        seen.insert(name.toLower());

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

struct IssueTrackerService::JiraSyncState {
    QVector<RemoteIssueItem> allItems;
    int startAt = 0;
    QString nextPageToken;
    int pagesFetched = 0;
    int totalCount = -1;
    int initialTotalCount = -1;
    bool limitReached = false;
    bool hasError = false;
    bool explicitZeroTotal = false;
};

void IssueTrackerService::fetchJiraIssues(const TodoListEntry& entry, int endpointStage, bool forceBearerAuth) {
    const QString baseUrl = normalizeBaseUrl(entry.jiraUrl);
    if (baseUrl.isEmpty()) {
        const QString msg = tr("Jira Server URL is empty.");
        setStatusMessage(msg);
        emit syncFailed(entry.id, msg);
        return;
    }

    startSyncOperation();
    setStatusMessage(tr("Syncing issues from Jira..."));

    const QString jql = entry.jiraJql.trimmed().isEmpty()
        ? QStringLiteral("assignee = currentUser() AND resolution = Unresolved ORDER BY id ASC")
        : entry.jiraJql.trimmed();

    const bool isCloud = baseUrl.contains(QStringLiteral(".atlassian.net"), Qt::CaseInsensitive);
    // Stage 0: /rest/api/3/search/jql on Cloud, /rest/api/2/search on Server/DC
    // Stage 1: fallback to /rest/api/2/search on Cloud, or /rest/api/3/search
    QString path;
    if (endpointStage == 0) {
        path = isCloud ? QStringLiteral("/rest/api/3/search/jql") : QStringLiteral("/rest/api/2/search");
    } else if (endpointStage == 1) {
        path = isCloud ? QStringLiteral("/rest/api/2/search") : QStringLiteral("/rest/api/3/search");
    } else {
        path = QStringLiteral("/rest/api/3/search");
    }

    auto state = std::make_shared<JiraSyncState>();
    fetchJiraPage(entry, state, baseUrl, path, jql, endpointStage, forceBearerAuth);
}

void IssueTrackerService::fetchJiraPage(const TodoListEntry& entry,
                                       const std::shared_ptr<JiraSyncState>& state,
                                       const QString& baseUrl,
                                       const QString& path,
                                       const QString& jql,
                                       int endpointStage,
                                       bool forceBearerAuth)
{
    QUrl url(baseUrl + path);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("jql"), jql);
    query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("100"));
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("summary,description,comment,priority,status,project,timetracking"));
    if (!state->nextPageToken.isEmpty()) {
        query.addQueryItem(QStringLiteral("nextPageToken"), state->nextPageToken);
    } else if (state->startAt > 0) {
        query.addQueryItem(QStringLiteral("startAt"), QString::number(state->startAt));
    }
    url.setQuery(query);

    QNetworkRequest req(url);
    applyAuthHeader(req, entry, forceBearerAuth);

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry, state, baseUrl, path, jql, endpointStage, forceBearerAuth]() {
        reply->deleteLater();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray bodyBytes = reply->readAll();
        const QByteArray seraphReason = reply->rawHeader("X-Seraph-Loginreason");
        const bool seraphAuthFailed = seraphReason.contains("AUTHENTICATED_FAILED") || seraphReason.contains("AUTHENTICATION_DENIED");

        if (reply->error() != QNetworkReply::NoError || seraphAuthFailed) {
            if (forceBearerAuth) {
                m_jiraBearerTokens.remove(entry.id);
            }
            if (state->pagesFetched == 0) {
                if (!forceBearerAuth && !entry.jiraUser.trimmed().isEmpty() && (httpStatus == 401 || httpStatus == 403 || seraphAuthFailed)) {
                    endSyncOperation();
                    fetchJiraIssues(entry, endpointStage, true);
                    return;
                }
                if (seraphAuthFailed) {
                    endSyncOperation();
                    const QString errMsg = tr("Jira authentication failed: please verify your Jira Email (e.g. corporate email) and API Token.");
                    setStatusMessage(errMsg);
                    emit syncFailed(entry.id, errMsg);
                    return;
                }
                if (endpointStage < 2 && (httpStatus == 400 || httpStatus == 404 || httpStatus == 405 || httpStatus == 410)) {
                    endSyncOperation();
                    fetchJiraIssues(entry, endpointStage + 1, forceBearerAuth);
                    return;
                }

                endSyncOperation();
                QString serverDetail;
                const QJsonDocument errDoc = QJsonDocument::fromJson(bodyBytes);
                if (errDoc.isObject()) {
                    const QJsonArray errArr = errDoc.object().value(QStringLiteral("errorMessages")).toArray();
                    if (!errArr.isEmpty()) {
                        serverDetail = errArr.first().toString();
                    } else if (errDoc.object().contains(QStringLiteral("message"))) {
                        serverDetail = errDoc.object().value(QStringLiteral("message")).toString();
                    }
                }
                const QString errMsg = serverDetail.isEmpty()
                    ? tr("Jira sync error (%1): %2").arg(httpStatus).arg(reply->errorString())
                    : tr("Jira sync error (%1): %2").arg(httpStatus).arg(serverDetail);
                setStatusMessage(errMsg);
                emit syncFailed(entry.id, errMsg);
                return;
            } else {
                state->hasError = true;
                if (state->totalCount >= 0 && state->allItems.size() < state->totalCount) {
                    state->limitReached = true;
                }

                processSyncedItems(entry, state->allItems, QStringLiteral("Jira"), state->limitReached, state->hasError, state->explicitZeroTotal);
                return;
            }
        }

        if (forceBearerAuth) {
            m_jiraBearerTokens[entry.id] = entry.apiToken;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(bodyBytes);
        bool hasMore = false;
        if (!doc.isObject()) {
            state->hasError = true;
        } else {
            const QJsonObject rootObj = doc.object();
            if (rootObj.contains(QStringLiteral("errorMessages"))) {
                state->hasError = true;
            }
            const int total = rootObj.value(QStringLiteral("total")).toInt(-1);
            state->totalCount = total;
            if (state->pagesFetched == 0) {
                state->initialTotalCount = total;
            } else if (state->initialTotalCount >= 0 && total != state->initialTotalCount) {
                state->limitReached = true;
            }
            const int maxResults = rootObj.value(QStringLiteral("maxResults")).toInt(100);
            const QJsonArray issuesArr = rootObj.value(QStringLiteral("issues")).toArray();
            const bool isLast = rootObj.value(QStringLiteral("isLast")).toBool(true);
            const QString nextToken = rootObj.value(QStringLiteral("nextPageToken")).toString().trimmed();

            if (state->pagesFetched == 0 && !state->hasError && issuesArr.isEmpty()) {
                if (total == 0 || (rootObj.contains(QStringLiteral("isLast")) && rootObj.value(QStringLiteral("isLast")).toBool(false))) {
                    state->explicitZeroTotal = true;
                }
            }

            if (!nextToken.isEmpty()) {
                hasMore = true;
                state->nextPageToken = nextToken;
            } else if (rootObj.contains(QStringLiteral("isLast"))) {
                hasMore = !isLast;
            } else if (total >= 0) {
                hasMore = (state->startAt + issuesArr.size() < total);
            } else {
                hasMore = (issuesArr.size() >= maxResults && !issuesArr.isEmpty());
            }
            state->startAt += issuesArr.size();
        }

        const QVector<RemoteIssueItem> items = parseJiraIssuesJson(bodyBytes);
        state->allItems.append(items);
        state->pagesFetched++;

        if (hasMore && !state->hasError) {
            if (state->pagesFetched < 50) {
                fetchJiraPage(entry, state, baseUrl, path, jql, endpointStage, forceBearerAuth);
                return;
            } else {
                state->limitReached = true;
            }
        }

        if (state->totalCount >= 0 && state->allItems.size() < state->totalCount) {
            state->limitReached = true;
        }

        processSyncedItems(entry, state->allItems, QStringLiteral("Jira"), state->limitReached, state->hasError, state->explicitZeroTotal);
    });
}

} // namespace gofi
