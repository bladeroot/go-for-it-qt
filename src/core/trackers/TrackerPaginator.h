#pragma once

#include "TrackerCommon.h"
#include <QVector>

namespace gofi {

class TrackerPaginator {
public:
    struct Outcome {
        QVector<RemoteIssueItem> items;
        bool limitReached = false;
        bool hasError = false;
        bool explicitZeroTotal = false;

        [[nodiscard]] bool canReconcile() const noexcept {
            return !limitReached && !hasError && (!items.isEmpty() || explicitZeroTotal);
        }
    };

    explicit TrackerPaginator(int maxPages = 50);

    void recordPage(int itemsCount, int serverTotalCount = -1, bool incompleteResults = false);
    void recordError();
    [[nodiscard]] bool shouldFetchMore(bool serverHasMore) noexcept;
    [[nodiscard]] Outcome finalize(const QVector<RemoteIssueItem>& items) const;

    [[nodiscard]] int maxPages() const noexcept { return m_maxPages; }
    [[nodiscard]] int pagesFetched() const noexcept { return m_pagesFetched; }
    [[nodiscard]] int totalCount() const noexcept { return m_totalCount; }
    [[nodiscard]] int initialTotalCount() const noexcept { return m_initialTotalCount; }
    [[nodiscard]] int itemsAccumulated() const noexcept { return m_itemsAccumulated; }
    [[nodiscard]] bool isLimitReached() const noexcept { return m_limitReached; }
    [[nodiscard]] bool hasError() const noexcept { return m_hasError; }
    [[nodiscard]] bool isExplicitZeroTotal() const noexcept { return m_explicitZeroTotal; }

    void setLimitReached(bool val = true) noexcept { m_limitReached = val; }
    void setHasError(bool val = true) noexcept { m_hasError = val; }
    void setExplicitZeroTotal(bool val = true) noexcept { m_explicitZeroTotal = val; }

private:
    int m_maxPages = 50;
    int m_pagesFetched = 0;
    int m_totalCount = -1;
    int m_initialTotalCount = -1;
    int m_itemsAccumulated = 0;
    bool m_limitReached = false;
    bool m_hasError = false;
    bool m_explicitZeroTotal = false;
};

} // namespace gofi
