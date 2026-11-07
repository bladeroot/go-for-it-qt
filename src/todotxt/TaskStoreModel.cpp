#include "TaskStoreModel.h"
#include <QDateTime>
#include <QVariantMap>
#include <algorithm>

namespace gofi {

TaskStoreModel::TaskStoreModel(bool doneByDefault, QObject* parent)
    : QAbstractListModel(parent)
    , m_doneByDefault(doneByDefault)
{
}

int TaskStoreModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_tasks.size());
}

QVariant TaskStoreModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tasks.size()) {
        return {};
    }
    const TxtTask& t = m_tasks.at(index.row());
    switch (role) {
    case IdRole:
        return t.id();
    case Qt::DisplayRole:
    case DescriptionRole:
        return t.description();
    case SimpleTextRole:
        return t.toSimpleTxt();
    case DoneRole:
        return t.isDone();
    case StatusRole:
        return t.status();
    case PriorityRole:
        return t.priority();
    case PriorityStringRole:
        return t.priorityString();
    case TimerValueRole:
        return t.timerValue();
    case DurationRole:
        return t.duration();
    case TokensRole:
        return t.tokensVariantList();
    case RichTextRole:
        return t.richText();
    case CreationDateRole:
        return t.creationDate().toString(Qt::ISODate);
    case CompletionDateRole:
        return t.completionDate().toString(Qt::ISODate);
    case JiraKeyRole:
        return t.jiraKey();
    case DetailsRole:
        return t.details();
    case CommentsRole:
        return t.comments();
    case IsViewedRole:
        return t.isViewed();
    case ListIdRole:
        return t.listId();
    case ListNameRole:
        return t.listName();
    case ListProviderRole:
        return t.listProvider();
    case DueDateRole:
    case DueDateStringRole:
        return t.dueDateString();
    case IsOverdueRole:
        return t.isOverdue();
    case IsDueTodayRole:
        return t.isDueToday();
    case IsDueThisWeekRole:
        return t.isDueThisWeek();
    default:
        return {};
    }
}

bool TaskStoreModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_tasks.size()) {
        return false;
    }
    TxtTask& t = m_tasks[index.row()];
    switch (role) {
    case DoneRole: {
        const bool newDone = value.toBool();
        if (t.isDone() == newDone) return false;
        t.setDone(newDone);
        emit taskDoneToggled(index.row(), t);
        return true;
    }
    case StatusRole: {
        const QString newStatus = value.toString().trimmed().toLower();
        if (newStatus.isEmpty() || t.status() == newStatus) return false;
        const bool oldDone = t.isDone();
        t.setStatus(newStatus);
        if (oldDone != t.isDone()) {
            emit taskDoneToggled(index.row(), t);
        } else {
            emit dataChanged(index, index, {DoneRole, StatusRole, CompletionDateRole});
            emit taskUpdated(index.row(), t);
            emit taskDataChanged();
        }
        emit unviewedTodoCountChanged();
        emit totalTodoCountChanged();
        return true;
    }
    case IsViewedRole: {
        const bool v = value.toBool();
        if (t.isViewed() == v) return false;
        t.setViewed(v);
        emit dataChanged(index, index, {IsViewedRole});
        emit taskUpdated(index.row(), t);
        emit taskDataChanged();
        emit unviewedTodoCountChanged();
        return true;
    }
    case SimpleTextRole:
    case DescriptionRole: {
        t.updateFromSimpleTxt(value.toString());
        if (!t.isValid()) {
            emit taskBecameInvalid(index.row());
            return true;
        }
        emit dataChanged(index, index, {DescriptionRole, SimpleTextRole, PriorityRole, PriorityStringRole, DurationRole, TokensRole, RichTextRole, DueDateRole, DueDateStringRole, IsOverdueRole, IsDueTodayRole, IsDueThisWeekRole});
        emit taskUpdated(index.row(), t);
        emit taskDataChanged();
        return true;
    }
    case TimerValueRole: {
        const quint32 sec = value.toUInt();
        if (t.timerValue() == sec) return false;
        t.setTimerValue(sec);
        emit dataChanged(index, index, {TimerValueRole});
        emit taskUpdated(index.row(), t);
        emit taskDataChanged();
        return true;
    }
    case DetailsRole: {
        const QString d = value.toString();
        if (t.details() == d) return false;
        t.setDetails(d);
        emit dataChanged(index, index, {DetailsRole});
        emit taskUpdated(index.row(), t);
        emit taskDataChanged();
        return true;
    }
    case PriorityRole: {
        const quint8 prio = static_cast<quint8>(value.toUInt());
        if (t.priority() == prio) return false;
        t.setPriority(prio);
        emit dataChanged(index, index, {PriorityRole, PriorityStringRole, SimpleTextRole});
        emit taskUpdated(index.row(), t);
        emit taskDataChanged();
        return true;
    }
    default:
        return false;
    }
}

QHash<int, QByteArray> TaskStoreModel::roleNames() const {
    return {
        {IdRole, "taskId"},
        {DescriptionRole, "description"},
        {SimpleTextRole, "simpleText"},
        {DoneRole, "done"},
        {StatusRole, "taskStatus"},
        {PriorityRole, "priority"},
        {PriorityStringRole, "priorityString"},
        {TimerValueRole, "timerValue"},
        {DurationRole, "duration"},
        {TokensRole, "tokens"},
        {RichTextRole, "richText"},
        {CreationDateRole, "creationDate"},
        {CompletionDateRole, "completionDate"},
        {JiraKeyRole, "jiraKey"},
        {DetailsRole, "details"},
        {CommentsRole, "comments"},
        {IsViewedRole, "isViewed"},
        {ListIdRole, "listId"},
        {ListNameRole, "listName"},
        {ListProviderRole, "listProvider"},
        {DueDateRole, "dueDate"},
        {DueDateStringRole, "dueDateString"},
        {IsOverdueRole, "isOverdue"},
        {IsDueTodayRole, "isDueToday"},
        {IsDueThisWeekRole, "isDueThisWeek"}
    };
}

void TaskStoreModel::appendTask(const TxtTask& task) {
    const int pos = static_cast<int>(m_tasks.size());
    beginInsertRows(QModelIndex(), pos, pos);
    m_tasks.append(task);
    endInsertRows();
    emit countChanged();
    emit taskDataChanged();
    emit unviewedTodoCountChanged();
    emit totalTodoCountChanged();
}

void TaskStoreModel::prependTask(const TxtTask& task) {
    beginInsertRows(QModelIndex(), 0, 0);
    m_tasks.prepend(task);
    endInsertRows();
    emit countChanged();
    emit taskDataChanged();
    emit unviewedTodoCountChanged();
    emit totalTodoCountChanged();
}

void TaskStoreModel::setTasks(QVector<TxtTask> tasks) {
    beginResetModel();
    m_tasks = std::move(tasks);
    endResetModel();
    emit countChanged();
    emit unviewedTodoCountChanged();
    emit totalTodoCountChanged();
}

void TaskStoreModel::clear() {
    if (m_tasks.isEmpty()) return;
    beginResetModel();
    m_tasks.clear();
    endResetModel();
    emit countChanged();
    emit taskDataChanged();
    emit unviewedTodoCountChanged();
    emit totalTodoCountChanged();
}

void TaskStoreModel::sortTasks() {
    if (m_tasks.size() <= 1) return;
    beginResetModel();
    std::stable_sort(m_tasks.begin(), m_tasks.end(), [](const TxtTask& a, const TxtTask& b) {
        return a.compare(b) < 0;
    });
    endResetModel();
    emit tasksReordered();
    emit taskDataChanged();
}

void TaskStoreModel::removeAt(int row) {
    if (row < 0 || row >= m_tasks.size()) return;
    const qint64 removedId = m_tasks.at(row).id();
    beginRemoveRows(QModelIndex(), row, row);
    m_tasks.removeAt(row);
    endRemoveRows();
    emit countChanged();
    if (removedId >= 0) {
        emit taskRemoved(removedId);
    }
    emit taskDataChanged();
    emit unviewedTodoCountChanged();
    emit totalTodoCountChanged();
}

void TaskStoreModel::moveItem(int fromRow, int toRow) {
    if (fromRow < 0 || fromRow >= m_tasks.size() || toRow < 0 || toRow >= m_tasks.size() || fromRow == toRow) {
        return;
    }
    const int dest = (toRow > fromRow) ? (toRow + 1) : toRow;
    if (!beginMoveRows(QModelIndex(), fromRow, fromRow, QModelIndex(), dest)) {
        return;
    }
    m_tasks.move(fromRow, toRow);
    endMoveRows();
    emit tasksReordered();
    emit taskDataChanged();
}

void TaskStoreModel::updateTaskText(int row, const QString& simpleText) {
    setData(index(row, 0), simpleText, SimpleTextRole);
}

void TaskStoreModel::setTaskStatus(int row, const QString& newStatus) {
    setData(index(row, 0), newStatus, StatusRole);
}

void TaskStoreModel::updateTaskDetails(int row, const QString& details) {
    setData(index(row, 0), details, DetailsRole);
}

void TaskStoreModel::appendTaskComment(int row, const QString& author, const QString& commentBody) {
    if (row < 0 || row >= m_tasks.size() || commentBody.trimmed().isEmpty()) {
        return;
    }
    TxtTask& t = m_tasks[row];
    QVariantList comments = t.comments();
    comments.append(QVariantMap{
        {QStringLiteral("author"), author.trimmed().isEmpty() ? QStringLiteral("Me") : author.trimmed()},
        {QStringLiteral("created"), QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))},
        {QStringLiteral("body"), commentBody.trimmed()}
    });
    t.setComments(comments);
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {CommentsRole});
    emit taskUpdated(row, t);
    emit taskDataChanged();
}

TxtTask TaskStoreModel::taskAt(int row) const {
    if (row < 0 || row >= m_tasks.size()) {
        return {};
    }
    return m_tasks.at(row);
}

void TaskStoreModel::setTaskTimerValue(int row, quint32 seconds) {
    setData(index(row, 0), seconds, TimerValueRole);
}

int TaskStoreModel::unviewedTodoCount() const {
    int count = 0;
    for (const auto& t : m_tasks) {
        if (!t.isDone() && (t.status() == QStringLiteral("todo") || t.status().isEmpty()) && !t.isViewed()) {
            ++count;
        }
    }
    return count;
}

int TaskStoreModel::totalTodoCount() const {
    int count = 0;
    for (const auto& t : m_tasks) {
        if (!t.isDone() && (t.status() == QStringLiteral("todo") || t.status().isEmpty())) {
            ++count;
        }
    }
    return count;
}

void TaskStoreModel::markTaskViewed(int row) {
    if (row < 0 || row >= m_tasks.size()) return;
    if (m_tasks.at(row).isViewed()) return;

    m_tasks[row].setViewed(true);
    const QModelIndex idx = index(row, 0);
    emit dataChanged(idx, idx, {IsViewedRole});
    emit taskUpdated(row, m_tasks[row]);
    emit taskDataChanged();
    emit unviewedTodoCountChanged();
}

} // namespace gofi
