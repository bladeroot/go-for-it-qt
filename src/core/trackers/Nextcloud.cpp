#include "trackers/TrackerCommon.h"
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrl>
#include <QXmlStreamReader>

namespace gofi {

namespace {

struct CalDavMultistatusInfo {
    bool isMultistatus = false;
    int objectResponseCount = 0;
    int successfulCalendarDataCount = 0;
};

CalDavMultistatusInfo inspectCalDavMultistatus(int statusCode, const QByteArray& bodyBytes) {
    CalDavMultistatusInfo info;
    if (statusCode == 200 && (bodyBytes.contains("BEGIN:VCALENDAR") || bodyBytes.contains("multistatus"))) {
        info.isMultistatus = true;
        if (bodyBytes.contains("BEGIN:VTODO") || bodyBytes.contains("BEGIN:VEVENT")) {
            info.successfulCalendarDataCount = 1;
            info.objectResponseCount = 1;
        }
        return info;
    }
    if (statusCode != 207) {
        return info;
    }

    QXmlStreamReader xml(bodyBytes);
    bool inResponse = false;
    bool inPropstat = false;
    bool inProp = false;
    bool hasCalendarData = false;
    QString currentHref;
    QString currentStatus;

    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = xml.name().toString();
            if (name.compare(QLatin1String("multistatus"), Qt::CaseInsensitive) == 0) {
                info.isMultistatus = true;
            } else if (name.compare(QLatin1String("response"), Qt::CaseInsensitive) == 0) {
                inResponse = true;
                currentHref.clear();
            } else if (name.compare(QLatin1String("href"), Qt::CaseInsensitive) == 0 && inResponse && !inPropstat) {
                currentHref = xml.readElementText().trimmed();
            } else if (name.compare(QLatin1String("propstat"), Qt::CaseInsensitive) == 0) {
                inPropstat = true;
                currentStatus.clear();
                hasCalendarData = false;
            } else if (name.compare(QLatin1String("prop"), Qt::CaseInsensitive) == 0) {
                inProp = true;
            } else if (name.compare(QLatin1String("calendar-data"), Qt::CaseInsensitive) == 0 && inProp) {
                hasCalendarData = true;
            } else if (name.compare(QLatin1String("status"), Qt::CaseInsensitive) == 0 && inPropstat) {
                currentStatus = xml.readElementText().trimmed();
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QString name = xml.name().toString();
            if (name.compare(QLatin1String("propstat"), Qt::CaseInsensitive) == 0) {
                inPropstat = false;
                if (hasCalendarData && currentStatus.contains(QLatin1String("200"))) {
                    info.successfulCalendarDataCount++;
                }
            } else if (name.compare(QLatin1String("prop"), Qt::CaseInsensitive) == 0) {
                inProp = false;
            } else if (name.compare(QLatin1String("response"), Qt::CaseInsensitive) == 0) {
                inResponse = false;
                if (!currentHref.isEmpty() && !currentHref.endsWith(QLatin1Char('/'))) {
                    info.objectResponseCount++;
                }
            }
        }
    }
    return info;
}

} // namespace

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
            const int getStatusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() != QNetworkReply::NoError) {
                endSyncOperation();
                const QString errMsg = tr("Nextcloud sync error: %1").arg(reply->errorString());
                setStatusMessage(errMsg);
                emit syncFailed(entry.id, errMsg);
                return;
            }
            const QByteArray bodyBytes = reply->readAll();
            if (getStatusCode != 200 || !bodyBytes.contains("BEGIN:VCALENDAR")) {
                endSyncOperation();
                const QString errMsg = tr("Nextcloud returned non-CalDAV response (%1).").arg(getStatusCode);
                setStatusMessage(errMsg);
                emit syncFailed(entry.id, errMsg);
                return;
            }
            const QVector<RemoteIssueItem> items = parseNextcloudTasksIcs(bodyBytes);
            processSyncedItems(entry, items, QStringLiteral("Nextcloud"), false, false, false);
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
                    const int propStatusCode = propReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    if (propReply->error() != QNetworkReply::NoError) {
                        endSyncOperation();
                        const QString errMsg = tr("Nextcloud CalDAV error: %1").arg(propReply->errorString());
                        setStatusMessage(errMsg);
                        emit syncFailed(entry.id, errMsg);
                        return;
                    }
                    const QByteArray bodyBytes = propReply->readAll();
                    const CalDavMultistatusInfo info = inspectCalDavMultistatus(propStatusCode, bodyBytes);
                    if (!info.isMultistatus) {
                        endSyncOperation();
                        const QString errMsg = tr("Nextcloud returned non-CalDAV response (%1).").arg(propStatusCode);
                        setStatusMessage(errMsg);
                        emit syncFailed(entry.id, errMsg);
                        return;
                    }
                    const QVector<RemoteIssueItem> items = parseNextcloudTasksIcs(bodyBytes);
                    const bool hasError = (info.successfulCalendarDataCount < info.objectResponseCount) ||
                                          (info.objectResponseCount > 0 && items.isEmpty());
                    const bool explicitZero = (info.objectResponseCount == 0 && items.isEmpty());
                    processSyncedItems(entry, items, QStringLiteral("Nextcloud"), false, hasError, explicitZero);
                });
                return;
            }

            endSyncOperation();
            const QString errMsg = tr("Nextcloud CalDAV sync error (%1): %2").arg(statusCode).arg(reply->errorString());
            setStatusMessage(errMsg);
            emit syncFailed(entry.id, errMsg);
            return;
        }

        const QByteArray bodyBytes = reply->readAll();
        const CalDavMultistatusInfo info = inspectCalDavMultistatus(statusCode, bodyBytes);
        if (!info.isMultistatus) {
            endSyncOperation();
            const QString errMsg = tr("Nextcloud returned non-CalDAV response (%1).").arg(statusCode);
            setStatusMessage(errMsg);
            emit syncFailed(entry.id, errMsg);
            return;
        }
        const QVector<RemoteIssueItem> items = parseNextcloudTasksIcs(bodyBytes);
        const bool hasError = (info.successfulCalendarDataCount < info.objectResponseCount) ||
                              (info.objectResponseCount > 0 && items.isEmpty());
        const bool explicitZero = (info.objectResponseCount == 0 && items.isEmpty());
        processSyncedItems(entry, items, QStringLiteral("Nextcloud"), false, hasError, explicitZero);
    });
}

} // namespace gofi
