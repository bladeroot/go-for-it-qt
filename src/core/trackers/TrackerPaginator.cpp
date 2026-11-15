#include "TrackerPaginator.h"

namespace gofi {

TrackerPaginator::TrackerPaginator(int maxPages)
    : m_maxPages(maxPages)
{
}

void TrackerPaginator::recordPage(int itemsCount, int serverTotalCount, bool incompleteResults) {
    if (incompleteResults) {
        m_limitReached = true;
    }

    m_totalCount = serverTotalCount;
    if (m_pagesFetched == 0) {
        m_initialTotalCount = serverTotalCount;
        if (serverTotalCount == 0 && itemsCount == 0 && !m_hasError) {
            m_explicitZeroTotal = true;
        }
    } else {
        if (m_initialTotalCount >= 0 && serverTotalCount >= 0 && serverTotalCount != m_initialTotalCount) {
            m_limitReached = true;
        }
    }

    m_itemsAccumulated += itemsCount;
    m_pagesFetched++;
}

void TrackerPaginator::recordError() {
    m_hasError = true;
    if (m_totalCount >= 0 && m_itemsAccumulated < m_totalCount) {
        m_limitReached = true;
    }
}

bool TrackerPaginator::shouldFetchMore(bool serverHasMore) noexcept {
    if (!serverHasMore || m_hasError) {
        return false;
    }
    if (m_pagesFetched >= m_maxPages) {
        m_limitReached = true;
        return false;
    }
    return true;
}

TrackerPaginator::Outcome TrackerPaginator::finalize(const QVector<RemoteIssueItem>& items) const {
    Outcome outcome;
    outcome.items = items;
    outcome.limitReached = m_limitReached;
    outcome.hasError = m_hasError;
    outcome.explicitZeroTotal = m_explicitZeroTotal;

    if (m_totalCount >= 0 && items.size() < m_totalCount) {
        outcome.limitReached = true;
    }

    return outcome;
}

} // namespace gofi
