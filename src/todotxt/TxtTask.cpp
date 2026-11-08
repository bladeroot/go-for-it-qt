#include "TxtTask.h"
#include <QRegularExpression>
#include <QStringList>
#include <QVariantMap>

namespace gofi {

TxtTask::TxtTask(const QString& simpleLine, bool done)
    : m_status(done ? QStringLiteral("done") : QStringLiteral("todo"))
    , m_done(done)
{
    const QStringList tokens = simpleLine.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    parseDescriptionTokens(tokens, 0);
}

TxtTask TxtTask::fromSimpleTxt(const QString& line, bool done, bool addCreationDate) {
    TxtTask task;
    task.m_done = done;
    task.m_status = done ? QStringLiteral("done") : QStringLiteral("todo");
    if (addCreationDate) {
        task.m_creationDate = QDate::currentDate();
    }
    task.updateFromSimpleTxt(line);
    return task;
}

TxtTask TxtTask::fromTodoTxt(const QString& line, bool doneByDefault) {
    TxtTask task;
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return task;
    }

    const QStringList tokens = trimmed.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    int idx = 0;

    if (idx < tokens.size() && tokens[idx] == QStringLiteral("x")) {
        task.m_done = true;
        ++idx;
    } else {
        task.m_done = doneByDefault;
    }

    if (idx < tokens.size() && isPriorityToken(tokens[idx])) {
        task.m_priority = static_cast<quint8>(tokens[idx].at(1).unicode() - 'A');
        ++idx;
    } else {
        task.m_priority = NoPriority;
    }

    QDate date1;
    QDate date2;
    if (idx < tokens.size() && isDateToken(tokens[idx])) {
        date1 = QDate::fromString(tokens[idx], Qt::ISODate);
        ++idx;
        if (task.m_done && idx < tokens.size() && isDateToken(tokens[idx])) {
            date2 = QDate::fromString(tokens[idx], Qt::ISODate);
            ++idx;
        }
    }

    if (date1.isValid() && task.m_done && date2.isValid()) {
        task.m_completionDate = date1;
        task.m_creationDate = date2;
    } else {
        task.m_creationDate = date1;
        task.m_completionDate = QDate();
    }

    task.m_status = task.m_done ? QStringLiteral("done") : QStringLiteral("todo");
    task.parseDescriptionTokens(tokens, idx);
    return task;
}

TxtTask TxtTask::fromDatabase(qint64 id,
                              const QString& description,
                              bool done,
                              const QString& status,
                              quint8 priority,
                              const QDate& creationDate,
                              const QDate& completionDate,
                              quint32 duration,
                              quint32 timerValue,
                              const QString& jiraKey,
                              quint32 jiraSyncedSeconds,
                              const QString& details,
                              const QVariantList& comments,
                              bool isViewed,
                              const QString& listId,
                              const QString& listName,
                              const QString& listProvider)
{
    TxtTask task;
    task.m_id = id;
    task.m_status = status.isEmpty() ? (done ? QStringLiteral("done") : QStringLiteral("todo")) : status;
    task.m_done = isClosedStatus(task.m_status);
    task.m_priority = priority;
    task.m_creationDate = creationDate;
    task.m_completionDate = completionDate;
    const QStringList tokens = description.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    task.parseDescriptionTokens(tokens, 0);
    task.m_duration = duration;
    task.m_timerValue = timerValue;
    task.m_jiraKey = jiraKey;
    task.m_jiraSyncedSeconds = jiraSyncedSeconds;
    task.m_details = details;
    task.m_comments = comments;
    task.m_isViewed = isViewed;
    task.m_listId = listId;
    task.m_listName = listName;
    task.m_listProvider = listProvider;
    return task;
}

QString TxtTask::normalizeStatus(const QString& status) {
    const QString norm = status.trimmed().toLower();
    return norm.isEmpty() ? QStringLiteral("todo") : norm;
}

bool TxtTask::isClosedStatus(const QString& status) {
    const QString norm = normalizeStatus(status);
    return norm == QStringLiteral("done") || norm == QStringLiteral("canceled");
}

void TxtTask::setDone(bool done) {
    if (m_done == done && (done ? isClosedStatus(m_status) : !isClosedStatus(m_status))) {
        return;
    }
    m_done = done;
    m_status = done ? QStringLiteral("done") : QStringLiteral("todo");
    if (m_done && m_creationDate.isValid()) {
        m_completionDate = QDate::currentDate();
    } else if (!m_done) {
        m_completionDate = QDate();
    }
}

void TxtTask::setStatus(const QString& status) {
    const QString norm = normalizeStatus(status);
    m_status = norm;
    const bool closed = isClosedStatus(norm);
    if (m_done != closed) {
        m_done = closed;
        if (m_done && m_creationDate.isValid()) {
            m_completionDate = QDate::currentDate();
        } else if (!m_done) {
            m_completionDate = QDate();
        }
    }
}

QString TxtTask::priorityString() const {
    if (m_priority >= NoPriority) {
        return {};
    }
    return QStringLiteral("(%1)").arg(QChar('A' + m_priority));
}

void TxtTask::updateFromSimpleTxt(const QString& line) {
    const QStringList tokens = line.trimmed().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        m_description.clear();
        m_parts.clear();
        m_priority = NoPriority;
        return;
    }

    int idx = 0;
    if (isPriorityToken(tokens[0])) {
        m_priority = static_cast<quint8>(tokens[0].at(1).unicode() - 'A');
        ++idx;
    } else {
        m_priority = NoPriority;
    }

    m_duration = 0;
    parseDescriptionTokens(tokens, idx);
}

QVariantList TxtTask::tokensVariantList() const {
    QVariantList result;
    result.reserve(m_parts.size());
    for (const TxtPart& p : m_parts) {
        QVariantMap item;
        switch (p.type) {
        case TxtPartType::Project:
            item[QStringLiteral("type")] = QStringLiteral("project");
            item[QStringLiteral("text")] = QStringLiteral("+") + p.content;
            item[QStringLiteral("value")] = p.content;
            break;
        case TxtPartType::Context:
            item[QStringLiteral("type")] = QStringLiteral("context");
            item[QStringLiteral("text")] = QStringLiteral("@") + p.content;
            item[QStringLiteral("value")] = p.content;
            break;
        case TxtPartType::Tag:
            item[QStringLiteral("type")] = QStringLiteral("tag");
            item[QStringLiteral("text")] = p.tagName + QLatin1Char(':') + p.content;
            item[QStringLiteral("value")] = p.content;
            break;
        case TxtPartType::Uri:
            item[QStringLiteral("type")] = QStringLiteral("uri");
            item[QStringLiteral("text")] = p.tagName.isEmpty() ? p.content : (p.tagName + QLatin1Char(':') + p.content);
            item[QStringLiteral("value")] = p.content;
            break;
        case TxtPartType::Word:
        default:
            item[QStringLiteral("type")] = QStringLiteral("word");
            item[QStringLiteral("text")] = p.content;
            item[QStringLiteral("value")] = p.content;
            break;
        }
        result.append(item);
    }
    return result;
}

QString TxtTask::richText() const {
    QStringList segments;
    segments.reserve(m_parts.size());
    for (const TxtPart& p : m_parts) {
        switch (p.type) {
        case TxtPartType::Project: {
            const QString esc = (QLatin1Char('+') + p.content).toHtmlEscaped();
            segments.append(QStringLiteral("<a href=\"%1\" style=\"color:#c0392b;text-decoration:none;font-weight:bold;\">%1</a>").arg(esc));
            break;
        }
        case TxtPartType::Context: {
            const QString esc = (QLatin1Char('@') + p.content).toHtmlEscaped();
            segments.append(QStringLiteral("<a href=\"%1\" style=\"color:#2980b9;text-decoration:none;font-weight:bold;\">%1</a>").arg(esc));
            break;
        }
        case TxtPartType::Uri: {
            const QString fullUri = p.tagName.isEmpty() ? p.content : (p.tagName + QLatin1Char(':') + p.content);
            const QString esc = fullUri.toHtmlEscaped();
            segments.append(QStringLiteral("<a href=\"%1\">%1</a>").arg(esc));
            break;
        }
        case TxtPartType::Tag: {
            if (p.tagName == QStringLiteral("due")) {
                const QDate d = QDate::fromString(p.content, Qt::ISODate);
                QString color = QStringLiteral("#7f8c8d");
                if (d.isValid()) {
                    if (d < QDate::currentDate()) {
                        color = QStringLiteral("#e74c3c"); // Overdue red
                    } else if (d == QDate::currentDate()) {
                        color = QStringLiteral("#e67e22"); // Today orange
                    } else if (d <= QDate::currentDate().addDays(7)) {
                        color = QStringLiteral("#2980b9"); // This week blue
                    }
                }
                segments.append(QStringLiteral("<span style=\"color:%1;font-weight:bold;\">due:%2</span>").arg(color, p.content.toHtmlEscaped()));
                break;
            }
            const QString esc = (p.tagName + QLatin1Char(':') + p.content).toHtmlEscaped();
            segments.append(esc);
            break;
        }
        case TxtPartType::Word:
        default:
            segments.append(p.content.toHtmlEscaped());
            break;
        }
    }
    return segments.join(QLatin1Char(' '));
}

QString TxtTask::toSimpleTxt() const {
    QString out;
    if (m_priority < NoPriority) {
        out += QStringLiteral("(%1) ").arg(QChar('A' + m_priority));
    }
    out += m_description;
    if (m_duration > 0) {
        out += QStringLiteral(" duration:") + formatDurationValue(m_duration);
    }
    return out;
}

QString TxtTask::toTodoTxt(bool logTimer) const {
    QString out;
    if (m_done) {
        out += QStringLiteral("x ");
    }
    if (m_priority < NoPriority) {
        out += QStringLiteral("(%1) ").arg(QChar('A' + m_priority));
    }
    if (m_creationDate.isValid()) {
        if (m_done && m_completionDate.isValid()) {
            out += m_completionDate.toString(Qt::ISODate) + QLatin1Char(' ');
        }
        out += m_creationDate.toString(Qt::ISODate) + QLatin1Char(' ');
    }
    out += m_description;
    if (logTimer && m_timerValue > 0) {
        out += QStringLiteral(" timer:") + formatTimerValue(m_timerValue);
    }
    if (m_duration > 0) {
        out += QStringLiteral(" duration:") + formatDurationValue(m_duration);
    }
    return out;
}

int TxtTask::compare(const TxtTask& other) const {
    if (m_priority != other.m_priority) {
        if (m_priority == NoPriority) return 1;
        if (other.m_priority == NoPriority) return -1;
        return (m_priority < other.m_priority) ? -1 : 1;
    }

    int cmp = QString::compare(m_description, other.m_description, Qt::CaseInsensitive);
    if (cmp != 0) return cmp;

    cmp = QString::compare(m_description, other.m_description, Qt::CaseSensitive);
    if (cmp != 0) return cmp;

    if (m_creationDate.isValid() && other.m_creationDate.isValid() && m_creationDate != other.m_creationDate) {
        return (m_creationDate < other.m_creationDate) ? -1 : 1;
    }
    return 0;
}

bool TxtTask::parseDurationValue(const QString& str, quint32& outSeconds) {
    if (str.isEmpty()) {
        return false;
    }
    static const QRegularExpression re(
        QStringLiteral("^(?:(\\d+)[hH])?(?:-?(\\d+)[mM])?(?:-?(\\d+)[sS])?$"));
    const QRegularExpressionMatch match = re.match(str);
    if (!match.hasMatch()) {
        return false;
    }
    if (match.captured(1).isEmpty() && match.captured(2).isEmpty() && match.captured(3).isEmpty()) {
        return false;
    }

    quint32 total = 0;
    if (!match.captured(1).isEmpty()) {
        total += match.captured(1).toUInt() * 3600U;
    }
    if (!match.captured(2).isEmpty()) {
        total += match.captured(2).toUInt() * 60U;
    }
    if (!match.captured(3).isEmpty()) {
        total += match.captured(3).toUInt();
    }
    outSeconds = total;
    return true;
}

QString TxtTask::formatDurationValue(quint32 seconds) {
    const quint32 h = seconds / 3600U;
    const quint32 m = (seconds / 60U) % 60U;
    const quint32 s = seconds % 60U;

    QStringList parts;
    if (h > 0) parts << QStringLiteral("%1h").arg(h);
    if (m > 0) parts << QStringLiteral("%1m").arg(m);
    if (s > 0) parts << QStringLiteral("%1s").arg(s);
    return parts.join(QLatin1Char('-'));
}

QString TxtTask::formatTimerValue(quint32 seconds) {
    const quint32 h = seconds / 3600U;
    const quint32 m = (seconds / 60U) % 60U;
    const quint32 s = seconds % 60U;
    return QStringLiteral("%1h-%2m-%3s").arg(h).arg(m).arg(s);
}

bool TxtTask::isDateToken(const QString& token) {
    if (token.size() != 10) return false;
    static const QRegularExpression re(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
    return re.match(token).hasMatch() && QDate::fromString(token, Qt::ISODate).isValid();
}

bool TxtTask::isPriorityToken(const QString& token) {
    return token.size() == 3
        && token.at(0) == QLatin1Char('(')
        && token.at(1) >= QLatin1Char('A') && token.at(1) <= QLatin1Char('Z')
        && token.at(2) == QLatin1Char(')');
}

bool TxtTask::isProjectToken(const QString& token) {
    return token.size() >= 2 && token.at(0) == QLatin1Char('+') && !token.at(1).isSpace();
}

bool TxtTask::isContextToken(const QString& token) {
    return token.size() >= 2 && token.at(0) == QLatin1Char('@') && !token.at(1).isSpace();
}

bool TxtTask::isCommonUriScheme(const QString& scheme) {
    return isSafeUriScheme(scheme);
}

bool TxtTask::isSafeUriScheme(const QString& scheme) {
    const QString s = scheme.toLower();
    return s == QStringLiteral("http")
        || s == QStringLiteral("https")
        || s == QStringLiteral("mailto");
}

TxtPart TxtTask::tokenizePart(const QString& token) {
    if (isProjectToken(token)) {
        return {TxtPartType::Project, token.mid(1), {}};
    }
    if (isContextToken(token)) {
        return {TxtPartType::Context, token.mid(1), {}};
    }
    const int colonPos = token.indexOf(QLatin1Char(':'));
    if (colonPos > 0 && colonPos + 1 < token.size() && !token.at(colonPos + 1).isSpace()) {
        const QString key = token.left(colonPos);
        const QString val = token.mid(colonPos + 1);
        const QString lowerKey = key.toLower();
        if ((lowerKey == QStringLiteral("mailto") && !val.isEmpty())
            || ((lowerKey == QStringLiteral("http") || lowerKey == QStringLiteral("https"))
                && val.startsWith(QStringLiteral("//")) && val.size() > 2)) {
            return {TxtPartType::Uri, val, key};
        }
        if (!val.contains(QLatin1Char(':'))) {
            return {TxtPartType::Tag, val, key};
        }
    }
    return {TxtPartType::Word, token, {}};
}

void TxtTask::parseDescriptionTokens(const QStringList& tokens, int startIndex) {
    m_parts.clear();
    for (int i = startIndex; i < tokens.size(); ++i) {
        TxtPart part = tokenizePart(tokens[i]);
        if (part.type == TxtPartType::Tag) {
            if (part.tagName == QStringLiteral("timer")) {
                quint32 parsedTimer = 0;
                if (parseDurationValue(part.content, parsedTimer)) {
                    m_timerValue = parsedTimer;
                    continue;
                }
            } else if (part.tagName == QStringLiteral("duration")) {
                quint32 parsedDuration = 0;
                if (parseDurationValue(part.content, parsedDuration)) {
                    m_duration = parsedDuration;
                    continue;
                }
            } else if (part.tagName == QStringLiteral("due")) {
                const QDate d = QDate::fromString(part.content, Qt::ISODate);
                if (d.isValid()) {
                    m_dueDate = d;
                }
            }
        }
        m_parts.append(std::move(part));
    }
    rebuildDescriptionFromParts();
}

void TxtTask::rebuildDescriptionFromParts() {
    QStringList segments;
    segments.reserve(m_parts.size());
    for (const TxtPart& p : m_parts) {
        switch (p.type) {
        case TxtPartType::Project:
            segments.append(QLatin1Char('+') + p.content);
            break;
        case TxtPartType::Context:
            segments.append(QLatin1Char('@') + p.content);
            break;
        case TxtPartType::Tag:
            segments.append(p.tagName + QLatin1Char(':') + p.content);
            break;
        case TxtPartType::Uri:
            segments.append(p.tagName.isEmpty() ? p.content : (p.tagName + QLatin1Char(':') + p.content));
            break;
        case TxtPartType::Word:
        default:
            segments.append(p.content);
            break;
        }
    }
    m_description = segments.join(QLatin1Char(' '));
}

QString TxtTask::dueDateString() const {
    return m_dueDate.isValid() ? m_dueDate.toString(Qt::ISODate) : QString();
}

bool TxtTask::isOverdue(const QDate& refDate) const {
    return !m_done && m_dueDate.isValid() && m_dueDate < refDate;
}

bool TxtTask::isDueToday(const QDate& refDate) const {
    return !m_done && m_dueDate.isValid() && m_dueDate == refDate;
}

bool TxtTask::isDueThisWeek(const QDate& refDate) const {
    if (m_done || !m_dueDate.isValid()) return false;
    return m_dueDate >= refDate && m_dueDate <= refDate.addDays(7);
}

void TxtTask::setDueDate(const QDate& date) {
    m_dueDate = date;
    for (int i = static_cast<int>(m_parts.size()) - 1; i >= 0; --i) {
        if (m_parts[i].type == TxtPartType::Tag && m_parts[i].tagName == QStringLiteral("due")) {
            m_parts.removeAt(i);
        }
    }
    if (m_dueDate.isValid()) {
        m_parts.append({TxtPartType::Tag, m_dueDate.toString(Qt::ISODate), QStringLiteral("due")});
    }
    rebuildDescriptionFromParts();
}

} // namespace gofi
