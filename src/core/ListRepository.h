#pragma once

#include "Schedule.h"
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>
#include <QVector>

namespace gofi {

class DatabaseManager;

struct TodoListEntry {
    QString id;
    QString name;
    QString provider = QStringLiteral("SQLite");
    QString directoryPath;
    QString todoFilePath;
    QString doneFilePath;
    QString activityLogPath;
    bool logTimerInTxt = true;
    bool addCreationDates = true;
    int reminderTime = -1;
    Schedule customSchedule;
    QString jiraUrl;
    QString jiraUser;
    QString jiraJql;
    QString apiToken;
    bool autoWorklog = true;
    bool syncStatusToRemote = false;
    QVariantMap statusMapping; // remoteStatusName -> "todo" | "in_progress" | "paused" | "done" | "canceled"
    QVariantMap priorityMapping; // remotePriorityName -> "A" | "B" | "C" | "D" | "E" | "none"
};

class ListRepository {
public:
    explicit ListRepository(DatabaseManager* db = nullptr);

    // Lists CRUD
    [[nodiscard]] QVector<TodoListEntry> loadLists() const;
    bool saveList(const TodoListEntry& entry, int sortOrder);
    bool updateListStatusMapping(const QString& listId, const QVariantMap& mapping);
    bool removeList(const QString& listId, bool* notFound = nullptr);
    bool updateListsOrder(const QVector<TodoListEntry>& entries);

private:
    [[nodiscard]] QSqlDatabase db() const;
    [[nodiscard]] QString databasePath() const;

    DatabaseManager* m_dbManager = nullptr;
};

} // namespace gofi
