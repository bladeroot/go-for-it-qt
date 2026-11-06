#pragma once

#include "TxtTask.h"
#include <QAbstractListModel>
#include <QVector>

namespace gofi {

class TaskStoreModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(bool doneByDefault READ doneByDefault CONSTANT)
    Q_PROPERTY(int unviewedTodoCount READ unviewedTodoCount NOTIFY unviewedTodoCountChanged)
    Q_PROPERTY(int totalTodoCount READ totalTodoCount NOTIFY totalTodoCountChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        DescriptionRole,
        SimpleTextRole,
        DoneRole,
        StatusRole,
        PriorityRole,
        PriorityStringRole,
        TimerValueRole,
        DurationRole,
        TokensRole,
        RichTextRole,
        CreationDateRole,
        CompletionDateRole,
        JiraKeyRole,
        DetailsRole,
        CommentsRole,
        IsViewedRole
    };

    explicit TaskStoreModel(bool doneByDefault = false, QObject* parent = nullptr);

    [[nodiscard]] bool doneByDefault() const noexcept { return m_doneByDefault; }

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void appendTask(const TxtTask& task);
    void prependTask(const TxtTask& task);
    void setTasks(QVector<TxtTask> tasks);
    void clear();
    void sortTasks();

    Q_INVOKABLE void removeAt(int row);
    Q_INVOKABLE void moveItem(int fromRow, int toRow);
    Q_INVOKABLE void updateTaskText(int row, const QString& simpleText);
    Q_INVOKABLE void setTaskStatus(int row, const QString& newStatus);
    Q_INVOKABLE void updateTaskDetails(int row, const QString& details);
    Q_INVOKABLE void appendTaskComment(int row, const QString& author, const QString& commentBody);

    [[nodiscard]] const QVector<TxtTask>& tasks() const noexcept { return m_tasks; }
    [[nodiscard]] TxtTask taskAt(int row) const;
    void setTaskTimerValue(int row, quint32 seconds);
    [[nodiscard]] int unviewedTodoCount() const;
    [[nodiscard]] int totalTodoCount() const;
    Q_INVOKABLE void markTaskViewed(int row);

signals:
    void countChanged();
    void unviewedTodoCountChanged();
    void totalTodoCountChanged();
    void taskDataChanged();
    void taskUpdated(int row, gofi::TxtTask task);
    void taskRemoved(qint64 taskId);
    void tasksReordered();
    void taskDoneToggled(int row, gofi::TxtTask task);
    void taskBecameInvalid(int row);

private:
    QVector<TxtTask> m_tasks;
    bool m_doneByDefault = false;
};

} // namespace gofi
