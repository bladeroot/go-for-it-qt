#pragma once

#include "DatabaseManager.h"
#include "Schedule.h"
#include <QAbstractListModel>
#include <QVariantMap>
#include <QVector>

namespace gofi {

class ListManagerModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        ProviderRole,
        DirectoryPathRole,
        ActivityLogPathRole,
        AddCreationDatesRole,
        ReminderTimeRole,
        CustomScheduleRole
    };

    explicit ListManagerModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE QString addList(const QString& name,
                                const QString& directoryPath = {},
                                bool separateDoneFile = true);
    Q_INVOKABLE QString saveListConfig(const QVariantMap& config,
                                       const QString& conflictResolution = QStringLiteral("keep"));
    Q_INVOKABLE QVariantMap getListConfig(const QString& listId) const;
    Q_INVOKABLE QVariantMap validateAndCheckConflicts(const QVariantMap& config) const;
    Q_INVOKABLE [[nodiscard]] QVariantMap entryAt(int row) const;
    Q_INVOKABLE [[nodiscard]] QVariantMap get(int row) const;
    Q_INVOKABLE void removeListById(const QString& id);
    Q_INVOKABLE void moveItem(int fromRow, int toRow);
    void reloadFromDatabase();

    [[nodiscard]] int indexOfId(const QString& id) const;
    [[nodiscard]] TodoListEntry entry(int row) const;
    [[nodiscard]] const QVector<TodoListEntry>& entries() const noexcept { return m_entries; }

signals:
    void countChanged();
    void listRemoved(const QString& id);
    void listUpdated(const QString& id);

private:
    QVector<TodoListEntry> m_entries;
};

} // namespace gofi
