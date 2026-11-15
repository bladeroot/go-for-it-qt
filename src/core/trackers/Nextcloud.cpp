#include "trackers/NextcloudProvider.h"
#include "trackers/TrackerCommon.h"
#include "DatabaseManager.h"
#include <QDateTime>
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

void IssueTrackerService::postNextcloudWorklog(const TodoListEntry& listEntry, qint64 activityLogId, const QString& issueKey, quint32 runtimeSeconds) {
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

void IssueTrackerService::updateNextcloudDescription(const TodoListEntry& listEntry, const QString& issueKey, const QString& markdownDetails) {
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

void IssueTrackerService::updateNextcloudStatusRemote(const TodoListEntry& listEntry,
                                                      const QString& issueKey,
                                                      bool done,
                                                      const std::function<void(QNetworkReply*)>& handleReply,
                                                      const std::function<void(int, const QString&)>& failWithError) {
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

void IssueTrackerService::postNextcloudComment(const TodoListEntry& listEntry, const QString& issueKey, const QString& markdownComment) {
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

QUrl IssueTrackerService::buildNextcloudBrowserUrl(const TodoListEntry& listEntry, const QString& /*issueKey*/) {
    const QString baseUrl = normalizeBaseUrl(listEntry.jiraUrl);
    if (!baseUrl.isEmpty()) {
        return QUrl(baseUrl + QStringLiteral("/apps/tasks/"));
    }
    return QUrl();
}

void NextcloudProvider::syncList(const TodoListEntry& entry) {
    service()->fetchNextcloudTasks(entry);
}

void NextcloudProvider::postWorklog(const TodoListEntry& entry,
                                    qint64,
                                    qint64 activityLogId,
                                    const QString& issueKey,
                                    const QDateTime&,
                                    quint32 runtimeSeconds,
                                    bool,
                                    const QString&) {
    service()->postNextcloudWorklog(entry, activityLogId, issueKey, runtimeSeconds);
}

void NextcloudProvider::updateDescription(const TodoListEntry& entry,
                                          const QString& issueKey,
                                          const QString& markdownDetails,
                                          bool,
                                          bool,
                                          const QString&) {
    service()->updateNextcloudDescription(entry, issueKey, markdownDetails);
}

void NextcloudProvider::updateStatusRemote(const TodoListEntry& entry,
                                           const QString& issueKey,
                                           bool done,
                                           const QString&,
                                           bool,
                                           const QString&,
                                           const StatusReplyHandler& handleReply,
                                           const StatusErrorHandler& failWithError) {
    service()->updateNextcloudStatusRemote(entry, issueKey, done, handleReply, failWithError);
}

void NextcloudProvider::postComment(const TodoListEntry& entry,
                                    const QString& issueKey,
                                    const QString& markdownComment,
                                    bool,
                                    bool,
                                    const QString&) {
    service()->postNextcloudComment(entry, issueKey, markdownComment);
}

void NextcloudProvider::fetchRemoteStatuses(const QString&,
                                            const TodoListEntry&,
                                            const QVariantMap& existingMapping) {
    emit service()->remoteStatusesLoaded(IssueTrackerService::parseNextcloudStatuses(existingMapping));
}

void NextcloudProvider::fetchRemotePriorities(const QString&,
                                              const TodoListEntry&,
                                              const QVariantMap& existingMapping) {
    emit service()->remotePrioritiesLoaded(defaultPriorities(existingMapping));
}

QVariantList NextcloudProvider::defaultPriorities(const QVariantMap& existingMapping) const {
    return service()->defaultPrioritiesForProvider(name(), existingMapping);
}

QUrl NextcloudProvider::buildBrowserUrl(const TodoListEntry& entry, const QString& issueKey) const {
    return IssueTrackerService::buildNextcloudBrowserUrl(entry, issueKey);
}

} // namespace gofi
