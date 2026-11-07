#include "ListManagerModel.h"
#include <QUuid>

namespace gofi {

ListManagerModel::ListManagerModel(QObject* parent)
    : QAbstractListModel(parent)
{
    reloadFromDatabase();
}

void ListManagerModel::reloadFromDatabase() {
    DatabaseManager& db = DatabaseManager::instance();
    if (!db.isOpen()) {
        db.initialize();
    }
    beginResetModel();
    m_entries = db.loadLists();
    endResetModel();

    if (m_entries.isEmpty()) {
        addList(tr("To-Do"));
    } else {
        emit countChanged();
    }
}

int ListManagerModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) return 0;
    return static_cast<int>(m_entries.size());
}

QVariant ListManagerModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) {
        return {};
    }
    const TodoListEntry& e = m_entries.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return e.name;
    case IdRole:
        return e.id;
    case ProviderRole:
        return e.provider;
    case DirectoryPathRole:
        return e.jiraUrl.isEmpty() ? DatabaseManager::instance().databasePath() : e.jiraUrl;
    case ActivityLogPathRole:
        return e.activityLogPath;
    case AddCreationDatesRole:
        return e.addCreationDates;
    case ReminderTimeRole:
        return e.reminderTime;
    case CustomScheduleRole:
        return e.customSchedule.toVariantList();
    default:
        return {};
    }
}

QHash<int, QByteArray> ListManagerModel::roleNames() const {
    return {
        {IdRole, "listId"},
        {NameRole, "name"},
        {ProviderRole, "provider"},
        {DirectoryPathRole, "directoryPath"},
        {ActivityLogPathRole, "activityLogPath"},
        {AddCreationDatesRole, "addCreationDates"},
        {ReminderTimeRole, "reminderTime"},
        {CustomScheduleRole, "customSchedule"}
    };
}

QString ListManagerModel::addList(const QString& name, const QString& directoryPath, bool separateDoneFile) {
    Q_UNUSED(directoryPath)
    Q_UNUSED(separateDoneFile)

    const QString trimmedName = name.trimmed().isEmpty() ? tr("To-Do") : name.trimmed();

    TodoListEntry entry;
    entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    entry.name = trimmedName;
    entry.provider = QStringLiteral("SQLite");
    entry.directoryPath = DatabaseManager::instance().databasePath();

    const int pos = static_cast<int>(m_entries.size());
    DatabaseManager::instance().saveList(entry, pos);

    beginInsertRows(QModelIndex(), pos, pos);
    m_entries.append(entry);
    endInsertRows();

    emit countChanged();
    return entry.id;
}

QVariantMap ListManagerModel::getListConfig(const QString& listId) const {
    const int idx = indexOfId(listId);
    if (idx < 0) {
        return {
            {QStringLiteral("id"), QString()},
            {QStringLiteral("name"), QString()},
            {QStringLiteral("provider"), QStringLiteral("SQLite")},
            {QStringLiteral("activityLogPath"), QString()},
            {QStringLiteral("addCreationDates"), true},
            {QStringLiteral("reminderTime"), -1},
            {QStringLiteral("customSchedule"), QVariantList()},
            {QStringLiteral("jiraUrl"), QString()},
            {QStringLiteral("jiraUser"), QString()},
            {QStringLiteral("jiraJql"), QString()},
            {QStringLiteral("apiToken"), QString()},
            {QStringLiteral("autoWorklog"), true},
            {QStringLiteral("statusMapping"), QVariantMap()},
            {QStringLiteral("priorityMapping"), QVariantMap()}
        };
    }
    const TodoListEntry& e = m_entries.at(idx);
    return {
        {QStringLiteral("id"), e.id},
        {QStringLiteral("name"), e.name},
        {QStringLiteral("provider"), e.provider},
        {QStringLiteral("activityLogPath"), e.activityLogPath},
        {QStringLiteral("addCreationDates"), e.addCreationDates},
        {QStringLiteral("reminderTime"), e.reminderTime},
        {QStringLiteral("customSchedule"), e.customSchedule.toVariantList()},
        {QStringLiteral("jiraUrl"), e.jiraUrl},
        {QStringLiteral("jiraUser"), e.jiraUser},
        {QStringLiteral("jiraJql"), e.jiraJql},
        {QStringLiteral("apiToken"), e.apiToken},
        {QStringLiteral("autoWorklog"), e.autoWorklog},
        {QStringLiteral("statusMapping"), e.statusMapping},
        {QStringLiteral("priorityMapping"), e.priorityMapping}
    };
}

QVariantMap ListManagerModel::validateAndCheckConflicts(const QVariantMap& config) const {
    const QString name = config.value(QStringLiteral("name")).toString().trimmed();
    const QString provider = config.value(QStringLiteral("provider"), QStringLiteral("SQLite")).toString();
    const QString jiraUrl = config.value(QStringLiteral("jiraUrl")).toString().trimmed();
    const QString apiToken = config.value(QStringLiteral("apiToken")).toString().trimmed();

    if (name.isEmpty()) {
        return {
            {QStringLiteral("valid"), false},
            {QStringLiteral("error"), tr("Please assign a name to the list.")}
        };
    }
    if ((provider == QStringLiteral("Jira") || provider == QStringLiteral("YouTrack")) && jiraUrl.isEmpty()) {
        return {
            {QStringLiteral("valid"), false},
            {QStringLiteral("error"), tr("Please enter the Server URL for %1.").arg(provider)}
        };
    }
    if ((provider == QStringLiteral("Jira") || provider == QStringLiteral("YouTrack")) && apiToken.isEmpty()) {
        return {
            {QStringLiteral("valid"), false},
            {QStringLiteral("error"), tr("Please enter an API Token / Permanent Token for %1.").arg(provider)}
        };
    }
    return {
        {QStringLiteral("valid"), true},
        {QStringLiteral("hasConflict"), false},
        {QStringLiteral("conflictMessage"), QString()}
    };
}

QString ListManagerModel::saveListConfig(const QVariantMap& config, const QString& conflictResolution) {
    Q_UNUSED(conflictResolution)

    const QString existingId = config.value(QStringLiteral("id")).toString();
    const QString name = config.value(QStringLiteral("name")).toString().trimmed();

    TodoListEntry entry;
    entry.id = existingId.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) : existingId;
    entry.name = name.isEmpty() ? tr("To-Do") : name;
    entry.provider = config.value(QStringLiteral("provider"), QStringLiteral("SQLite")).toString();
    entry.directoryPath = DatabaseManager::instance().databasePath();
    entry.activityLogPath = config.value(QStringLiteral("activityLogPath")).toString().trimmed();
    entry.addCreationDates = config.value(QStringLiteral("addCreationDates"), true).toBool();
    entry.reminderTime = config.value(QStringLiteral("reminderTime"), -1).toInt();
    entry.customSchedule.fromVariantList(config.value(QStringLiteral("customSchedule")).toList());
    entry.jiraUrl = config.value(QStringLiteral("jiraUrl")).toString().trimmed();
    if (entry.provider == QStringLiteral("GitHub") && entry.jiraUrl.isEmpty()) {
        entry.jiraUrl = QStringLiteral("https://api.github.com");
    } else if (entry.provider == QStringLiteral("GitLab") && entry.jiraUrl.isEmpty()) {
        entry.jiraUrl = QStringLiteral("https://gitlab.com");
    } else if (entry.provider == QStringLiteral("Todoist") && entry.jiraUrl.isEmpty()) {
        entry.jiraUrl = QStringLiteral("https://api.todoist.com");
    }
    entry.jiraUser = config.value(QStringLiteral("jiraUser")).toString().trimmed();
    entry.jiraJql = config.value(QStringLiteral("jiraJql")).toString().trimmed();
    entry.apiToken = config.value(QStringLiteral("apiToken")).toString().trimmed();
    entry.autoWorklog = config.value(QStringLiteral("autoWorklog"), true).toBool();
    entry.statusMapping = config.value(QStringLiteral("statusMapping")).toMap();
    entry.priorityMapping = config.value(QStringLiteral("priorityMapping")).toMap();

    const int idx = indexOfId(existingId);
    if (idx >= 0) {
        m_entries[idx] = entry;
        DatabaseManager::instance().saveList(entry, idx);
        const QModelIndex modelIdx = index(idx, 0);
        emit dataChanged(modelIdx, modelIdx);
        emit listUpdated(entry.id);
        return entry.id;
    }

    const int pos = static_cast<int>(m_entries.size());
    DatabaseManager::instance().saveList(entry, pos);
    beginInsertRows(QModelIndex(), pos, pos);
    m_entries.append(entry);
    endInsertRows();
    emit countChanged();
    return entry.id;
}

void ListManagerModel::removeListById(const QString& id) {
    const int idx = indexOfId(id);
    if (idx < 0) return;

    DatabaseManager::instance().removeList(id);

    beginRemoveRows(QModelIndex(), idx, idx);
    m_entries.removeAt(idx);
    endRemoveRows();

    DatabaseManager::instance().updateListsOrder(m_entries);
    emit countChanged();
    emit listRemoved(id);
}

void ListManagerModel::moveItem(int fromRow, int toRow) {
    if (fromRow < 0 || fromRow >= m_entries.size() || toRow < 0 || toRow >= m_entries.size() || fromRow == toRow) {
        return;
    }
    const int dest = (toRow > fromRow) ? (toRow + 1) : toRow;
    if (!beginMoveRows(QModelIndex(), fromRow, fromRow, QModelIndex(), dest)) {
        return;
    }
    m_entries.move(fromRow, toRow);
    endMoveRows();
    DatabaseManager::instance().updateListsOrder(m_entries);
}

int ListManagerModel::indexOfId(const QString& id) const {
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).id == id) {
            return i;
        }
    }
    return -1;
}

TodoListEntry ListManagerModel::entry(int row) const {
    if (row < 0 || row >= m_entries.size()) return {};
    return m_entries.at(row);
}

QVariantMap ListManagerModel::entryAt(int row) const {
    if (row < 0 || row >= m_entries.size()) return {};
    const TodoListEntry& e = m_entries.at(row);
    return {
        {QStringLiteral("id"), e.id},
        {QStringLiteral("name"), e.name},
        {QStringLiteral("provider"), e.provider},
        {QStringLiteral("directoryPath"), e.directoryPath},
        {QStringLiteral("todoFilePath"), e.todoFilePath},
        {QStringLiteral("doneFilePath"), e.doneFilePath},
        {QStringLiteral("activityLogPath"), e.activityLogPath},
        {QStringLiteral("logTimerInTxt"), e.logTimerInTxt},
        {QStringLiteral("addCreationDates"), e.addCreationDates},
        {QStringLiteral("reminderTime"), e.reminderTime},
        {QStringLiteral("customSchedule"), e.customSchedule.toVariantList()},
        {QStringLiteral("jiraUrl"), e.jiraUrl},
        {QStringLiteral("jiraUser"), e.jiraUser},
        {QStringLiteral("jiraJql"), e.jiraJql},
        {QStringLiteral("apiToken"), e.apiToken},
        {QStringLiteral("autoWorklog"), e.autoWorklog},
        {QStringLiteral("statusMapping"), e.statusMapping},
        {QStringLiteral("priorityMapping"), e.priorityMapping}
    };
}

QVariantMap ListManagerModel::get(int row) const {
    return entryAt(row);
}

} // namespace gofi
