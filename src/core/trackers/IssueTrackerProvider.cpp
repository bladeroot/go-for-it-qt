#include "trackers/IssueTrackerProvider.h"

namespace gofi {

IssueTrackerProvider::IssueTrackerProvider(IssueTrackerService* service)
    : m_service(service)
{
}

} // namespace gofi
