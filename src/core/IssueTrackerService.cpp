#include "IssueTrackerService.h"
#include "trackers/TrackerCommon.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace gofi {

namespace {

int resolveRedmineStatusId(const QVariantMap& statusMapping, bool done) {
    const int fallbackId = done ? 5 : 1;
    const QString targetCategory = done ? QStringLiteral("done") : QStringLiteral("todo");

    QStringList preferredOrder;
    if (done) {
        preferredOrder << QStringLiteral("closed") << QStringLiteral("resolved") << QStringLiteral("done") << QStringLiteral("fixed");
    } else {
        preferredOrder << QStringLiteral("new") << QStringLiteral("open");
    }

    // 1. Explicit user mapping matching preferred names
    QString selectedStatusName;
    for (const QString& pref : preferredOrder) {
        for (auto it = statusMapping.cbegin(); it != statusMapping.cend(); ++it) {
            if (it.key().startsWith(QLatin1Char('_'))) continue;
            if (it.value().toString().compare(targetCategory, Qt::CaseInsensitive) == 0 &&
                it.key().compare(pref, Qt::CaseInsensitive) == 0) {
                selectedStatusName = it.key();
                break;
            }
        }
        if (!selectedStatusName.isEmpty()) break;
    }

    // 2. Explicit user mapping matching any status in category (skipping reject/cancel for done)
    if (selectedStatusName.isEmpty()) {
        for (auto it = statusMapping.cbegin(); it != statusMapping.cend(); ++it) {
            if (it.key().startsWith(QLatin1Char('_'))) continue;
            if (it.value().toString().compare(targetCategory, Qt::CaseInsensitive) == 0) {
                if (done && (it.key().contains(QStringLiteral("reject"), Qt::CaseInsensitive) ||
                             it.key().contains(QStringLiteral("cancel"), Qt::CaseInsensitive) ||
                             it.key().contains(QStringLiteral("declin"), Qt::CaseInsensitive) ||
                             it.key().contains(QStringLiteral("won't"), Qt::CaseInsensitive))) {
                    continue;
                }
                selectedStatusName = it.key();
                break;
            }
        }
    }

    if (!selectedStatusName.isEmpty()) {
        const QString idKey = QStringLiteral("_id_") + selectedStatusName;
        if (statusMapping.contains(idKey)) {
            bool ok = false;
            const int mappedId = statusMapping.value(idKey).toInt(&ok);
            if (ok && mappedId > 0) {
                return mappedId;
            }
        }
    }

    // 3. Auto-learned _id_<name> matching preferred names
    for (const QString& pref : preferredOrder) {
        for (auto it = statusMapping.cbegin(); it != statusMapping.cend(); ++it) {
            if (it.key().startsWith(QStringLiteral("_id_"))) {
                const QString rawName = it.key().mid(4);
                if (rawName.compare(pref, Qt::CaseInsensitive) == 0) {
                    bool ok = false;
                    const int mappedId = it.value().toInt(&ok);
                    if (ok && mappedId > 0) {
                        return mappedId;
                    }
                }
            }
        }
    }

    // 4. If done, search among all learned _is_closed_<id> keys, filtering out rejected/canceled
    if (done) {
        QHash<int, QString> idToName;
        for (auto it = statusMapping.cbegin(); it != statusMapping.cend(); ++it) {
            if (it.key().startsWith(QStringLiteral("_id_"))) {
                bool ok = false;
                const int id = it.value().toInt(&ok);
                if (ok && id > 0) {
                    idToName.insert(id, it.key().mid(4));
                }
            }
        }

        QList<int> candidateIds;
        for (auto it = statusMapping.cbegin(); it != statusMapping.cend(); ++it) {
            if (it.key().startsWith(QStringLiteral("_is_closed_"))) {
                bool ok = false;
                const int id = it.key().mid(11).toInt(&ok);
                if (ok && id > 0) {
                    const QString name = idToName.value(id);
                    if (!name.contains(QStringLiteral("reject"), Qt::CaseInsensitive) &&
                        !name.contains(QStringLiteral("cancel"), Qt::CaseInsensitive) &&
                        !name.contains(QStringLiteral("declin"), Qt::CaseInsensitive) &&
                        !name.contains(QStringLiteral("won't"), Qt::CaseInsensitive)) {
                        candidateIds.append(id);
                    }
                }
            }
        }

        std::sort(candidateIds.begin(), candidateIds.end());

        // Check if any candidate name contains done / closed / resolved / fixed
        for (int id : candidateIds) {
            const QString name = idToName.value(id);
            if (name.contains(QStringLiteral("done"), Qt::CaseInsensitive) ||
                name.contains(QStringLiteral("closed"), Qt::CaseInsensitive) ||
                name.contains(QStringLiteral("resolved"), Qt::CaseInsensitive) ||
                name.contains(QStringLiteral("fixed"), Qt::CaseInsensitive)) {
                return id;
            }
        }

        if (!candidateIds.isEmpty()) {
            return candidateIds.first();
        }
    }

    return fallbackId;
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

void IssueTrackerService::applyAuthHeader(QNetworkRequest& req, const TodoListEntry& entry, bool forceBearerAuth) {
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("User-Agent", "Go-For-It-Qt");
    req.setTransferTimeout(25000); // 25 seconds timeout to prevent hanging connections

    const QUrl reqUrl = req.url();
    if (reqUrl.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 && !entry.apiToken.trimmed().isEmpty()) {
        const QString host = reqUrl.host();
        if (host != QStringLiteral("localhost") && host != QStringLiteral("127.0.0.1")) {
            const QString warnMsg = QStringLiteral("Security warning: Transmitting API token over unencrypted HTTP to %1!").arg(host);
            qWarning() << warnMsg;
            setStatusMessage(warnMsg);
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
    } else if (!forceBearerAuth && !(m_jiraBearerTokens.contains(entry.id) && m_jiraBearerTokens.value(entry.id) == entry.apiToken) && entry.provider == QStringLiteral("Jira") && !user.isEmpty()) {
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

    // Tokenize status string into words
    static const QRegularExpression wordRe(QStringLiteral("[^\\p{L}\\p{N}]+"));
    const QStringList tokens = s.split(wordRe, Qt::SkipEmptyParts);

    // 0. Check explicit negation keywords before matching positive categories
    // e.g. "Incomplete", "Not Done", "Inactive", "Unblocked"
    const bool hasNot = tokens.contains(QStringLiteral("not")) || tokens.contains(QStringLiteral("non")) || tokens.contains(QStringLiteral("не"));
    if (tokens.contains(QStringLiteral("incomplete")) || tokens.contains(QStringLiteral("undone")) ||
        tokens.contains(QStringLiteral("uncompleted")) || tokens.contains(QStringLiteral("невиконан")) ||
        (hasNot && (tokens.contains(QStringLiteral("done")) || tokens.contains(QStringLiteral("complete")) ||
                    tokens.contains(QStringLiteral("completed")) || tokens.contains(QStringLiteral("finished")) ||
                    tokens.contains(QStringLiteral("resolved")) || tokens.contains(QStringLiteral("fixed")) ||
                    tokens.contains(QStringLiteral("готово")) || tokens.contains(QStringLiteral("виконано"))))) {
        return QStringLiteral("todo");
    }
    if (tokens.contains(QStringLiteral("inactive")) || (hasNot && tokens.contains(QStringLiteral("active")))) {
        return QStringLiteral("todo");
    }
    if (tokens.contains(QStringLiteral("unblocked")) || (hasNot && (tokens.contains(QStringLiteral("blocked")) || tokens.contains(QStringLiteral("block"))))) {
        return QStringLiteral("todo");
    }

    // 1. Check Canceled / Rejected keywords first
    if (s.contains(QStringLiteral("won't fix")) || s.contains(QStringLiteral("wontfix"))
        || tokens.contains(QStringLiteral("cancel")) || tokens.contains(QStringLiteral("canceled")) || tokens.contains(QStringLiteral("cancelled"))
        || tokens.contains(QStringLiteral("reject")) || tokens.contains(QStringLiteral("rejected"))
        || tokens.contains(QStringLiteral("duplicate")) || tokens.contains(QStringLiteral("obsolete"))
        || tokens.contains(QStringLiteral("invalid")) || tokens.contains(QStringLiteral("declined")) || tokens.contains(QStringLiteral("decline"))
        || s.contains(QStringLiteral("скасован")) || s.contains(QStringLiteral("відхилен"))) {
        return QStringLiteral("canceled");
    }

    // 2. Check Paused / On Hold / Blocked / Waiting keywords
    if (tokens.contains(QStringLiteral("pause")) || tokens.contains(QStringLiteral("paused"))
        || tokens.contains(QStringLiteral("hold"))
        || tokens.contains(QStringLiteral("block")) || tokens.contains(QStringLiteral("blocked")) || tokens.contains(QStringLiteral("blocking"))
        || tokens.contains(QStringLiteral("wait")) || tokens.contains(QStringLiteral("waiting"))
        || tokens.contains(QStringLiteral("pending")) || tokens.contains(QStringLiteral("postpone")) || tokens.contains(QStringLiteral("postponed"))
        || tokens.contains(QStringLiteral("draft")) || s.contains(QStringLiteral("чернетк"))
        || s.contains(QStringLiteral("пауз")) || s.contains(QStringLiteral("очікув"))) {
        return QStringLiteral("paused");
    }

    // 3. Check In Progress / Active / Review / Testing keywords
    if (cat == QStringLiteral("indeterminate") || cat == QStringLiteral("in_progress")
        || tokens.contains(QStringLiteral("progress"))
        || tokens.contains(QStringLiteral("review")) || tokens.contains(QStringLiteral("reviewing"))
        || tokens.contains(QStringLiteral("active")) || tokens.contains(QStringLiteral("working"))
        || tokens.contains(QStringLiteral("testing")) || tokens.contains(QStringLiteral("qa"))
        || tokens.contains(QStringLiteral("verification")) || s.contains(QStringLiteral("робот"))) {
        return QStringLiteral("in_progress");
    }

    // 4. Check Done / Resolved / Closed / Verified / Merged
    if (cat == QStringLiteral("done") || cat == QStringLiteral("resolved") || cat == QStringLiteral("closed")
        || tokens.contains(QStringLiteral("done")) || tokens.contains(QStringLiteral("resolved"))
        || tokens.contains(QStringLiteral("closed")) || tokens.contains(QStringLiteral("close"))
        || tokens.contains(QStringLiteral("fixed")) || tokens.contains(QStringLiteral("fix"))
        || tokens.contains(QStringLiteral("complete")) || tokens.contains(QStringLiteral("completed"))
        || tokens.contains(QStringLiteral("verified")) || tokens.contains(QStringLiteral("merged"))
        || s.contains(QStringLiteral("злит")) || s.contains(QStringLiteral("готово")) || s.contains(QStringLiteral("виконан"))) {
        return QStringLiteral("done");
    }

    return QStringLiteral("todo");
}

quint8 IssueTrackerService::mapPriorityName(const QString& priorityName) {
    QString p = priorityName.trimmed().toLower();
    if (p.isEmpty()) return TxtTask::NoPriority;

    // Check P0-P4 with optional " - Name" or ": Name", e.g. "P1 - High", "P0: Blocker", "P2 - Normal"
    static const QRegularExpression pCodeNameRe(QStringLiteral("^p([0-4])(?:\\s*[-:]\\s*.*)?$"));
    const auto pMatch = pCodeNameRe.match(p);
    if (pMatch.hasMatch()) {
        return pMatch.captured(1).toUShort();
    }

    bool hadPrefix = false;
    static const QStringList prefixes = {
        QStringLiteral("priority::"), QStringLiteral("priority:"), QStringLiteral("priority/"),
        QStringLiteral("priority-"), QStringLiteral("priority_"),
        QStringLiteral("prio::"), QStringLiteral("prio:"), QStringLiteral("prio/"),
        QStringLiteral("prio-"), QStringLiteral("prio_"),
        QStringLiteral("severity::"), QStringLiteral("severity:"), QStringLiteral("severity/"),
        QStringLiteral("severity-"), QStringLiteral("severity_")
    };
    for (const QString& pref : prefixes) {
        if (p.startsWith(pref)) {
            p = p.mid(pref.length()).trimmed();
            hadPrefix = true;
            break;
        }
    }

    // Re-check P-code on stripped string for scoped labels like "priority::P1", "prio:P0", "priority/p2"
    if (hadPrefix) {
        const auto pMatchAfter = pCodeNameRe.match(p);
        if (pMatchAfter.hasMatch()) {
            return pMatchAfter.captured(1).toUShort();
        }
    }

    // Check P-0 to P-4 or hadPrefix with 0..4
    if (p == QStringLiteral("p-0") || (hadPrefix && p == QStringLiteral("0"))) {
        return 0; // (A)
    }
    if (p == QStringLiteral("p-1") || (hadPrefix && p == QStringLiteral("1"))) {
        return 1; // (B)
    }
    if (p == QStringLiteral("p-2") || (hadPrefix && p == QStringLiteral("2"))) {
        return 2; // (C)
    }
    if (p == QStringLiteral("p-3") || (hadPrefix && p == QStringLiteral("3"))) {
        return 3; // (D)
    }
    if (p == QStringLiteral("p-4") || (hadPrefix && p == QStringLiteral("4"))) {
        return 4; // (E)
    }

    // Strip priority/severity/prio suffixes separated by space, '-', or '_'
    static const QStringList suffixes = {
        QStringLiteral(" priority"), QStringLiteral("-priority"), QStringLiteral("_priority"),
        QStringLiteral(" severity"), QStringLiteral("-severity"), QStringLiteral("_severity"),
        QStringLiteral(" prio"), QStringLiteral("-prio"), QStringLiteral("_prio")
    };
    for (const QString& suff : suffixes) {
        if (p.endsWith(suff)) {
            p.chop(suff.length());
            p = p.trimmed();
            break;
        }
    }

    // Strip leading "priority " or "severity "
    if (p.startsWith(QStringLiteral("priority "))) p = p.mid(9).trimmed();
    if (p.startsWith(QStringLiteral("severity "))) p = p.mid(9).trimmed();

    p = p.trimmed();

    // Check whole-string matches
    // 0: Highest, Blocker, Show-stopper
    if (p == QStringLiteral("highest") || p == QStringLiteral("blocker") ||
        p == QStringLiteral("show-stopper") || p == QStringLiteral("showstopper") ||
        p == QStringLiteral("very high") || p == QStringLiteral("very-high")) {
        return 0; // (A)
    }
    // 1: High, Critical, Major, Urgent
    if (p == QStringLiteral("high") || p == QStringLiteral("critical") ||
        p == QStringLiteral("major") || p == QStringLiteral("urgent")) {
        return 1; // (B)
    }
    // 2: Medium, Normal, Moderate
    if (p == QStringLiteral("medium") || p == QStringLiteral("normal") ||
        p == QStringLiteral("moderate") || p == QStringLiteral("default")) {
        return 2; // (C)
    }
    // 4: Lowest, Trivial, Very Low
    if (p == QStringLiteral("lowest") || p == QStringLiteral("trivial") ||
        p == QStringLiteral("very low") || p == QStringLiteral("very-low")) {
        return 4; // (E)
    }
    // 3: Low, Minor
    if (p == QStringLiteral("low") || p == QStringLiteral("minor")) {
        return 3; // (D)
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

        // Check if p is a number matching range keys like "2-4" or number-prefixed keys like "1 (Highest)"
        bool isNum = false;
        const int pVal = p.toInt(&isNum);
        if (isNum) {
            for (auto it = customMapping.constBegin(); it != customMapping.constEnd(); ++it) {
                const QString k = it.key().trimmed();
                static const QRegularExpression rangeRe(QStringLiteral("^(\\d+)\\s*-\\s*(\\d+)"));
                const auto m = rangeRe.match(k);
                if (m.hasMatch()) {
                    const int low = m.captured(1).toInt();
                    const int high = m.captured(2).toInt();
                    if (pVal >= low && pVal <= high) {
                        return parseLocalPrio(it.value().toString());
                    }
                }
                static const QRegularExpression numPrefixRe(QStringLiteral("^(\\d+)\\b"));
                const auto nm = numPrefixRe.match(k);
                if (nm.hasMatch() && nm.captured(1).toInt() == pVal) {
                    return parseLocalPrio(it.value().toString());
                }
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
            {"Priority 4", "A"},
            {"Priority 3", "B"},
            {"Priority 2", "C"},
            {"Priority 1", "none"}
        };
    } else if (provider == QStringLiteral("Nextcloud")) {
        presets = {
            {"1", "A"},
            {"2", "B"},
            {"3", "B"},
            {"4", "B"},
            {"5", "C"},
            {"6", "D"},
            {"7", "D"},
            {"8", "D"},
            {"9", "E"}
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
        emit remotePrioritiesError(tr("Please enter the Server URL first."));
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
        connect(reply, &QNetworkReply::finished, this, [this, reply, tempEntry, baseUrl, existingMapping]() {
            reply->deleteLater();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray seraphReason = reply->rawHeader("X-Seraph-Loginreason");
            const bool seraphAuthFailed = seraphReason.contains("AUTHENTICATED_FAILED") || seraphReason.contains("AUTHENTICATION_DENIED");
            if (reply->error() != QNetworkReply::NoError || seraphAuthFailed) {
                if (!tempEntry.jiraUser.trimmed().isEmpty() && (httpStatus == 401 || httpStatus == 403 || seraphAuthFailed)) {
                    // Retry with Bearer token (Jira Server/DC PAT)
                    QNetworkRequest bearerReq(QUrl(baseUrl + QStringLiteral("/rest/api/2/priority")));
                    applyAuthHeader(bearerReq, tempEntry, true);
                    QNetworkReply* retryReply = m_nam.get(bearerReq);
                    connect(retryReply, &QNetworkReply::finished, this, [this, retryReply, existingMapping, seraphAuthFailed]() {
                        retryReply->deleteLater();
                        const QByteArray retrySeraph = retryReply->rawHeader("X-Seraph-Loginreason");
                        const bool retrySeraphFailed = retrySeraph.contains("AUTHENTICATED_FAILED") || retrySeraph.contains("AUTHENTICATION_DENIED");
                        if (retryReply->error() != QNetworkReply::NoError || retrySeraphFailed) {
                            if (seraphAuthFailed || retrySeraphFailed) {
                                emit remotePrioritiesError(tr("Jira authentication failed: please check your Jira Email (e.g. corporate email) and API Token."));
                            } else {
                                emit remotePrioritiesError(tr("Failed to load Jira priorities: %1").arg(retryReply->errorString()));
                            }
                            return;
                        }
                        const QVariantList list = parseJiraPrioritiesJson(retryReply->readAll(), existingMapping);
                        if (!list.isEmpty()) {
                            emit remotePrioritiesLoaded(list);
                            return;
                        }
                        const int retryHttpStatus = retryReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                        emit remotePrioritiesError(tr("Failed to load Jira priorities (%1): %2").arg(retryHttpStatus).arg(retryReply->errorString()));
                    });
                    return;
                }
                if (seraphAuthFailed) {
                    emit remotePrioritiesError(tr("Jira authentication failed: please check your Jira Email and API Token."));
                } else {
                    emit remotePrioritiesError(tr("Failed to load Jira priorities (%1): %2").arg(httpStatus).arg(reply->errorString()));
                }
                return;
            }
            const QVariantList list = parseJiraPrioritiesJson(reply->readAll(), existingMapping);
            if (!list.isEmpty()) {
                emit remotePrioritiesLoaded(list);
                return;
            }
            emit remotePrioritiesError(tr("Failed to load Jira priorities (%1): %2").arg(httpStatus).arg(reply->errorString()));
        });
    } else if (provider == QStringLiteral("Redmine")) {
        QUrl url(baseUrl + QStringLiteral("/enumerations/issue_priorities.json"));
        QNetworkRequest req(url);
        applyAuthHeader(req, tempEntry);

        QNetworkReply* reply = m_nam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, existingMapping]() {
            reply->deleteLater();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::NoError) {
                const QVariantList list = parseRedminePrioritiesJson(reply->readAll(), existingMapping);
                if (!list.isEmpty()) {
                    emit remotePrioritiesLoaded(list);
                    return;
                }
            }
            emit remotePrioritiesError(tr("Failed to load Redmine priorities (%1): %2").arg(httpStatus).arg(reply->errorString()));
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

void IssueTrackerService::processSyncedItems(const TodoListEntry& entry,
                                             const QVector<RemoteIssueItem>& items,
                                             const QString& providerName,
                                             bool pageLimitReached,
                                             bool hasError,
                                             bool explicitZeroTotal)
{
    DatabaseManager& db = DatabaseManager::instance();
    QStringList newIssues;
    QStringList statusChanges;
    QSet<QString> remoteKeys;

    if (!db.beginTransaction()) {
        endSyncOperation();
        const QString errMsg = tr("Failed to start database transaction for synchronized issues from %1.").arg(providerName);
        setStatusMessage(errMsg);
        emit syncFailed(entry.id, errMsg);
        return;
    }
    bool hasDbError = false;
    for (const RemoteIssueItem& item : items) {
        if (!item.key.isEmpty()) {
            remoteKeys.insert(item.key);
        }
        const QString localStatus = mapRemoteStatusToLocal(
            item.remoteStatus, item.remoteStatusCategory, entry.statusMapping
        );
        quint8 localPriority = item.priority;
        if (!entry.priorityMapping.isEmpty()) {
            const quint8 mapped = mapRemotePriorityToLocal(item.remotePriority, entry.priorityMapping);
            if (mapped != TxtTask::NoPriority || entry.priorityMapping.contains(item.remotePriority)) {
                localPriority = mapped;
            }
        }
        bool isNew = false;
        QString oldStatus;
        const bool ok = db.upsertRemoteIssueTask(
            entry.id, item.key, item.description, localStatus, localPriority,
            item.durationSeconds, item.details, item.comments, &isNew, &oldStatus
        );
        if (!ok) {
            hasDbError = true;
            break;
        }
        if (isNew) {
            if (!TxtTask::isClosedStatus(localStatus)) {
                newIssues.append(item.description);
            }
        } else if (!oldStatus.isEmpty() && oldStatus != localStatus) {
            statusChanges.append(QStringLiteral("%1: %2 → %3").arg(item.key, oldStatus, localStatus));
        }
    }

    // Reconcile: Only reconcile closed issues if:
    // 1. Page limit was NOT reached (pageLimitReached is false).
    // 2. No partial or parser error occurred (!hasError).
    // 3. No database write error occurred (!hasDbError).
    // 4. Remote items list is non-empty (!items.isEmpty()), OR the tracker explicitly reported total == 0 (explicitZeroTotal)
    //    in a valid error-free response (e.g. GitHub/Jira/Redmine total_count == 0).
    const bool limitReached = pageLimitReached;
    const bool canReconcile = !limitReached && !hasError && !hasDbError && (!items.isEmpty() || explicitZeroTotal);
    if (canReconcile) {
        const auto localOpenTasks = db.loadTasks(entry.id, false);
        for (const TxtTask& t : localOpenTasks) {
            if (!t.jiraKey().isEmpty() && !remoteKeys.contains(t.jiraKey())) {
                if (m_activeTaskChecker && m_activeTaskChecker(entry.id, t.jiraKey())) {
                    // Do not close a task that currently has an active running timer!
                    continue;
                }
                bool isNew = false;
                QString oldStatus;
                if (db.upsertRemoteIssueTask(
                    entry.id, t.jiraKey(), t.description(), QStringLiteral("done"),
                    t.priority(), t.duration(), t.details(), t.comments(), &isNew, &oldStatus)) {
                    statusChanges.append(QStringLiteral("%1: %2 → done (closed remotely)").arg(t.jiraKey(), t.status()));
                } else {
                    hasDbError = true;
                    break;
                }
            }
        }
    }

    if (hasDbError) {
        db.rollbackTransaction();
        endSyncOperation();
        const QString errMsg = tr("Database error occurred while saving synchronized issues from %1.").arg(providerName);
        setStatusMessage(errMsg);
        emit syncFailed(entry.id, errMsg);
        return;
    }

    if (!db.commitTransaction()) {
        db.rollbackTransaction();
        endSyncOperation();
        const QString errMsg = tr("Database transaction error while saving synchronized issues from %1.").arg(providerName);
        setStatusMessage(errMsg);
        emit syncFailed(entry.id, errMsg);
        return;
    }

    endSyncOperation();
    const QUrl reqUrl(entry.jiraUrl);
    const bool isInsecureHttp = reqUrl.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0 &&
                                reqUrl.host() != QStringLiteral("localhost") &&
                                reqUrl.host() != QStringLiteral("127.0.0.1") &&
                                !entry.apiToken.trimmed().isEmpty();

    QString statusText;
    if (limitReached) {
        statusText = tr("Synced %1 issues from %2 (maximum page limit reached; older items omitted).").arg(items.size()).arg(providerName);
    } else {
        statusText = tr("Synced %1 issues from %2.").arg(items.size()).arg(providerName);
    }
    if (isInsecureHttp) {
        statusText += tr(" ⚠️ Insecure HTTP!");
    }
    setStatusMessage(statusText);
    emit listIssuesSynced(entry.id, static_cast<int>(items.size()));
    if (!newIssues.isEmpty() || !statusChanges.isEmpty()) {
        emit trackerChangesDetected(entry.name, newIssues, statusChanges);
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

void IssueTrackerService::postWorklog(const TodoListEntry& listEntry,
                                      qint64 taskId,
                                      qint64 activityLogId,
                                      const QString& issueKey,
                                      const QDateTime& startTimeUtc,
                                      quint32 runtimeSeconds,
                                      bool forceBearerAuth)
{
    Q_UNUSED(taskId)
    if (QStandardPaths::isTestModeEnabled()) return;
    if (!listEntry.autoWorklog || issueKey.isEmpty() || runtimeSeconds == 0) {
        return;
    }
    const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
    if (baseUrl.isEmpty()) {
        return;
    }

    if (listEntry.provider == QStringLiteral("Jira")) {
        const bool effectiveBearer = forceBearerAuth || (m_jiraBearerTokens.contains(listEntry.id) && m_jiraBearerTokens.value(listEntry.id) == listEntry.apiToken);
        const quint32 loggedSeconds = qMax(60U, runtimeSeconds);
        const QString encodedKey = QString::fromUtf8(QUrl::toPercentEncoding(issueKey));
        QUrl url(baseUrl + QStringLiteral("/rest/api/2/issue/") + encodedKey + QStringLiteral("/worklog"));
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry, effectiveBearer);

        QJsonObject payload;
        payload[QStringLiteral("timeSpentSeconds")] = static_cast<qint64>(loggedSeconds);
        payload[QStringLiteral("started")] = startTimeUtc.toUTC().toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.000'+0000'"));
        payload[QStringLiteral("comment")] = QStringLiteral("Logged via Go for It Qt Pomodoro timer");

        QNetworkReply* reply = m_nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, taskId, activityLogId, issueKey, startTimeUtc, runtimeSeconds, loggedSeconds, effectiveBearer]() {
            reply->deleteLater();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray seraph = reply->rawHeader("X-Seraph-Loginreason");
            const bool authErr = (httpStatus == 401 || httpStatus == 403 || seraph.contains("AUTHENTICATED_FAILED"));

            if (reply->error() != QNetworkReply::NoError || authErr) {
                if (effectiveBearer) {
                    m_jiraBearerTokens.remove(listEntry.id);
                }
                if (!effectiveBearer && !listEntry.jiraUser.trimmed().isEmpty() && authErr) {
                    postWorklog(listEntry, taskId, activityLogId, issueKey, startTimeUtc, runtimeSeconds, true);
                    return;
                }
                const QString errMsg = tr("Failed to post worklog to Jira (%1): %2")
                    .arg(httpStatus)
                    .arg(reply->errorString());
                setStatusMessage(errMsg);
                emit syncFailed(listEntry.id, errMsg);
                return;
            }

            if (effectiveBearer) {
                m_jiraBearerTokens[listEntry.id] = listEntry.apiToken;
            }

            if (DatabaseManager::instance().isOpen() && !DatabaseManager::instance().markActivityWorklogSynced(activityLogId)) {
                const QString dbErr = tr("Database error: failed to mark activity worklog as synced for %1.").arg(issueKey);
                setStatusMessage(dbErr);
                emit syncFailed(listEntry.id, dbErr);
            } else {
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
        connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, activityLogId, issueKey, minutes]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                if (!DatabaseManager::instance().markActivityWorklogSynced(activityLogId)) {
                    const QString dbErr = tr("Database error: failed to mark activity worklog as synced for %1.").arg(issueKey);
                    setStatusMessage(dbErr);
                    emit syncFailed(listEntry.id, dbErr);
                } else {
                    setStatusMessage(tr("Logged %1m to GitHub issue %2").arg(minutes).arg(issueKey));
                    emit worklogPosted(issueKey, minutes * 60U);
                }
            } else {
                const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QString errMsg = tr("Failed to post worklog to GitHub (%1): %2")
                    .arg(statusCode)
                    .arg(reply->errorString());
                setStatusMessage(errMsg);
                emit syncFailed(listEntry.id, errMsg);
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
        connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, activityLogId, issueKey, runtimeSeconds]() {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                if (!DatabaseManager::instance().markActivityWorklogSynced(activityLogId)) {
                    const QString dbErr = tr("Database error: failed to mark activity worklog as synced for %1.").arg(issueKey);
                    setStatusMessage(dbErr);
                    emit syncFailed(listEntry.id, dbErr);
                } else {
                    setStatusMessage(tr("Logged %1m to Redmine %2").arg(runtimeSeconds / 60U).arg(issueKey));
                    emit worklogPosted(issueKey, runtimeSeconds);
                }
            } else {
                const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QString errMsg = tr("Failed to post worklog to Redmine (%1): %2")
                    .arg(statusCode)
                    .arg(reply->errorString());
                setStatusMessage(errMsg);
                emit syncFailed(listEntry.id, errMsg);
            }
        });
    } else if (listEntry.provider == QStringLiteral("Todoist")) {
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
            if (getReply->error() != QNetworkReply::NoError) {
                const int statusCode = getReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QString errMsg = tr("Failed to post worklog to Nextcloud (%1): %2")
                    .arg(statusCode)
                    .arg(getReply->errorString());
                setStatusMessage(errMsg);
                emit syncFailed(listEntry.id, errMsg);
                return;
            }
            QString ics = QString::fromUtf8(getReply->readAll());
            const QString commentLine = QStringLiteral("COMMENT:⏱️ Logged %1m via Go for It Qt Pomodoro timer\r\n").arg(minutes);
            const int vtodoEnd = ics.indexOf(QStringLiteral("END:VTODO"));
            if (vtodoEnd > 0) {
                ics.insert(vtodoEnd, commentLine);
                QNetworkRequest putReq = req;
                putReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/calendar; charset=utf-8"));
                QNetworkReply* putReply = m_nam.put(putReq, ics.toUtf8());
                connect(putReply, &QNetworkReply::finished, this, [this, putReply, listEntry, activityLogId, issueKey, minutes]() {
                    putReply->deleteLater();
                    if (putReply->error() == QNetworkReply::NoError) {
                        if (!DatabaseManager::instance().markActivityWorklogSynced(activityLogId)) {
                            const QString dbErr = tr("Database error: failed to mark activity worklog as synced for %1.").arg(issueKey);
                            setStatusMessage(dbErr);
                            emit syncFailed(listEntry.id, dbErr);
                        } else {
                            setStatusMessage(tr("Logged %1m to Nextcloud task %2").arg(minutes).arg(issueKey));
                            emit worklogPosted(issueKey, minutes * 60U);
                        }
                    } else {
                        const int statusCode = putReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                        const QString errMsg = tr("Failed to post worklog to Nextcloud (%1): %2")
                            .arg(statusCode)
                            .arg(putReply->errorString());
                        setStatusMessage(errMsg);
                        emit syncFailed(listEntry.id, errMsg);
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

void IssueTrackerService::updateIssueDescription(const TodoListEntry& listEntry,
                                                 const QString& issueKey,
                                                 const QString& markdownDetails,
                                                 bool forceBearerAuth,
                                                 bool useV2Fallback)
{
    if (QStandardPaths::isTestModeEnabled()) return;
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
                if (forceBearerAuth) {
                    m_jiraBearerTokens.remove(listEntry.id);
                }
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
            if (forceBearerAuth) {
                m_jiraBearerTokens[listEntry.id] = listEntry.apiToken;
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
            static const QRegularExpression vtodoStartRe(QStringLiteral("(?:^|\r?\n)(BEGIN:VTODO)(?:\r?\n|$)"));
            const auto vtodoStartMatch = vtodoStartRe.match(ics);
            if (!vtodoStartMatch.hasMatch()) return;
            const int vtodoStart = vtodoStartMatch.capturedStart(1);
            const int vtodoPropsStart = vtodoStartMatch.capturedEnd(1);

            static const QRegularExpression vtodoEndRe(QStringLiteral("\r?\n(END:VTODO)(?:\r?\n|$)"));
            const auto vtodoEndMatch = vtodoEndRe.match(ics, vtodoPropsStart);
            if (!vtodoEndMatch.hasMatch()) return;
            const int vtodoEnd = vtodoEndMatch.capturedStart(1);

            int propBlockEnd = vtodoEnd;
            static const QRegularExpression subBeginRe(QStringLiteral("\r?\n(BEGIN:[^\r\n]*)"));
            const auto subBeginMatch = subBeginRe.match(ics, vtodoPropsStart);
            if (subBeginMatch.hasMatch() && subBeginMatch.capturedStart(1) < propBlockEnd) {
                propBlockEnd = subBeginMatch.capturedStart(1);
            }

            static const QRegularExpression descRe(QStringLiteral("(^|\r?\n)DESCRIPTION(?:;[^:]*)?:[^\r\n]*(?:\r?\n[ \t][^\r\n]*)*"));
            const QString escaped = QStringLiteral("DESCRIPTION:") + escapeIcsText(markdownDetails);
            const auto match = descRe.match(ics, vtodoStart);
            if (match.hasMatch() && match.capturedStart() < propBlockEnd) {
                ics.replace(match.capturedStart(), match.capturedLength(), match.captured(1) + escaped);
            } else {
                const QString nl = ics.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n") : QStringLiteral("\n");
                ics.insert(propBlockEnd, escaped + nl);
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

void IssueTrackerService::updateIssueStatusRemote(const TodoListEntry& listEntry,
                                                  const QString& issueKey,
                                                  bool done,
                                                  const QString& localStatus,
                                                  bool forceBearerAuth)
{
    if (!listEntry.syncStatusToRemote) return;
    if (QStandardPaths::isTestModeEnabled()) return;
    if (issueKey.trimmed().isEmpty() || listEntry.apiToken.trimmed().isEmpty()) return;
    const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);

    auto failWithError = [this, listEntry, issueKey](int statusCode, const QString& errStr) {
        const QString errMsg = tr("Failed to update status for %1 on %2 (%3): %4")
            .arg(issueKey, listEntry.provider)
            .arg(statusCode)
            .arg(errStr);
        setStatusMessage(errMsg);
        emit syncFailed(listEntry.id, errMsg);
    };

    auto handleReply = [this, listEntry, issueKey, done, failWithError](QNetworkReply* reply) {
        if (!reply) return;
        connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, issueKey, done, failWithError]() {
            reply->deleteLater();
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError || (statusCode >= 400 && statusCode < 600)) {
                failWithError(statusCode, reply->errorString());
            } else {
                setStatusMessage(done ? tr("Updated %1 on %2: closed").arg(issueKey, listEntry.provider)
                                      : tr("Updated %1 on %2: open").arg(issueKey, listEntry.provider));
            }
        });
    };

    if (listEntry.provider == QStringLiteral("GitHub")) {
        // Never send status closure/reopening for Pull Requests (e.g. from reviews)
        if (issueKey.startsWith(QStringLiteral("PR#"), Qt::CaseInsensitive) ||
            issueKey.contains(QStringLiteral("#PR#"), Qt::CaseInsensitive)) {
            return;
        }

        QString repo;
        QString numStr;
        if (issueKey.contains(QLatin1Char('#'))) {
            const QStringList parts = issueKey.split(QLatin1Char('#'));
            if (parts.size() >= 3) { repo = parts.at(1); numStr = parts.at(2); }
            else if (parts.size() == 2) { numStr = parts.at(1); }
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

        QJsonObject payload{{QStringLiteral("state"), done ? QStringLiteral("closed") : QStringLiteral("open")}};
        QNetworkReply* reply = m_nam.sendCustomRequest(req, "PATCH", QJsonDocument(payload).toJson(QJsonDocument::Compact));
        handleReply(reply);
    } else if (listEntry.provider == QStringLiteral("GitLab")) {
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
    } else if (listEntry.provider == QStringLiteral("Todoist")) {
        QString tdTaskId = issueKey;
        if (tdTaskId.startsWith(QStringLiteral("TD#"))) tdTaskId = tdTaskId.mid(3);
        if (tdTaskId.isEmpty()) return;

        const QString action = done ? QStringLiteral("close") : QStringLiteral("reopen");
        QUrl url(QStringLiteral("https://api.todoist.com/rest/v2/tasks/") + tdTaskId + QLatin1Char('/') + action);
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);
        QNetworkReply* reply = m_nam.post(req, QByteArray());
        handleReply(reply);
    } else if (listEntry.provider == QStringLiteral("Redmine")) {
        QString numStr;
        if (issueKey.contains(QLatin1Char('#'))) numStr = issueKey.split(QLatin1Char('#')).last();
        if (numStr.isEmpty()) {
            for (const QChar& c : issueKey) if (c.isDigit()) numStr.append(c);
        }
        if (numStr.isEmpty()) return;

        QUrl url(baseUrl + QStringLiteral("/issues/") + numStr + QStringLiteral(".json"));
        QNetworkRequest req(url);
        applyAuthHeader(req, listEntry);

        const int statusId = resolveRedmineStatusId(listEntry.statusMapping, done);

        QJsonObject payload{
            {QStringLiteral("issue"), QJsonObject{{QStringLiteral("status_id"), statusId}}}
        };
        QNetworkReply* reply = m_nam.sendCustomRequest(req, "PUT", QJsonDocument(payload).toJson(QJsonDocument::Compact));
        handleReply(reply);
    } else if (listEntry.provider == QStringLiteral("Jira")) {
        const bool isCloud = baseUrl.contains(QStringLiteral(".atlassian.net"), Qt::CaseInsensitive);
        const QString apiVer = isCloud ? QStringLiteral("3") : QStringLiteral("2");
        const QUrl transUrl(baseUrl + QStringLiteral("/rest/api/") + apiVer + QStringLiteral("/issue/") + issueKey.trimmed() + QStringLiteral("/transitions"));
        QNetworkRequest req(transUrl);
        applyAuthHeader(req, listEntry, forceBearerAuth);

        QNetworkReply* reply = m_nam.get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, listEntry, issueKey, done, localStatus, transUrl, forceBearerAuth]() {
            reply->deleteLater();
            const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray seraph = reply->rawHeader("X-Seraph-Loginreason");
            const bool authErr = (httpStatus == 401 || httpStatus == 403 || seraph.contains("AUTHENTICATED_FAILED"));
            auto fail = [this, &listEntry](const QString& errMsg) {
                setStatusMessage(errMsg);
                emit syncFailed(listEntry.id, errMsg);
            };
            if (reply->error() != QNetworkReply::NoError || authErr) {
                if (forceBearerAuth) {
                    m_jiraBearerTokens.remove(listEntry.id);
                }
                if (!forceBearerAuth && !listEntry.jiraUser.trimmed().isEmpty() && authErr) {
                    updateIssueStatusRemote(listEntry, issueKey, done, localStatus, true);
                    return;
                }
                fail(tr("Failed to update status for %1 on %2 (%3): %4")
                         .arg(issueKey, listEntry.provider).arg(httpStatus).arg(reply->errorString()));
                return;
            }

            // Reopening goes to the "To Do" category (or "In Progress" when the local status is
            // in_progress); completing goes to the "done" category. Category matches win over
            // name matches, and there is deliberately no fallback to another category.
            const QString wantedCategory = done ? QStringLiteral("done")
                : (localStatus == QStringLiteral("in_progress") ? QStringLiteral("indeterminate") : QStringLiteral("new"));
            const QStringList toNames = done
                ? QStringList{QStringLiteral("done"), QStringLiteral("closed"), QStringLiteral("resolved")}
                : (wantedCategory == QStringLiteral("indeterminate")
                       ? QStringList{QStringLiteral("in progress")}
                       : QStringList{QStringLiteral("to do"), QStringLiteral("open"), QStringLiteral("reopened"), QStringLiteral("backlog")});

            const QJsonArray transitions = QJsonDocument::fromJson(reply->readAll()).object()
                                               .value(QStringLiteral("transitions")).toArray();
            QString selectedTransitionId;
            for (const QJsonValue& val : transitions) {
                const QJsonObject toObj = val.toObject().value(QStringLiteral("to")).toObject();
                const QString catKey = toObj.value(QStringLiteral("statusCategory")).toObject()
                                           .value(QStringLiteral("key")).toString().toLower();
                if (catKey == wantedCategory) {
                    selectedTransitionId = val.toObject().value(QStringLiteral("id")).toString();
                    break;
                }
            }
            if (selectedTransitionId.isEmpty()) {
                for (const QJsonValue& val : transitions) {
                    const QString toName = val.toObject().value(QStringLiteral("to")).toObject()
                                               .value(QStringLiteral("name")).toString().toLower();
                    if (toNames.contains(toName)) {
                        selectedTransitionId = val.toObject().value(QStringLiteral("id")).toString();
                        break;
                    }
                }
            }
            if (selectedTransitionId.isEmpty()) {
                fail(tr("No matching Jira transition to update status of %1; status was not changed on %2.")
                         .arg(issueKey, listEntry.provider));
                return;
            }

            QNetworkRequest postReq(transUrl);
            applyAuthHeader(postReq, listEntry, forceBearerAuth);
            postReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
            const QJsonObject payload{{QStringLiteral("transition"), QJsonObject{{QStringLiteral("id"), selectedTransitionId}}}};
            QNetworkReply* postReply = m_nam.post(postReq, QJsonDocument(payload).toJson(QJsonDocument::Compact));
            connect(postReply, &QNetworkReply::finished, this, [this, postReply, listEntry, issueKey, done, forceBearerAuth]() {
                postReply->deleteLater();
                const int status = postReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (postReply->error() != QNetworkReply::NoError || (status >= 400 && status < 600)) {
                    const QString errMsg = tr("Failed to update status for %1 on %2 (%3): %4")
                        .arg(issueKey, listEntry.provider).arg(status).arg(postReply->errorString());
                    setStatusMessage(errMsg);
                    emit syncFailed(listEntry.id, errMsg);
                    return;
                }
                if (forceBearerAuth) {
                    m_jiraBearerTokens[listEntry.id] = listEntry.apiToken;
                }
                setStatusMessage(done ? tr("Updated %1 on %2: closed").arg(issueKey, listEntry.provider)
                                      : tr("Updated %1 on %2: open").arg(issueKey, listEntry.provider));
            });
        });
    } else if (listEntry.provider == QStringLiteral("YouTrack")) {
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
    } else if (listEntry.provider == QStringLiteral("Nextcloud")) {
        QString uid = issueKey.trimmed();
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
        connect(getReply, &QNetworkReply::finished, this, [this, getReply, req, issueKey, done, handleReply, failWithError]() {
            getReply->deleteLater();
            const int statusCode = getReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (getReply->error() != QNetworkReply::NoError || (statusCode >= 400 && statusCode < 600)) {
                failWithError(statusCode, getReply->errorString());
                return;
            }
            QString ics = QString::fromUtf8(getReply->readAll());
            static const QRegularExpression vtodoStartRe(QStringLiteral("(?:^|\r?\n)(BEGIN:VTODO)(?:\r?\n|$)"));
            const auto vtodoStartMatch = vtodoStartRe.match(ics);
            if (!vtodoStartMatch.hasMatch()) {
                failWithError(statusCode, tr("Invalid CalDAV VTODO response"));
                return;
            }
            const int vtodoStart = vtodoStartMatch.capturedStart(1);
            const int vtodoPropsStart = vtodoStartMatch.capturedEnd(1);

            static const QRegularExpression vtodoEndRe(QStringLiteral("\r?\n(END:VTODO)(?:\r?\n|$)"));
            const auto vtodoEndMatch = vtodoEndRe.match(ics, vtodoPropsStart);
            if (!vtodoEndMatch.hasMatch()) {
                failWithError(statusCode, tr("Invalid CalDAV VTODO response"));
                return;
            }
            const int vtodoEnd = vtodoEndMatch.capturedStart(1);

            int propBlockEnd = vtodoEnd;
            static const QRegularExpression subBeginRe(QStringLiteral("\r?\n(BEGIN:[^\r\n]*)"));
            const auto subBeginMatch = subBeginRe.match(ics, vtodoPropsStart);
            if (subBeginMatch.hasMatch() && subBeginMatch.capturedStart(1) < propBlockEnd) {
                propBlockEnd = subBeginMatch.capturedStart(1);
            }

            // Rewrite STATUS together with COMPLETED / PERCENT-COMPLETE inside the VTODO property
            // block only: other CalDAV clients read all three to decide whether a task is done.
            const QString nl = ics.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n") : QStringLiteral("\n");
            const QString block = ics.mid(vtodoPropsStart, propBlockEnd - vtodoPropsStart);
            static const QRegularExpression managedRe(QStringLiteral("^(STATUS|COMPLETED|PERCENT-COMPLETE)[;:]"),
                                                      QRegularExpression::CaseInsensitiveOption);
            QStringList kept;
            bool skippingFolded = false;
            for (const QString& line : block.split(nl)) {
                if (!line.isEmpty() && (line.at(0) == QLatin1Char(' ') || line.at(0) == QLatin1Char('\t'))) {
                    if (!skippingFolded) kept << line;
                    continue;
                }
                skippingFolded = managedRe.match(line).hasMatch();
                if (!skippingFolded) kept << line;
            }
            QStringList added{done ? QStringLiteral("STATUS:COMPLETED") : QStringLiteral("STATUS:NEEDS-ACTION")};
            if (done) {
                added << QStringLiteral("COMPLETED:") + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
                added << QStringLiteral("PERCENT-COMPLETE:100");
            }
            int insertAt = (!kept.isEmpty() && kept.last().isEmpty()) ? static_cast<int>(kept.size()) - 1 : static_cast<int>(kept.size());
            for (const QString& prop : added) kept.insert(insertAt++, prop);
            ics.replace(vtodoPropsStart, propBlockEnd - vtodoPropsStart, kept.join(nl));

            QNetworkRequest putReq = req;
            putReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/calendar; charset=utf-8"));
            QNetworkReply* putReply = m_nam.put(putReq, ics.toUtf8());
            handleReply(putReply);
        });
    }
}

void IssueTrackerService::postIssueComment(const TodoListEntry& listEntry,
                                           const QString& issueKey,
                                           const QString& markdownComment,
                                           bool forceBearerAuth,
                                           bool useV2Fallback)
{
    if (QStandardPaths::isTestModeEnabled()) return;
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
                if (forceBearerAuth) {
                    m_jiraBearerTokens.remove(listEntry.id);
                }
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
            if (forceBearerAuth) {
                m_jiraBearerTokens[listEntry.id] = listEntry.apiToken;
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
            static const QRegularExpression vtodoStartRe(QStringLiteral("(?:^|\r?\n)(BEGIN:VTODO)(?:\r?\n|$)"));
            const auto vtodoStartMatch = vtodoStartRe.match(ics);
            if (!vtodoStartMatch.hasMatch()) return;
            const int vtodoPropsStart = vtodoStartMatch.capturedEnd(1);

            static const QRegularExpression vtodoEndRe(QStringLiteral("\r?\n(END:VTODO)(?:\r?\n|$)"));
            const auto vtodoEndMatch = vtodoEndRe.match(ics, vtodoPropsStart);
            if (!vtodoEndMatch.hasMatch()) return;
            const int vtodoEnd = vtodoEndMatch.capturedStart(1);

            int insertPos = vtodoEnd;
            static const QRegularExpression subBeginRe(QStringLiteral("\r?\n(BEGIN:[^\r\n]*)"));
            const auto subBeginMatch = subBeginRe.match(ics, vtodoPropsStart);
            if (subBeginMatch.hasMatch() && subBeginMatch.capturedStart(1) < insertPos) {
                insertPos = subBeginMatch.capturedStart(1);
            }
            const QString nl = ics.contains(QStringLiteral("\r\n")) ? QStringLiteral("\r\n") : QStringLiteral("\n");
            const QString commentLine = QStringLiteral("COMMENT:%1%2").arg(escapedComment, nl);
            ics.insert(insertPos, commentLine);
            QNetworkRequest putReq = req;
            putReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("text/calendar; charset=utf-8"));
            QNetworkReply* putReply = m_nam.put(putReq, ics.toUtf8());
            connect(putReply, &QNetworkReply::finished, this, [this, putReply, issueKey]() {
                putReply->deleteLater();
                if (putReply->error() == QNetworkReply::NoError) {
                    setStatusMessage(tr("Posted comment to Nextcloud task %1.").arg(issueKey));
                }
            });
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
