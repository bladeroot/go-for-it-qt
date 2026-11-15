#include "trackers/TrackerCommon.h"
#include "todotxt/TxtTask.h"
#include <QRegularExpression>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace gofi {

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

} // namespace gofi
