#include "TaskFilterProxyModel.h"
#include "TaskStoreModel.h"

namespace gofi {

TaskFilterProxyModel::TaskFilterProxyModel(QObject* parent)
    : QSortFilterProxyModel(parent)
{
    connect(this, &QAbstractItemModel::rowsInserted, this, &TaskFilterProxyModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &TaskFilterProxyModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &TaskFilterProxyModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &TaskFilterProxyModel::countChanged);
}

void TaskFilterProxyModel::setStatusFilter(const QString& status) {
    const QString norm = status.trimmed().toLower();
    if (m_statusFilter == norm) return;
    #if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    beginFilterChange();
    #endif
    m_statusFilter = norm;
    #if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    #else
    invalidateFilter();
    #endif
    emit statusFilterChanged();
    emit countChanged();
}

void TaskFilterProxyModel::setDueFilter(const QString& filter) {
    const QString norm = filter.trimmed().toLower();
    if (m_dueFilter == norm) return;
    #if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    beginFilterChange();
    #endif
    m_dueFilter = norm;
    #if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    #else
    invalidateFilter();
    #endif
    emit dueFilterChanged();
    emit countChanged();
}

void TaskFilterProxyModel::setReferenceDate(const QDate& date) {
    if (m_referenceDate == date) return;
    #if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    beginFilterChange();
    #endif
    m_referenceDate = date;
    #if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
    #else
    invalidateFilter();
    #endif
    emit countChanged();
}

void TaskFilterProxyModel::setFilterQuery(const QString& query) {
    if (m_filterQuery == query) {
        return;
    }
    m_filterQuery = query;

    const QStringList tokens = query.toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    beginFilterChange();
#endif
    m_searchParts.clear();
    m_prioConstraint.reset();

    for (const QString& token : tokens) {
        if (token.startsWith(QStringLiteral("project:")) && token.size() > 8) {
            m_searchParts.append({TxtPartType::Project, token.mid(8), {}});
        } else if (token.startsWith(QStringLiteral("context:")) && token.size() > 8) {
            m_searchParts.append({TxtPartType::Context, token.mid(8), {}});
        } else if (token.startsWith(QStringLiteral("priority:")) && token.size() > 9) {
            parsePriorityConstraint(token.mid(9));
        } else if (token.startsWith(QLatin1Char('+')) && token.size() > 1) {
            m_searchParts.append({TxtPartType::Project, token.mid(1), {}});
        } else if (token.startsWith(QLatin1Char('@')) && token.size() > 1) {
            m_searchParts.append({TxtPartType::Context, token.mid(1), {}});
        } else {
            const int colonIdx = token.indexOf(QLatin1Char(':'));
            if (colonIdx > 0 && colonIdx + 1 < token.size()) {
                m_searchParts.append({TxtPartType::Tag, token.mid(colonIdx + 1), token.left(colonIdx)});
            } else {
                m_searchParts.append({TxtPartType::Word, token, {}});
            }
        }
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    invalidateFilter();
#endif
    emit filterQueryChanged();
    emit countChanged();
}

void TaskFilterProxyModel::parsePriorityConstraint(const QString& expr) {
    if (expr.isEmpty()) return;
    int offset = 0;
    PrioConstraintType pcType = PrioConstraintType::Equal;
    if (expr.startsWith(QStringLiteral(">="))) {
        pcType = PrioConstraintType::GreaterOrEqual;
        offset = 2;
    } else if (expr.startsWith(QLatin1Char('>'))) {
        pcType = PrioConstraintType::Greater;
        offset = 1;
    } else if (expr.startsWith(QStringLiteral("<="))) {
        pcType = PrioConstraintType::LessOrEqual;
        offset = 2;
    } else if (expr.startsWith(QLatin1Char('<'))) {
        pcType = PrioConstraintType::Less;
        offset = 1;
    }

    const QString rem = expr.mid(offset);
    if (rem.isEmpty() || rem.at(0) < QLatin1Char('a') || rem.at(0) > QLatin1Char('z')) {
        return;
    }
    const quint8 prio1 = static_cast<quint8>(rem.at(0).unicode() - 'a');
    if (rem.size() == 1) {
        m_prioConstraint = PrioConstraint{pcType, prio1, prio1};
    } else if (pcType == PrioConstraintType::Equal && rem.size() == 3 && rem.at(1) == QLatin1Char('-')) {
        if (rem.at(2) >= QLatin1Char('a') && rem.at(2) <= QLatin1Char('z')) {
            const quint8 prio2 = static_cast<quint8>(rem.at(2).unicode() - 'a');
            m_prioConstraint = PrioConstraint{
                PrioConstraintType::Between,
                qMax(prio1, prio2),
                qMin(prio1, prio2)
            };
        }
    }
}

bool TaskFilterProxyModel::checkPriority(const TxtTask& task) const {
    if (!m_prioConstraint.has_value()) {
        return true;
    }
    if (task.priority() >= TxtTask::NoPriority) {
        return false;
    }
    const PrioConstraint& c = *m_prioConstraint;
    switch (c.type) {
    case PrioConstraintType::Less:
        return task.priority() > c.p1;
    case PrioConstraintType::Greater:
        return task.priority() < c.p1;
    case PrioConstraintType::LessOrEqual:
        return task.priority() >= c.p1;
    case PrioConstraintType::GreaterOrEqual:
        return task.priority() <= c.p1;
    case PrioConstraintType::Between:
        return task.priority() <= c.p1 && task.priority() >= c.p2;
    case PrioConstraintType::Equal:
    default:
        return task.priority() == c.p1;
    }
}

int TaskFilterProxyModel::mapToSourceRow(int proxyRow) const {
    const QModelIndex proxyIdx = index(proxyRow, 0);
    if (!proxyIdx.isValid()) return -1;
    return mapToSource(proxyIdx).row();
}

bool TaskFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    Q_UNUSED(sourceParent)
    const auto* store = qobject_cast<const TaskStoreModel*>(sourceModel());
    if (!store) {
        return true;
    }

    const TxtTask task = store->taskAt(sourceRow);
    if (!m_statusFilter.isEmpty() && task.status() != m_statusFilter) {
        return false;
    }
    const QDate ref = referenceDate();
    if (!m_dueFilter.isEmpty() && m_dueFilter != QStringLiteral("all")) {
        if (m_dueFilter == QStringLiteral("today") && !task.isDueToday(ref)) return false;
        if (m_dueFilter == QStringLiteral("overdue") && !task.isOverdue(ref)) return false;
        if (m_dueFilter == QStringLiteral("week") && !task.isDueThisWeek(ref)) return false;
    }
    if (!checkPriority(task)) {
        return false;
    }
    if (m_searchParts.isEmpty()) {
        return true;
    }

    const QString descLower = task.description().toLower();
    const QVector<TxtPart>& parts = task.parts();

    for (const TxtPart& needle : m_searchParts) {
        if (needle.type == TxtPartType::Word) {
            if (!descLower.contains(needle.content)) {
                return false;
            }
        } else {
            bool matched = false;
            if (needle.type == TxtPartType::Tag && needle.tagName.toLower() == QStringLiteral("due")) {
                if (needle.content == QStringLiteral("today") && task.isDueToday(ref)) matched = true;
                else if (needle.content == QStringLiteral("overdue") && task.isOverdue(ref)) matched = true;
                else if (needle.content == QStringLiteral("week") && task.isDueThisWeek(ref)) matched = true;
            }
            if (!matched) {
                for (const TxtPart& part : parts) {
                    if (part.type != needle.type) continue;
                    if (part.type == TxtPartType::Tag && part.tagName.toLower() != needle.tagName) continue;
                    if (part.content.toLower().contains(needle.content)) {
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) {
                return false;
            }
        }
    }
    return true;
}

} // namespace gofi
