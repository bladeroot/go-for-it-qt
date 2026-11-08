#include "IssueTrackerService.h"
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

QString sanitizeTagToken(const QString& raw) {
    QString out = raw.trimmed();
    static const QRegularExpression wsRe(QStringLiteral("\\s+"));
    out.replace(wsRe, QStringLiteral("_"));
    return out;
}

QString unescapeIcsText(const QString& str) {
    QString out;
    out.reserve(str.size());
    for (int i = 0; i < str.size(); ++i) {
        if (str[i] == QLatin1Char('\\') && i + 1 < str.size()) {
            const QChar next = str[i + 1];
            if (next == QLatin1Char('n') || next == QLatin1Char('N')) {
                out.append(QLatin1Char('\n'));
                ++i;
            } else if (next == QLatin1Char(',') || next == QLatin1Char(';') || next == QLatin1Char('\\')) {
                out.append(next);
                ++i;
            } else {
                out.append(str[i]);
            }
        } else {
            out.append(str[i]);
        }
    }
    return out;
}

QString escapeIcsText(const QString& str) {
    QString out;
    out.reserve(str.size() + 10);
    for (const QChar& c : str) {
        if (c == QLatin1Char('\\')) out += QStringLiteral("\\\\");
        else if (c == QLatin1Char(';')) out += QStringLiteral("\\;");
        else if (c == QLatin1Char(',')) out += QStringLiteral("\\,");
        else if (c == QLatin1Char('\n')) out += QStringLiteral("\\n");
        else if (c == QLatin1Char('\r')) {}
        else out += c;
    }
    return out;
}

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

} // namespace

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

void IssueTrackerService::applyAuthHeader(QNetworkRequest& req, const TodoListEntry& entry, bool forceBearerAuth) {
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("User-Agent", "Go-For-It-Qt");
    req.setTransferTimeout(25000); // 25 seconds timeout to prevent hanging connections

    const QUrl reqUrl = req.url();
    if (reqUrl.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 && !entry.apiToken.trimmed().isEmpty()) {
        const QString host = reqUrl.host();
        if (host != QStringLiteral("localhost") && host != QStringLiteral("127.0.0.1")) {
            qWarning() << "Security warning: Transmitting API token over unencrypted HTTP to" << host;
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
    } else if (!forceBearerAuth && entry.provider == QStringLiteral("Jira") && !user.isEmpty()) {
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

QString IssueTrackerService::mapRemoteStatusToLocal(const QString& remoteStatus,
                                                    const QString& categoryHint,
                                                    const QVariantMap& customMapping)
{
    const QString trimmed = remoteStatus.trimmed();
    if (!trimmed.isEmpty()) {
        if (customMapping.contains(trimmed)) {
            const QString mapped = customMapping.value(trimmed).toString();
            if (!mapped.isEmpty()) return mapped;
        }
        // Case-insensitive lookup in customMapping
        for (auto it = customMapping.begin(); it != customMapping.end(); ++it) {
            if (it.key().compare(trimmed, Qt::CaseInsensitive) == 0 && !it.value().toString().isEmpty()) {
                return it.value().toString();
            }
        }
    }

    const QString s = trimmed.toLower();
    const QString cat = categoryHint.trimmed().toLower();

    // 1. Check Canceled / Rejected keywords first
    if (s.contains(QStringLiteral("cancel")) || s.contains(QStringLiteral("reject"))
        || s.contains(QStringLiteral("won't fix")) || s.contains(QStringLiteral("wontfix"))
        || s.contains(QStringLiteral("duplicate")) || s.contains(QStringLiteral("obsolete"))
        || s.contains(QStringLiteral("invalid")) || s.contains(QStringLiteral("declined"))
        || s.contains(QStringLiteral("скасован")) || s.contains(QStringLiteral("відхилен"))) {
        return QStringLiteral("canceled");
    }

    // 2. Check Paused / On Hold / Blocked / Waiting keywords
    if (s.contains(QStringLiteral("pause")) || s.contains(QStringLiteral("hold"))
        || s.contains(QStringLiteral("block")) || s.contains(QStringLiteral("wait"))
        || s.contains(QStringLiteral("pending")) || s.contains(QStringLiteral("postpone"))
        || s.contains(QStringLiteral("draft")) || s.contains(QStringLiteral("чернетк"))
        || s.contains(QStringLiteral("пауз")) || s.contains(QStringLiteral("очікув"))) {
        return QStringLiteral("paused");
    }

    // 3. Check In Progress / Active / Review / Testing keywords
    if (s.contains(QStringLiteral("progress")) || s.contains(QStringLiteral("review"))
        || s.contains(QStringLiteral("active")) || s.contains(QStringLiteral("working"))
        || s.contains(QStringLiteral("testing")) || s.contains(QStringLiteral("qa"))
        || s.contains(QStringLiteral("verification")) || s.contains(QStringLiteral("робот"))
        || cat == QStringLiteral("indeterminate")) {
        return QStringLiteral("in_progress");
    }

    // 4. Check Done / Resolved / Closed / Verified / Merged
    if (s.contains(QStringLiteral("done")) || s.contains(QStringLiteral("resolved"))
        || s.contains(QStringLiteral("closed")) || s.contains(QStringLiteral("fixed"))
        || s.contains(QStringLiteral("complete")) || s.contains(QStringLiteral("verified"))
        || s.contains(QStringLiteral("merged")) || s.contains(QStringLiteral("злит"))
        || s.contains(QStringLiteral("готово")) || s.contains(QStringLiteral("виконан"))
        || cat == QStringLiteral("done")) {
        return QStringLiteral("done");
    }

    return QStringLiteral("todo");
}

quint8 IssueTrackerService::mapPriorityName(const QString& priorityName) {
    const QString p = priorityName.trimmed().toLower();
    if (p.isEmpty()) return TxtTask::NoPriority;
    if (p.contains(QStringLiteral("highest")) || p.contains(QStringLiteral("blocker")) || p.contains(QStringLiteral("show-stopper"))) {
        return 0; // (A)
    }
    if (p.contains(QStringLiteral("high")) || p.contains(QStringLiteral("critical")) || p.contains(QStringLiteral("major"))) {
        return 1; // (B)
    }
    if (p.contains(QStringLiteral("medium")) || p.contains(QStringLiteral("normal"))) {
        return 2; // (C)
    }
    if (p.contains(QStringLiteral("low")) || p.contains(QStringLiteral("minor"))) {
        return 3; // (D)
    }
    if (p.contains(QStringLiteral("lowest")) || p.contains(QStringLiteral("trivial"))) {
        return 4; // (E)
    }
    return TxtTask::NoPriority;
}

quint8 IssueTrackerService::mapRemotePriorityToLocal(const QString& remotePriority, const QVariantMap& customMapping) {
    const QString p = remotePriority.trimmed();
    if (p.isEmpty()) return TxtTask::NoPriority;

    if (!customMapping.isEmpty()) {
        const auto parseLocalPrio = [](const QString& val) -> quint8 {
            const QString v = val.trimmed().toUpper();
            if (v == QStringLiteral("A") || v == QStringLiteral("0")) return 0;
            if (v == QStringLiteral("B") || v == QStringLiteral("1")) return 1;
            if (v == QStringLiteral("C") || v == QStringLiteral("2")) return 2;
            if (v == QStringLiteral("D") || v == QStringLiteral("3")) return 3;
            if (v == QStringLiteral("E") || v == QStringLiteral("4")) return 4;
            return TxtTask::NoPriority;
        };

        if (customMapping.contains(p)) {
            return parseLocalPrio(customMapping.value(p).toString());
        }

        const QString pLower = p.toLower();
        for (auto it = customMapping.constBegin(); it != customMapping.constEnd(); ++it) {
            if (it.key().trimmed().toLower() == pLower) {
                return parseLocalPrio(it.value().toString());
            }
        }
    }

    return mapPriorityName(p);
}

QVariantList IssueTrackerService::defaultPrioritiesForProvider(const QString& provider, const QVariantMap& existingMapping) const {
    struct DefaultPrio {
        const char* name;
        const char* local;
    };

    QVector<DefaultPrio> presets;
    if (provider == QStringLiteral("Jira")) {
        presets = {
            {"Highest", "A"},
            {"High", "B"},
            {"Medium", "C"},
            {"Low", "D"},
            {"Lowest", "E"}
        };
    } else if (provider == QStringLiteral("YouTrack")) {
        presets = {
            {"Show-stopper", "A"},
            {"Critical", "A"},
            {"Major", "B"},
            {"Normal", "C"},
            {"Minor", "D"}
        };
    } else if (provider == QStringLiteral("GitHub")) {
        presets = {
            {"p0", "A"},
            {"p1", "A"},
            {"priority:critical", "A"},
            {"priority:high", "B"},
            {"p2", "B"},
            {"priority:medium", "C"},
            {"p3", "C"},
            {"priority:low", "D"}
        };
    } else if (provider == QStringLiteral("GitLab")) {
        presets = {
            {"priority::critical", "A"},
            {"priority::high", "B"},
            {"priority::medium", "C"},
            {"priority::low", "D"}
        };
    } else if (provider == QStringLiteral("Redmine")) {
        presets = {
            {"Immediate", "A"},
            {"Urgent", "A"},
            {"High", "B"},
            {"Normal", "C"},
            {"Low", "D"}
        };
    } else if (provider == QStringLiteral("Todoist")) {
        presets = {
            {"Priority 4 (P1 Urgent)", "A"},
            {"Priority 3 (P2 High)", "B"},
            {"Priority 2 (P3 Medium)", "C"},
            {"Priority 1 (P4 Normal)", "none"}
        };
    } else if (provider == QStringLiteral("Nextcloud")) {
        presets = {
            {"1 (Highest)", "A"},
            {"2-4 (High)", "B"},
            {"5 (Medium)", "C"},
            {"6-8 (Low)", "D"},
            {"9 (Lowest)", "E"}
        };
    }

    QVariantList result;
    QSet<QString> seen;

    for (const auto& dp : presets) {
        const QString name = QString::fromUtf8(dp.name);
        seen.insert(name.toLower());
        QString localP = QString::fromUtf8(dp.local);
        if (existingMapping.contains(name)) {
            localP = existingMapping.value(name).toString();
        } else {
            for (auto it = existingMapping.constBegin(); it != existingMapping.constEnd(); ++it) {
                if (it.key().trimmed().toLower() == name.toLower()) {
                    localP = it.value().toString();
                    break;
                }
            }
        }
        result.append(QVariantMap{
            {QStringLiteral("remoteName"), name},
            {QStringLiteral("localPriority"), localP}
        });
    }

    for (auto it = existingMapping.constBegin(); it != existingMapping.constEnd(); ++it) {
        if (!seen.contains(it.key().trimmed().toLower())) {
            result.append(QVariantMap{
                {QStringLiteral("remoteName"), it.key()},
                {QStringLiteral("localPriority"), it.value().toString()}
            });
        }
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
        emit remotePrioritiesLoaded(defaultPrioritiesForProvider(provider, existingMapping));
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
        connect(reply, &QNetworkReply::finished, this, [this, reply, provider, existingMapping]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                const QVariantList list = parseJiraPrioritiesJson(reply->readAll(), existingMapping);
                if (!list.isEmpty()) {
                    emit remotePrioritiesLoaded(list);
                    return;
                }
            }
            emit remotePrioritiesLoaded(defaultPrioritiesForProvider(provider, existingMapping));
        });
    } else if (provider == QStringLiteral("Redmine")) {
        QUrl url(baseUrl + QStringLiteral("/enumerations/issue_priorities.json"));
        QNetworkRequest req(url);
        applyAuthHeader(req, tempEntry);

        QNetworkReply* reply = m_nam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, provider, existingMapping]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                const QVariantList list = parseRedminePrioritiesJson(reply->readAll(), existingMapping);
                if (!list.isEmpty()) {
                    emit remotePrioritiesLoaded(list);
                    return;
                }
            }
            emit remotePrioritiesLoaded(defaultPrioritiesForProvider(provider, existingMapping));
        });
    } else {
        emit remotePrioritiesLoaded(defaultPrioritiesForProvider(provider, existingMapping));
    }
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
        } else if (isPr && !obj.value(QStringLiteral("pull_request")).toObject().value(QStringLiteral("merged_at")).isNull()) {
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
        item.remoteStatus = isClosed ? QStringLiteral("Closed") : (statusName.isEmpty() ? QStringLiteral("open") : statusName);
        item.remotePriority = priorityName;
        item.priority = prio;
        item.durationSeconds = durationSeconds;
        result.append(std::move(item));
    }
    return result;
}

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

QVector<RemoteIssueItem> IssueTrackerService::parseNextcloudTasksIcs(const QByteArray& rawBytes) {
    QVector<RemoteIssueItem> result;
    if (rawBytes.isEmpty()) return result;

    QString text = QString::fromUtf8(rawBytes);

    // Unfold iCalendar text (RFC 5545 section 3.1: CRLF followed by space or tab)
    static const QRegularExpression unfoldRe(QStringLiteral("(\\r?\\n[ \\t])"));
    text.replace(unfoldRe, QString());

    // Search for all BEGIN:VTODO ... END:VTODO blocks
    static const QRegularExpression vtodoRe(
        QStringLiteral("BEGIN:VTODO\\b([\\s\\S]*?)END:VTODO\\b"),
        QRegularExpression::CaseInsensitiveOption
    );

    auto it = vtodoRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString block = match.captured(1);

        QString uid;
        QString summary;
        QString description;
        QString statusStr;
        int priorityNum = 0;
        QStringList categories;
        quint32 durationSeconds = 0;
        QVariantList comments;

        const QStringList lines = block.split(QLatin1Char('\n'));
        for (QString line : lines) {
            line = line.trimmed();
            if (line.isEmpty()) continue;

            const int colonIdx = line.indexOf(QLatin1Char(':'));
            if (colonIdx <= 0) continue;

            const QString rawProp = line.left(colonIdx);
            const QString val = line.mid(colonIdx + 1);

            const int semiIdx = rawProp.indexOf(QLatin1Char(';'));
            const QString propName = (semiIdx >= 0 ? rawProp.left(semiIdx) : rawProp).toUpper().trimmed();

            if (propName == QStringLiteral("UID")) {
                uid = val.trimmed();
            } else if (propName == QStringLiteral("SUMMARY")) {
                summary = unescapeIcsText(val.trimmed());
            } else if (propName == QStringLiteral("DESCRIPTION")) {
                description = unescapeIcsText(val.trimmed());
            } else if (propName == QStringLiteral("STATUS")) {
                statusStr = val.trimmed().toLower();
            } else if (propName == QStringLiteral("PRIORITY")) {
                priorityNum = val.trimmed().toInt();
            } else if (propName == QStringLiteral("CATEGORIES")) {
                const QStringList cats = val.split(QLatin1Char(','));
                for (const QString& c : cats) {
                    const QString trimmedCat = c.trimmed();
                    if (!trimmedCat.isEmpty()) {
                        categories.append(sanitizeTagToken(trimmedCat));
                    }
                }
            } else if (propName == QStringLiteral("COMMENT")) {
                const QString cmt = unescapeIcsText(val.trimmed());
                if (!cmt.isEmpty()) {
                    comments.append(QJsonObject{
                        {QStringLiteral("author"), QStringLiteral("Nextcloud")},
                        {QStringLiteral("body"), cmt}
                    });
                }
            } else if (propName == QStringLiteral("DURATION") || propName == QStringLiteral("ESTIMATED-DURATION")) {
                static const QRegularExpression durRe(QStringLiteral("PT(?:(\\d+)H)?(?:(\\d+)M)?(?:(\\d+)S)?"), QRegularExpression::CaseInsensitiveOption);
                const auto dm = durRe.match(val);
                if (dm.hasMatch()) {
                    const quint32 h = dm.captured(1).toUInt();
                    const quint32 m = dm.captured(2).toUInt();
                    const quint32 s = dm.captured(3).toUInt();
                    durationSeconds = h * 3600U + m * 60U + s;
                }
            }
        }

        if (uid.isEmpty()) continue;

        quint8 prio = TxtTask::NoPriority;
        if (priorityNum >= 1 && priorityNum <= 4) prio = 0;      // (A)
        else if (priorityNum == 5) prio = 1;                     // (B)
        else if (priorityNum >= 6 && priorityNum <= 9) prio = 2; // (C)

        const QString shortUid = uid.contains(QLatin1Char('-')) ? uid.left(8) : uid;
        const QString displayKey = QStringLiteral("NC#%1").arg(shortUid);
        const QString key = QStringLiteral("NC#%1").arg(uid);

        QString desc = displayKey + QLatin1Char(' ') + (summary.isEmpty() ? displayKey : summary);
        for (const QString& cat : categories) {
            desc += QStringLiteral(" +") + cat;
        }
        if (!statusStr.isEmpty()) {
            desc += QStringLiteral(" @") + sanitizeTagToken(statusStr);
        }

        RemoteIssueItem item;
        item.key = key;
        item.description = desc;
        item.details = description;
        item.comments = comments;
        item.remoteStatus = statusStr.isEmpty() ? QStringLiteral("needs-action") : statusStr;
        item.remotePriority = priorityNum > 0 ? QString::number(priorityNum) : QString();
        item.priority = prio;
        item.durationSeconds = durationSeconds;
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

        QVariantMap entry;
        entry[QStringLiteral("remoteName")] = name;
        entry[QStringLiteral("localStatus")] = mapRemoteStatusToLocal(
            name,
            isClosed ? QStringLiteral("Done") : QString(),
            existingMapping
        );
        list.append(entry);
    }
    return list;
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

QVariantList IssueTrackerService::parseNextcloudStatuses(const QVariantMap& existingMapping) {
    QVariantList list;
    const QStringList standard = {
        QStringLiteral("needs-action"),
        QStringLiteral("in-process"),
        QStringLiteral("completed"),
        QStringLiteral("cancelled")
    };
    for (const QString& s : standard) {
        QVariantMap entry;
        entry[QStringLiteral("remoteName")] = s;
        entry[QStringLiteral("localStatus")] = mapRemoteStatusToLocal(s, {}, existingMapping);
        list.append(entry);
    }
    return list;
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
    } else if (provider == QStringLiteral("Redmine")) {
        QUrl url(baseUrl + QStringLiteral("/issue_statuses.json"));
        QNetworkRequest req(url);
        applyAuthHeader(req, tempEntry, false);
        QNetworkReply* reply = m_nam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, existingMapping]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                emit remoteStatusesError(tr("Failed to load Redmine statuses: %1").arg(reply->errorString()));
                return;
            }
            emit remoteStatusesLoaded(parseRedmineStatusesJson(reply->readAll(), existingMapping));
        });
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
        ? QStringLiteral("assignee = currentUser() ORDER BY updated DESC")
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

    QUrl url(baseUrl + path);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("jql"), jql);
    query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("100"));
    query.addQueryItem(QStringLiteral("fields"), QStringLiteral("summary,description,comment,priority,status,project,timetracking"));
    url.setQuery(query);

    QNetworkRequest req(url);
    applyAuthHeader(req, entry, forceBearerAuth);

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry, endpointStage, forceBearerAuth]() {
        reply->deleteLater();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray bodyBytes = reply->readAll();
        const QByteArray seraphReason = reply->rawHeader("X-Seraph-Loginreason");
        const bool seraphAuthFailed = seraphReason.contains("AUTHENTICATED_FAILED") || seraphReason.contains("AUTHENTICATION_DENIED");

        if (reply->error() != QNetworkReply::NoError || seraphAuthFailed) {
            // 1. If Basic Auth failed with 401/403 or X-Seraph-Loginreason: AUTHENTICATED_FAILED, retry with Bearer PAT
            if (!forceBearerAuth && !entry.jiraUser.trimmed().isEmpty() && (httpStatus == 401 || httpStatus == 403 || seraphAuthFailed)) {
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
            // 2. If endpoint returned 400/404/405/410, try fallback search endpoint
            if (endpointStage < 2 && (httpStatus == 400 || httpStatus == 404 || httpStatus == 405 || httpStatus == 410)) {
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
        }

        const QVector<RemoteIssueItem> items = parseJiraIssuesJson(bodyBytes);
        processSyncedItems(entry, items, QStringLiteral("Jira"));
    });
}

void IssueTrackerService::fetchYouTrackIssues(const TodoListEntry& entry) {
    const QString baseUrl = normalizeBaseUrl(entry.jiraUrl);
    if (baseUrl.isEmpty()) {
        emit syncFailed(entry.id, tr("YouTrack Server URL is empty."));
        return;
    }

    startSyncOperation();
    setStatusMessage(tr("Syncing issues from YouTrack..."));

    const QString ytQuery = entry.jiraJql.trimmed().isEmpty()
        ? QStringLiteral("for: me sort by: updated desc")
        : entry.jiraJql.trimmed();

    QUrl url(baseUrl + QStringLiteral("/api/issues"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("query"), ytQuery);
    query.addQueryItem(QStringLiteral("$top"), QStringLiteral("100"));
    query.addQueryItem(
        QStringLiteral("fields"),
        QStringLiteral("idReadable,summary,description,project(shortName),customFields(name,value(name,isResolved,minutes)),comments(author(fullName,login),created,text)")
    );
    url.setQuery(query);

    QNetworkRequest req(url);
    applyAuthHeader(req, entry);

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry]() {
        reply->deleteLater();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError) {
            endSyncOperation();
            const QString errMsg = tr("YouTrack sync error (%1): %2").arg(httpStatus).arg(reply->errorString());
            setStatusMessage(errMsg);
            emit syncFailed(entry.id, errMsg);
            return;
        }

        const QVector<RemoteIssueItem> items = parseYouTrackIssuesJson(reply->readAll());
        processSyncedItems(entry, items, QStringLiteral("YouTrack"));
    });
}

void IssueTrackerService::processSyncedItems(const TodoListEntry& entry,
                                             const QVector<RemoteIssueItem>& items,
                                             const QString& providerName)
{
    DatabaseManager& db = DatabaseManager::instance();
    QStringList newIssues;
    QStringList statusChanges;
    for (const RemoteIssueItem& item : items) {
        const QString localStatus = mapRemoteStatusToLocal(
            item.remoteStatus, item.remoteStatusCategory, entry.statusMapping
        );
        quint8 localPriority = item.priority;
        if (!entry.priorityMapping.isEmpty()) {
            localPriority = mapRemotePriorityToLocal(item.remotePriority, entry.priorityMapping);
        }
        bool isNew = false;
        QString oldStatus;
        db.upsertRemoteIssueTask(
            entry.id, item.key, item.description, localStatus, localPriority,
            item.durationSeconds, item.details, item.comments, &isNew, &oldStatus
        );
        if (isNew) {
            newIssues.append(item.description);
        } else if (!oldStatus.isEmpty() && oldStatus != localStatus) {
            statusChanges.append(QStringLiteral("%1: %2 → %3").arg(item.key, oldStatus, localStatus));
        }
    }

    endSyncOperation();
    if (items.size() >= 100) {
        setStatusMessage(tr("Synced %1 issues from %2 (maximum page limit reached; older items omitted).").arg(items.size()).arg(providerName));
    } else {
        setStatusMessage(tr("Synced %1 issues from %2.").arg(items.size()).arg(providerName));
    }
    emit listIssuesSynced(entry.id, static_cast<int>(items.size()));
    if (!newIssues.isEmpty() || !statusChanges.isEmpty()) {
        emit trackerChangesDetected(entry.name, newIssues, statusChanges);
    }
}

void IssueTrackerService::fetchGitHubIssues(const TodoListEntry& entry) {
    const QString apiUrl = gitHubApiBaseUrl(entry.jiraUrl);

    startSyncOperation();
    setStatusMessage(tr("Syncing issues and pull requests from GitHub..."));

    QString queryStr = entry.jiraJql.trimmed();
    if (queryStr.isEmpty()) {
        queryStr = entry.apiToken.trimmed().isEmpty() ? QStringLiteral("sort:updated-desc") : QStringLiteral("involves:@me");
    } else if (!queryStr.contains(QStringLiteral("is:")) && !queryStr.contains(QStringLiteral("repo:")) && !queryStr.contains(QStringLiteral("org:"))) {
        if (queryStr.contains(QLatin1Char('/'))) {
            queryStr = QStringLiteral("repo:") + queryStr;
        }
    }

    QUrl url(apiUrl + QStringLiteral("/search/issues"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("q"), queryStr);
    query.addQueryItem(QStringLiteral("sort"), QStringLiteral("updated"));
    query.addQueryItem(QStringLiteral("order"), QStringLiteral("desc"));
    query.addQueryItem(QStringLiteral("per_page"), QStringLiteral("100"));
    url.setQuery(query);

    QNetworkRequest req(url);
    applyAuthHeader(req, entry);

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry]() {
        reply->deleteLater();
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            endSyncOperation();
            const QString errMsg = tr("GitHub sync error (%1): %2").arg(httpStatus).arg(reply->errorString());
            setStatusMessage(errMsg);
            emit syncFailed(entry.id, errMsg);
            return;
        }

        const QVector<RemoteIssueItem> items = parseGitHubIssuesJson(reply->readAll());
        processSyncedItems(entry, items, QStringLiteral("GitHub"));
    });
}

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
    if (!projectSpec.isEmpty() && !projectSpec.contains(QLatin1Char('=')) && !projectSpec.contains(QLatin1Char('&'))) {
        const QString encodedProj = QString::fromUtf8(QUrl::toPercentEncoding(projectSpec));
        issuesPath = QStringLiteral("/api/v4/projects/%1/issues?state=all&order_by=updated_at&sort=desc&per_page=100").arg(encodedProj);
        mrPath = QStringLiteral("/api/v4/projects/%1/merge_requests?state=all&order_by=updated_at&sort=desc&per_page=100").arg(encodedProj);
    } else {
        issuesPath = QStringLiteral("/api/v4/issues?state=all&scope=all&order_by=updated_at&sort=desc&per_page=100");
        mrPath = QStringLiteral("/api/v4/merge_requests?state=all&scope=all&order_by=updated_at&sort=desc&per_page=100");
        if (!projectSpec.isEmpty()) {
            issuesPath += QLatin1Char('&') + projectSpec;
            mrPath += QLatin1Char('&') + projectSpec;
        }
    }

    struct CombinedSyncState {
        QVector<RemoteIssueItem> items;
        int completedRequests = 0;
        bool hasError = false;
        QString errorMsg;
    };
    auto combinedState = std::make_shared<CombinedSyncState>();

    auto checkDone = [this, entry, combinedState]() {
        if (combinedState->completedRequests < 2) return;
        if (combinedState->hasError && combinedState->items.isEmpty()) {
            endSyncOperation();
            setStatusMessage(combinedState->errorMsg);
            emit syncFailed(entry.id, combinedState->errorMsg);
            return;
        }
        processSyncedItems(entry, combinedState->items, QStringLiteral("GitLab"));
    };

    // 1. Fetch Issues
    {
        QUrl url(baseUrl + issuesPath);
        QNetworkRequest req(url);
        applyAuthHeader(req, entry);
        QNetworkReply* reply = m_nam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, combinedState, checkDone]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                combinedState->hasError = true;
                combinedState->errorMsg = tr("GitLab issues error: %1").arg(reply->errorString());
            } else {
                combinedState->items.append(parseGitLabIssuesJson(reply->readAll()));
            }
            combinedState->completedRequests++;
            checkDone();
        });
    }

    // 2. Fetch Merge Requests
    {
        QUrl url(baseUrl + mrPath);
        QNetworkRequest req(url);
        applyAuthHeader(req, entry);
        QNetworkReply* reply = m_nam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, combinedState, checkDone]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                combinedState->hasError = true;
                if (combinedState->errorMsg.isEmpty()) {
                    combinedState->errorMsg = tr("GitLab merge requests error: %1").arg(reply->errorString());
                }
            } else {
                combinedState->items.append(parseGitLabMergeRequestsJson(reply->readAll()));
            }
            combinedState->completedRequests++;
            checkDone();
        });
    }
}

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
        queryStr = QStringLiteral("assigned_to_id=me&status_id=*&limit=100&sort=updated_on:desc");
    } else {
        if (!queryStr.contains(QStringLiteral("limit="))) {
            queryStr += QStringLiteral("&limit=100");
        }
    }
    if (queryStr.startsWith(QLatin1Char('?'))) {
        queryStr.remove(0, 1);
    }

    QUrl url(baseUrl + QStringLiteral("/issues.json?") + queryStr);
    QNetworkRequest req(url);
    applyAuthHeader(req, entry);

    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            endSyncOperation();
            const QString errMsg = tr("Redmine sync error: %1").arg(reply->errorString());
            setStatusMessage(errMsg);
            emit syncFailed(entry.id, errMsg);
            return;
        }

        const QVector<RemoteIssueItem> items = parseRedmineIssuesJson(reply->readAll());
        processSyncedItems(entry, items, QStringLiteral("Redmine"));
    });
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

        const QVector<RemoteIssueItem> items = parseTodoistTasksJson(reply->readAll());
        processSyncedItems(entry, items, QStringLiteral("Todoist"));
    });
}

void IssueTrackerService::fetchNextcloudTasks(const TodoListEntry& entry) {
    QString urlStr = entry.jiraUrl.trimmed();
    if (urlStr.isEmpty()) {
        const QString msg = tr("Nextcloud CalDAV URL is empty.");
        setStatusMessage(msg);
        emit syncFailed(entry.id, msg);
        return;
    }

    startSyncOperation();
    setStatusMessage(tr("Syncing tasks from Nextcloud (CalDAV)..."));

    if (!urlStr.contains(QStringLiteral("/dav/calendars/")) && !urlStr.endsWith(QStringLiteral(".ics"))) {
        QString base = normalizeBaseUrl(urlStr);
        const QString user = entry.jiraUser.trimmed();
        const QString cal = entry.jiraJql.trimmed().isEmpty() ? QStringLiteral("tasks") : entry.jiraJql.trimmed();
        if (!user.isEmpty()) {
            urlStr = base + QStringLiteral("/remote.php/dav/calendars/%1/%2/").arg(user, cal);
        }
    }
    if (!urlStr.endsWith(QLatin1Char('/')) && !urlStr.endsWith(QStringLiteral(".ics"))) {
        urlStr += QLatin1Char('/');
    }

    QUrl url(urlStr);
    QNetworkRequest req(url);
    applyAuthHeader(req, entry);

    if (urlStr.endsWith(QStringLiteral(".ics"))) {
        QNetworkReply* reply = m_nam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, entry]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                endSyncOperation();
                const QString errMsg = tr("Nextcloud sync error: %1").arg(reply->errorString());
                setStatusMessage(errMsg);
                emit syncFailed(entry.id, errMsg);
                return;
            }
            const QVector<RemoteIssueItem> items = parseNextcloudTasksIcs(reply->readAll());
            processSyncedItems(entry, items, QStringLiteral("Nextcloud"));
        });
        return;
    }

    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml; charset=utf-8"));
    req.setRawHeader("Depth", "1");

    const QByteArray reportXml = QByteArray(
        "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">\n"
        "  <d:prop>\n"
        "    <d:getetag />\n"
        "    <c:calendar-data />\n"
        "  </d:prop>\n"
        "  <c:filter>\n"
        "    <c:comp-filter name=\"VCALENDAR\">\n"
        "      <c:comp-filter name=\"VTODO\" />\n"
        "    </c:comp-filter>\n"
        "  </c:filter>\n"
        "</c:calendar-query>\n"
    );

    QNetworkReply* reply = m_nam.sendCustomRequest(req, "REPORT", reportXml);
    connect(reply, &QNetworkReply::finished, this, [this, reply, entry, req]() {
        reply->deleteLater();
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            if (statusCode == 405 || statusCode == 501) {
                QNetworkRequest propReq = req;
                propReq.setRawHeader("Depth", "1");
                const QByteArray propXml = QByteArray(
                    "<d:propfind xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">\n"
                    "  <d:prop><c:calendar-data /></d:prop>\n"
                    "</d:propfind>\n"
                );
                QNetworkReply* propReply = m_nam.sendCustomRequest(propReq, "PROPFIND", propXml);
                connect(propReply, &QNetworkReply::finished, this, [this, propReply, entry]() {
                    propReply->deleteLater();
                    if (propReply->error() != QNetworkReply::NoError) {
                        endSyncOperation();
                        const QString errMsg = tr("Nextcloud CalDAV error: %1").arg(propReply->errorString());
                        setStatusMessage(errMsg);
                        emit syncFailed(entry.id, errMsg);
                        return;
                    }
                    const QVector<RemoteIssueItem> items = parseNextcloudTasksIcs(propReply->readAll());
                    processSyncedItems(entry, items, QStringLiteral("Nextcloud"));
                });
                return;
            }

            endSyncOperation();
            const QString errMsg = tr("Nextcloud CalDAV sync error (%1): %2").arg(statusCode).arg(reply->errorString());
            setStatusMessage(errMsg);
            emit syncFailed(entry.id, errMsg);
            return;
        }

        const QVector<RemoteIssueItem> items = parseNextcloudTasksIcs(reply->readAll());
        processSyncedItems(entry, items, QStringLiteral("Nextcloud"));
    });
}

void IssueTrackerService::postWorklog(const TodoListEntry& listEntry,
                                      qint64 taskId,
                                      qint64 activityLogId,
                                      const QString& issueKey,
                                      const QDateTime& startTimeUtc,
                                      quint32 runtimeSeconds)
{
    Q_UNUSED(taskId)
    if (!listEntry.autoWorklog || issueKey.isEmpty() || runtimeSeconds == 0) {
        return;
    }
    const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
    if (baseUrl.isEmpty()) {
        return;
    }

    if (listEntry.provider == QStringLiteral("Jira")) {
        const quint32 loggedSeconds = qMax(60U, runtimeSeconds);
        const QString encodedKey = QString::fromUtf8(QUrl::toPercentEncoding(issueKey));
        QUrl url(baseUrl + QStringLiteral("/rest/api/2/issue/") + encodedKey + QStringLiteral("/worklog"));
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        QJsonObject payload;
        payload[QStringLiteral("timeSpentSeconds")] = static_cast<qint64>(loggedSeconds);
        payload[QStringLiteral("started")] = startTimeUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.000'+0000'"));
        payload[QStringLiteral("comment")] = QStringLiteral("Logged via Go for It Qt Pomodoro timer");

        QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, activityLogId, issueKey, loggedSeconds]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                DatabaseManager::instance().markActivityWorklogSynced(activityLogId);
                setStatusMessage(tr("Logged %1m to Jira issue %2").arg(loggedSeconds / 60U).arg(issueKey));
                emit worklogPosted(issueKey, loggedSeconds);
            }
        });
    } else if (listEntry.provider == QStringLiteral("YouTrack")) {
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
        connect(reply, &QNetworkReply::finished, this, [this, reply, activityLogId, issueKey, minutes]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                DatabaseManager::instance().markActivityWorklogSynced(activityLogId);
                setStatusMessage(tr("Logged %1m to YouTrack issue %2").arg(minutes).arg(issueKey));
                emit worklogPosted(issueKey, minutes * 60U);
            }
        });
    } else if (listEntry.provider == QStringLiteral("GitHub")) {
        const quint32 minutes = qMax(1U, (runtimeSeconds + 30U) / 60U);
        QString repo;
        QString numStr;
        if (issueKey.contains(QLatin1Char('#'))) {
            const QStringList parts = issueKey.split(QLatin1Char('#'));
            if (parts.size() >= 3) {
                repo = parts.at(1);
                numStr = parts.at(2);
            } else if (parts.size() == 2) {
                numStr = parts.at(1);
            }
        }
        if (numStr.isEmpty()) {
            for (const QChar& c : issueKey) if (c.isDigit()) numStr.append(c);
        }
        if (numStr.isEmpty()) return;

        if (repo.isEmpty()) {
            const QString jql = listEntry.jiraJql.trimmed();
            static const QRegularExpression repoRe(QStringLiteral("repo:([a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+)"));
            const auto m = repoRe.match(jql);
            if (m.hasMatch()) {
                repo = m.captured(1);
            } else if (jql.contains(QLatin1Char('/')) && !jql.contains(QLatin1Char(' '))) {
                repo = jql;
            }
        }
        if (repo.isEmpty()) {
            static const QRegularExpression urlRepoRe(QStringLiteral("https?://[^/]+/([a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+)"));
            const auto um = urlRepoRe.match(baseUrl);
            if (um.hasMatch()) repo = um.captured(1);
        }
        if (repo.isEmpty()) return;

        const QString apiUrl = gitHubApiBaseUrl(listEntry.jiraUrl);
        QUrl url(apiUrl + QStringLiteral("/repos/") + repo + QStringLiteral("/issues/") + numStr + QStringLiteral("/comments"));
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        QJsonObject payload{
            {QStringLiteral("body"), QStringLiteral("⏱️ Logged %1m via Go for It Qt Pomodoro timer").arg(minutes)}
        };
        QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, activityLogId, issueKey, minutes]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                DatabaseManager::instance().markActivityWorklogSynced(activityLogId);
                setStatusMessage(tr("Logged %1m to GitHub issue %2").arg(minutes).arg(issueKey));
                emit worklogPosted(issueKey, minutes * 60U);
            }
        });
    } else if (listEntry.provider == QStringLiteral("GitLab")) {
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
        connect(reply, &QNetworkReply::finished, this, [this, reply, activityLogId, issueKey, minutes]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                DatabaseManager::instance().markActivityWorklogSynced(activityLogId);
                setStatusMessage(tr("Logged %1m to GitLab %2").arg(minutes).arg(issueKey));
                emit worklogPosted(issueKey, minutes * 60U);
            }
        });
    } else if (listEntry.provider == QStringLiteral("Redmine")) {
        QString numStr;
        if (issueKey.contains(QLatin1Char('#'))) {
            numStr = issueKey.split(QLatin1Char('#')).last();
        }
        if (numStr.isEmpty()) {
            for (const QChar& c : issueKey) if (c.isDigit()) numStr.append(c);
        }
        if (numStr.isEmpty()) return;

        const double hours = qMax(0.01, static_cast<double>(runtimeSeconds) / 3600.0);
        QUrl url(baseUrl + QStringLiteral("/time_entries.json"));
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        QJsonObject timeEntryObj{
            {QStringLiteral("issue_id"), numStr.toInt()},
            {QStringLiteral("hours"), hours},
            {QStringLiteral("spent_on"), startTimeUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd"))},
            {QStringLiteral("comments"), QStringLiteral("Logged via Go for It Qt Pomodoro timer")}
        };
        QJsonObject payload{{QStringLiteral("time_entry"), timeEntryObj}};

        QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, activityLogId, issueKey, runtimeSeconds]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                DatabaseManager::instance().markActivityWorklogSynced(activityLogId);
                setStatusMessage(tr("Logged %1m to Redmine %2").arg(runtimeSeconds / 60U).arg(issueKey));
                emit worklogPosted(issueKey, runtimeSeconds);
            }
        });
    } else if (listEntry.provider == QStringLiteral("Todoist")) {
        QString taskId = issueKey;
        if (taskId.startsWith(QStringLiteral("TD#"))) taskId = taskId.mid(3);
        if (taskId.isEmpty()) return;

        const quint32 minutes = qMax(1U, (runtimeSeconds + 30U) / 60U);
        QUrl url(QStringLiteral("https://api.todoist.com/rest/v2/comments"));
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        QJsonObject payload{
            {QStringLiteral("task_id"), taskId},
            {QStringLiteral("content"), QStringLiteral("⏱️ Logged %1m via Go for It Qt Pomodoro timer").arg(minutes)}
        };

        QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, activityLogId, issueKey, minutes]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                DatabaseManager::instance().markActivityWorklogSynced(activityLogId);
                setStatusMessage(tr("Logged %1m to Todoist task %2").arg(minutes).arg(issueKey));
                emit worklogPosted(issueKey, minutes * 60U);
            }
        });
    } else if (listEntry.provider == QStringLiteral("Nextcloud")) {
        QString uid = issueKey;
        if (uid.startsWith(QStringLiteral("NC#"))) uid = uid.mid(3);
        if (uid.contains(QLatin1Char('#'))) uid = uid.split(QLatin1Char('#')).last();
        if (uid.isEmpty()) return;

        const quint32 minutes = qMax(1U, (runtimeSeconds + 30U) / 60U);
        QString calUrl = listEntry.jiraUrl.trimmed();
        if (!calUrl.endsWith(QLatin1Char('/')) && !calUrl.endsWith(QStringLiteral(".ics"))) calUrl += QLatin1Char('/');
        const QString itemUrl = calUrl.endsWith(QStringLiteral(".ics")) ? calUrl : (calUrl + uid + QStringLiteral(".ics"));

        QUrl url(itemUrl);
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        QNetworkReply* getReply = m_nam.get(req);
        connect(getReply, &QNetworkReply::finished, this, [this, getReply, req, listEntry, activityLogId, issueKey, minutes]() {
            getReply->deleteLater();
            if (getReply->error() != QNetworkReply::NoError) return;
            QString ics = QString::fromUtf8(getReply->readAll());
            const QString commentLine = QStringLiteral("COMMENT:⏱️ Logged %1m via Go for It Qt Pomodoro timer\r\n").arg(minutes);
            const int vtodoEnd = ics.indexOf(QStringLiteral("END:VTODO"));
            if (vtodoEnd > 0) {
                ics.insert(vtodoEnd, commentLine);
                QNetworkRequest putReq = req;
                putReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/calendar; charset=utf-8"));
                QNetworkReply* putReply = m_nam.put(putReq, ics.toUtf8());
                connect(putReply, &QNetworkReply::finished, this, [this, putReply, activityLogId, issueKey, minutes]() {
                    putReply->deleteLater();
                    if (putReply->error() == QNetworkReply::NoError) {
                        DatabaseManager::instance().markActivityWorklogSynced(activityLogId);
                        setStatusMessage(tr("Logged %1m to Nextcloud task %2").arg(minutes).arg(issueKey));
                        emit worklogPosted(issueKey, minutes * 60U);
                    }
                });
            }
        });
    }
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
                QJsonObject cb{
                    {QStringLiteral("type"), QStringLiteral("codeBlock")},
                    {QStringLiteral("content"), QJsonArray{QJsonObject{
                        {QStringLiteral("type"), QStringLiteral("text")},
                        {QStringLiteral("text"), codeLines.join(QStringLiteral("\n"))}
                    }}}
                };
                if (!codeLang.isEmpty()) {
                    cb[QStringLiteral("attrs")] = QJsonObject{{QStringLiteral("language"), codeLang}};
                }
                docContent.append(cb);
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

void IssueTrackerService::updateIssueDescription(const TodoListEntry& listEntry,
                                                 const QString& issueKey,
                                                 const QString& markdownDetails,
                                                 bool forceBearerAuth,
                                                 bool useV2Fallback)
{
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
            setStatusMessage(tr("Updated description on Jira issue %1.").arg(issueKey));
        });
    } else if (listEntry.provider == QStringLiteral("YouTrack")) {
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
    } else if (listEntry.provider == QStringLiteral("GitHub")) {
        QString repo;
        QString numStr;
        if (issueKey.contains(QLatin1Char('#'))) {
            const QStringList parts = issueKey.split(QLatin1Char('#'));
            if (parts.size() >= 3) {
                repo = parts.at(1);
                numStr = parts.at(2);
            } else if (parts.size() == 2) {
                numStr = parts.at(1);
            }
        }
        if (numStr.isEmpty()) {
            for (const QChar& c : issueKey) if (c.isDigit()) numStr.append(c);
        }
        if (numStr.isEmpty()) return;

        if (repo.isEmpty()) {
            const QString jql = listEntry.jiraJql.trimmed();
            static const QRegularExpression repoRe(QStringLiteral("repo:([a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+)"));
            const auto m = repoRe.match(jql);
            if (m.hasMatch()) repo = m.captured(1);
            else if (jql.contains(QLatin1Char('/')) && !jql.contains(QLatin1Char(' '))) repo = jql;
        }
        if (repo.isEmpty()) {
            static const QRegularExpression urlRepoRe(QStringLiteral("https?://[^/]+/([a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+)"));
            const auto um = urlRepoRe.match(baseUrl);
            if (um.hasMatch()) repo = um.captured(1);
        }
        if (repo.isEmpty()) return;

        const QString apiUrl = gitHubApiBaseUrl(listEntry.jiraUrl);
        QUrl url(apiUrl + QStringLiteral("/repos/") + repo + QStringLiteral("/issues/") + numStr);
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        QJsonObject payload{{QStringLiteral("body"), markdownDetails}};
        QNetworkReply* reply = m_nam.sendCustomRequest(req, "PATCH", QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, issueKey]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                setStatusMessage(tr("Failed to update description on GitHub (%1): %2").arg(issueKey, reply->errorString()));
                return;
            }
            setStatusMessage(tr("Updated description on GitHub issue %1.").arg(issueKey));
        });
    } else if (listEntry.provider == QStringLiteral("GitLab")) {
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
    } else if (listEntry.provider == QStringLiteral("Redmine")) {
        QString numStr;
        if (issueKey.contains(QLatin1Char('#'))) {
            numStr = issueKey.split(QLatin1Char('#')).last();
        }
        if (numStr.isEmpty()) {
            for (const QChar& c : issueKey) if (c.isDigit()) numStr.append(c);
        }
        if (numStr.isEmpty()) return;

        QUrl url(baseUrl + QStringLiteral("/issues/") + numStr + QStringLiteral(".json"));
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        QJsonObject payload{
            {QStringLiteral("issue"), QJsonObject{{QStringLiteral("description"), markdownDetails}}}
        };
        QNetworkReply* reply = m_nam.sendCustomRequest(req, "PUT", QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, issueKey]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                setStatusMessage(tr("Failed to update description on Redmine (%1): %2").arg(issueKey, reply->errorString()));
                return;
            }
            setStatusMessage(tr("Updated description on Redmine issue %1.").arg(issueKey));
        });
    } else if (listEntry.provider == QStringLiteral("Todoist")) {
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
    } else if (listEntry.provider == QStringLiteral("Nextcloud")) {
        QString uid = issueKey;
        if (uid.startsWith(QStringLiteral("NC#"))) uid = uid.mid(3);
        if (uid.contains(QLatin1Char('#'))) uid = uid.split(QLatin1Char('#')).last();
        if (uid.isEmpty()) return;

        QString calUrl = listEntry.jiraUrl.trimmed();
        if (!calUrl.endsWith(QLatin1Char('/')) && !calUrl.endsWith(QStringLiteral(".ics"))) calUrl += QLatin1Char('/');
        const QString itemUrl = calUrl.endsWith(QStringLiteral(".ics")) ? calUrl : (calUrl + uid + QStringLiteral(".ics"));

        QUrl url(itemUrl);
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        QNetworkReply* getReply = m_nam.get(req);
        connect(getReply, &QNetworkReply::finished, this, [this, getReply, req, issueKey, markdownDetails]() {
            getReply->deleteLater();
            if (getReply->error() != QNetworkReply::NoError) return;
            QString ics = QString::fromUtf8(getReply->readAll());
            static const QRegularExpression descRe(QStringLiteral("DESCRIPTION:.*(?=\\r?\\n[A-Z-]+:)"), QRegularExpression::DotMatchesEverythingOption);
            const QString escaped = QStringLiteral("DESCRIPTION:") + escapeIcsText(markdownDetails);
            if (ics.contains(descRe)) {
                ics.replace(descRe, escaped);
            } else {
                const int vtodoEnd = ics.indexOf(QStringLiteral("END:VTODO"));
                if (vtodoEnd > 0) {
                    ics.insert(vtodoEnd, escaped + QStringLiteral("\r\n"));
                }
            }
            QNetworkRequest putReq = req;
            putReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/calendar; charset=utf-8"));
            QNetworkReply* putReply = m_nam.put(putReq, ics.toUtf8());
            connect(putReply, &QNetworkReply::finished, this, [this, putReply, issueKey]() {
                putReply->deleteLater();
                if (putReply->error() == QNetworkReply::NoError) {
                    setStatusMessage(tr("Updated description on Nextcloud task %1.").arg(issueKey));
                }
            });
        });
    }
}

void IssueTrackerService::postIssueComment(const TodoListEntry& listEntry,
                                           const QString& issueKey,
                                           const QString& markdownComment,
                                           bool forceBearerAuth,
                                           bool useV2Fallback)
{
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
            setStatusMessage(tr("Posted comment to Jira issue %1.").arg(issueKey));
        });
    } else if (listEntry.provider == QStringLiteral("YouTrack")) {
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
    } else if (listEntry.provider == QStringLiteral("GitHub")) {
        QString repo;
        QString numStr;
        if (issueKey.contains(QLatin1Char('#'))) {
            const QStringList parts = issueKey.split(QLatin1Char('#'));
            if (parts.size() >= 3) {
                repo = parts.at(1);
                numStr = parts.at(2);
            } else if (parts.size() == 2) {
                numStr = parts.at(1);
            }
        }
        if (numStr.isEmpty()) {
            for (const QChar& c : issueKey) if (c.isDigit()) numStr.append(c);
        }
        if (numStr.isEmpty()) return;

        if (repo.isEmpty()) {
            const QString jql = listEntry.jiraJql.trimmed();
            static const QRegularExpression repoRe(QStringLiteral("repo:([a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+)"));
            const auto m = repoRe.match(jql);
            if (m.hasMatch()) repo = m.captured(1);
            else if (jql.contains(QLatin1Char('/')) && !jql.contains(QLatin1Char(' '))) repo = jql;
        }
        if (repo.isEmpty()) {
            static const QRegularExpression urlRepoRe(QStringLiteral("https?://[^/]+/([a-zA-Z0-9_.-]+/[a-zA-Z0-9_.-]+)"));
            const auto um = urlRepoRe.match(baseUrl);
            if (um.hasMatch()) repo = um.captured(1);
        }
        if (repo.isEmpty()) return;

        const QString apiUrl = gitHubApiBaseUrl(listEntry.jiraUrl);
        QUrl url(apiUrl + QStringLiteral("/repos/") + repo + QStringLiteral("/issues/") + numStr + QStringLiteral("/comments"));
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        QJsonObject payload{{QStringLiteral("body"), markdownComment}};
        QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, issueKey]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                setStatusMessage(tr("Failed to post comment to GitHub (%1): %2").arg(issueKey, reply->errorString()));
                return;
            }
            setStatusMessage(tr("Posted comment to GitHub issue %1.").arg(issueKey));
        });
    } else if (listEntry.provider == QStringLiteral("GitLab")) {
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
    } else if (listEntry.provider == QStringLiteral("Redmine")) {
        QString numStr;
        if (issueKey.contains(QLatin1Char('#'))) {
            numStr = issueKey.split(QLatin1Char('#')).last();
        }
        if (numStr.isEmpty()) {
            for (const QChar& c : issueKey) if (c.isDigit()) numStr.append(c);
        }
        if (numStr.isEmpty()) return;

        QUrl url(baseUrl + QStringLiteral("/issues/") + numStr + QStringLiteral(".json"));
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        QJsonObject payload{
            {QStringLiteral("issue"), QJsonObject{{QStringLiteral("notes"), markdownComment}}}
        };
        QNetworkReply* reply = m_nam.sendCustomRequest(req, "PUT", QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, issueKey]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                setStatusMessage(tr("Failed to post comment to Redmine (%1): %2").arg(issueKey, reply->errorString()));
                return;
            }
            setStatusMessage(tr("Posted comment to Redmine issue %1.").arg(issueKey));
        });
    } else if (listEntry.provider == QStringLiteral("Todoist")) {
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
    } else if (listEntry.provider == QStringLiteral("Nextcloud")) {
        QString uid = issueKey;
        if (uid.startsWith(QStringLiteral("NC#"))) uid = uid.mid(3);
        if (uid.contains(QLatin1Char('#'))) uid = uid.split(QLatin1Char('#')).last();
        if (uid.isEmpty()) return;

        QString calUrl = listEntry.jiraUrl.trimmed();
        if (!calUrl.endsWith(QLatin1Char('/')) && !calUrl.endsWith(QStringLiteral(".ics"))) calUrl += QLatin1Char('/');
        const QString itemUrl = calUrl.endsWith(QStringLiteral(".ics")) ? calUrl : (calUrl + uid + QStringLiteral(".ics"));

        QUrl url(itemUrl);
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        QNetworkReply* getReply = m_nam.get(req);
        connect(getReply, &QNetworkReply::finished, this, [this, getReply, req, issueKey, markdownComment]() {
            getReply->deleteLater();
            if (getReply->error() != QNetworkReply::NoError) return;
            QString ics = QString::fromUtf8(getReply->readAll());
            const QString escapedComment = escapeIcsText(markdownComment);
            const QString commentLine = QStringLiteral("COMMENT:%1\r\n").arg(escapedComment);
            const int vtodoEnd = ics.indexOf(QStringLiteral("END:VTODO"));
            if (vtodoEnd > 0) {
                ics.insert(vtodoEnd, commentLine);
                QNetworkRequest putReq = req;
                putReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/calendar; charset=utf-8"));
                QNetworkReply* putReply = m_nam.put(putReq, ics.toUtf8());
                connect(putReply, &QNetworkReply::finished, this, [this, putReply, issueKey]() {
                    putReply->deleteLater();
                    if (putReply->error() == QNetworkReply::NoError) {
                        setStatusMessage(tr("Posted comment to Nextcloud task %1.").arg(issueKey));
                    }
                });
            }
        });
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
        const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
        const QString key = issueKey.trimmed();
        if (!baseUrl.isEmpty() && !key.isEmpty()) {
            return QUrl(baseUrl + QStringLiteral("/issue/") + key);
        }
    } else if (provider == QStringLiteral("GitHub")) {
        QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
        if (baseUrl.isEmpty() || baseUrl.contains(QStringLiteral("api.github.com"))) {
            baseUrl = QStringLiteral("https://github.com");
        } else if (baseUrl.endsWith(QStringLiteral("/api/v3"), Qt::CaseInsensitive)) {
            baseUrl.chop(7);
        }

        const bool isPr = issueKey.startsWith(QStringLiteral("PR#"), Qt::CaseInsensitive)
                       || issueKey.startsWith(QStringLiteral("PR!"), Qt::CaseInsensitive);
        QString owner;
        QString repo;
        QString numStr;

        if (issueKey.contains(QLatin1Char('#'))) {
            const QStringList parts = issueKey.split(QLatin1Char('#'));
            if (parts.size() >= 3) {
                const QString repoPart = parts.at(1);
                const int slashIdx = repoPart.indexOf(QLatin1Char('/'));
                if (slashIdx > 0) {
                    owner = repoPart.left(slashIdx);
                    repo = repoPart.mid(slashIdx + 1);
                }
                numStr = parts.at(2);
            } else if (parts.size() == 2) {
                if (parts.at(0).contains(QLatin1Char('/'))) {
                    const int slashIdx = parts.at(0).indexOf(QLatin1Char('/'));
                    owner = parts.at(0).left(slashIdx);
                    repo = parts.at(0).mid(slashIdx + 1);
                }
                numStr = parts.at(1);
            }
        }

        if (owner.isEmpty() || repo.isEmpty()) {
            QString spec = listEntry.jiraJql.trimmed();
            if (spec.startsWith(QStringLiteral("repo:"), Qt::CaseInsensitive)) {
                spec = spec.mid(5).trimmed();
            }
            const int slashIdx = spec.indexOf(QLatin1Char('/'));
            if (slashIdx > 0) {
                owner = spec.left(slashIdx);
                repo = spec.mid(slashIdx + 1);
                const int sp = repo.indexOf(QLatin1Char(' '));
                if (sp > 0) repo = repo.left(sp);
            }
        }

        if (numStr.isEmpty()) {
            for (const QChar& c : issueKey) {
                if (c.isDigit()) numStr.append(c);
            }
        }

        if (!owner.isEmpty() && !repo.isEmpty() && !numStr.isEmpty()) {
            const QString itemType = isPr ? QStringLiteral("pull") : QStringLiteral("issues");
            return QUrl(QStringLiteral("%1/%2/%3/%4/%5").arg(baseUrl, owner, repo, itemType, numStr));
        }
        if (!baseUrl.isEmpty() && !listEntry.jiraUrl.trimmed().isEmpty()) {
            return QUrl(baseUrl);
        }
    } else if (provider == QStringLiteral("GitLab")) {
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
    } else if (provider == QStringLiteral("Redmine")) {
        const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
        QString numStr;
        for (const QChar& c : issueKey) {
            if (c.isDigit()) numStr.append(c);
        }
        if (!baseUrl.isEmpty() && !numStr.isEmpty()) {
            return QUrl(baseUrl + QStringLiteral("/issues/") + numStr);
        }
        if (!baseUrl.isEmpty()) {
            return QUrl(baseUrl);
        }
    } else if (provider == QStringLiteral("Todoist")) {
        QString numStr;
        for (const QChar& c : issueKey) {
            if (c.isDigit()) numStr.append(c);
        }
        if (!numStr.isEmpty()) {
            return QUrl(QStringLiteral("https://todoist.com/app/task/") + numStr);
        }
        return QUrl(QStringLiteral("https://todoist.com/app/today"));
    } else if (provider == QStringLiteral("Nextcloud")) {
        const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
        if (!baseUrl.isEmpty()) {
            return QUrl(baseUrl + QStringLiteral("/apps/tasks/"));
        }
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
