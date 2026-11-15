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

} // namespace gofi
