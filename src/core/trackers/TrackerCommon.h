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

} // namespace gofi
