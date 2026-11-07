#pragma once

#include "TxtTask.h"
#include <QSortFilterProxyModel>
#include <optional>

namespace gofi {

class TaskFilterProxyModel : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString filterQuery READ filterQuery WRITE setFilterQuery NOTIFY filterQueryChanged)
    Q_PROPERTY(QString statusFilter READ statusFilter WRITE setStatusFilter NOTIFY statusFilterChanged)
    Q_PROPERTY(QString dueFilter READ dueFilter WRITE setDueFilter NOTIFY dueFilterChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum class PrioConstraintType {
        Less,
        Greater,
        LessOrEqual,
        GreaterOrEqual,
        Equal,
        Between
    };

    struct PrioConstraint {
        PrioConstraintType type = PrioConstraintType::Equal;
        quint8 p1 = 0;
        quint8 p2 = 0;
    };

    explicit TaskFilterProxyModel(QObject* parent = nullptr);

    [[nodiscard]] QString filterQuery() const { return m_filterQuery; }
    void setFilterQuery(const QString& query);

    [[nodiscard]] QString statusFilter() const { return m_statusFilter; }
    void setStatusFilter(const QString& status);

    [[nodiscard]] QString dueFilter() const { return m_dueFilter; }
    void setDueFilter(const QString& filter);

    Q_INVOKABLE int mapToSourceRow(int proxyRow) const;

signals:
    void filterQueryChanged();
    void statusFilterChanged();
    void dueFilterChanged();
    void countChanged();

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    void parsePriorityConstraint(const QString& expr);
    [[nodiscard]] bool checkPriority(const TxtTask& task) const;

    QString m_filterQuery;
    QString m_statusFilter;
    QString m_dueFilter;
    QVector<TxtPart> m_searchParts;
    std::optional<PrioConstraint> m_prioConstraint;
};

} // namespace gofi
