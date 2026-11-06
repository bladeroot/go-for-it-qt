#pragma once

#include <QDate>
#include <QString>
#include <QVariantList>
#include <QVector>

namespace gofi {

enum class TxtPartType {
    Word,
    Tag,
    Project,
    Context,
    Uri
};

struct TxtPart {
    TxtPartType type = TxtPartType::Word;
    QString content;
    QString tagName;
};

class TxtTask {
public:
    static constexpr quint8 NoPriority = 127;

    TxtTask() = default;
    explicit TxtTask(const QString& simpleLine, bool done = false);

    static TxtTask fromSimpleTxt(const QString& line, bool done = false, bool addCreationDate = true);
    static TxtTask fromTodoTxt(const QString& line, bool doneByDefault = false);
    static TxtTask fromDatabase(qint64 id,
                                const QString& description,
                                bool done,
                                const QString& status,
                                quint8 priority,
                                const QDate& creationDate,
                                const QDate& completionDate,
                                quint32 duration,
                                quint32 timerValue,
                                const QString& jiraKey = {},
                                quint32 jiraSyncedSeconds = 0,
                                const QString& details = {},
                                const QVariantList& comments = {},
                                bool isViewed = true,
                                const QString& listId = {},
                                const QString& listName = {},
                                const QString& listProvider = {});

    [[nodiscard]] qint64 id() const noexcept { return m_id; }
    void setId(qint64 id) noexcept { m_id = id; }

    [[nodiscard]] const QString& listId() const noexcept { return m_listId; }
    void setListId(const QString& id) { m_listId = id; }

    [[nodiscard]] const QString& listName() const noexcept { return m_listName; }
    void setListName(const QString& name) { m_listName = name; }

    [[nodiscard]] const QString& listProvider() const noexcept { return m_listProvider; }
    void setListProvider(const QString& provider) { m_listProvider = provider; }

    [[nodiscard]] const QString& jiraKey() const noexcept { return m_jiraKey; }
    void setJiraKey(const QString& key) { m_jiraKey = key; }

    [[nodiscard]] quint32 jiraSyncedSeconds() const noexcept { return m_jiraSyncedSeconds; }
    void setJiraSyncedSeconds(quint32 sec) noexcept { m_jiraSyncedSeconds = sec; }

    [[nodiscard]] const QString& details() const noexcept { return m_details; }
    void setDetails(const QString& details) { m_details = details; }

    [[nodiscard]] const QVariantList& comments() const noexcept { return m_comments; }
    void setComments(const QVariantList& comments) { m_comments = comments; }

    [[nodiscard]] bool isValid() const noexcept { return !m_description.isEmpty(); }

    [[nodiscard]] const QString& description() const noexcept { return m_description; }
    void updateFromSimpleTxt(const QString& line);

    [[nodiscard]] bool isDone() const noexcept { return m_done; }
    void setDone(bool done);

    [[nodiscard]] bool isViewed() const noexcept { return m_isViewed; }
    void setViewed(bool viewed) noexcept { m_isViewed = viewed; }

    [[nodiscard]] QString status() const { return m_status; }
    void setStatus(const QString& status);
    static bool isClosedStatus(const QString& status);

    [[nodiscard]] quint8 priority() const noexcept { return m_priority; }
    void setPriority(quint8 prio) noexcept { m_priority = prio; }
    [[nodiscard]] QString priorityString() const;

    [[nodiscard]] QDate creationDate() const noexcept { return m_creationDate; }
    void setCreationDate(const QDate& date) noexcept { m_creationDate = date; }

    [[nodiscard]] QDate completionDate() const noexcept { return m_completionDate; }
    void setCompletionDate(const QDate& date) noexcept { m_completionDate = date; }

    [[nodiscard]] quint32 timerValue() const noexcept { return m_timerValue; }
    void setTimerValue(quint32 seconds) noexcept { m_timerValue = seconds; }

    [[nodiscard]] quint32 duration() const noexcept { return m_duration; }
    void setDuration(quint32 seconds) noexcept { m_duration = seconds; }

    [[nodiscard]] const QVector<TxtPart>& parts() const noexcept { return m_parts; }
    [[nodiscard]] QVariantList tokensVariantList() const;
    [[nodiscard]] QString richText() const;

    [[nodiscard]] QString toSimpleTxt() const;
    [[nodiscard]] QString toTodoTxt(bool logTimer = true) const;

    [[nodiscard]] int compare(const TxtTask& other) const;

    static bool parseDurationValue(const QString& str, quint32& outSeconds);
    static QString formatDurationValue(quint32 seconds);
    static QString formatTimerValue(quint32 seconds);

private:
    static bool isDateToken(const QString& token);
    static bool isPriorityToken(const QString& token);
    static bool isProjectToken(const QString& token);
    static bool isContextToken(const QString& token);
    static bool isCommonUriScheme(const QString& scheme);

    static TxtPart tokenizePart(const QString& token);
    void parseDescriptionTokens(const QStringList& tokens, int startIndex);
    void rebuildDescriptionFromParts();

    qint64 m_id = -1;
    QString m_description;
    QString m_jiraKey;
    QString m_status = QStringLiteral("todo");
    QString m_details;
    QVariantList m_comments;
    QVector<TxtPart> m_parts;
    QDate m_creationDate;
    QDate m_completionDate;
    quint32 m_timerValue = 0;
    quint32 m_duration = 0;
    quint32 m_jiraSyncedSeconds = 0;
    quint8 m_priority = NoPriority;
    bool m_done = false;
    bool m_isViewed = true;
    QString m_listId;
    QString m_listName;
    QString m_listProvider;
};

} // namespace gofi
