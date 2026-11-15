#pragma once

#include "IssueTrackerService.h"
#include <QRegularExpression>

namespace gofi {

inline QString sanitizeTagToken(const QString& raw) {
    QString out = raw.trimmed();
    static const QRegularExpression wsRe(QStringLiteral("\\s+"));
    out.replace(wsRe, QStringLiteral("_"));
    return out;
}

inline QString unescapeIcsText(const QString& str) {
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

inline QString escapeIcsText(const QString& str) {
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

inline int resolveRedmineStatusId(const QVariantMap& statusMapping, bool done) {
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

} // namespace gofi
