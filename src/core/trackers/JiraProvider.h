#pragma once

#include "trackers/IssueTrackerProvider.h"

namespace gofi {

class JiraProvider : public IssueTrackerProvider {
public:
    using IssueTrackerProvider::IssueTrackerProvider;
    [[nodiscard]] QString name() const override { return QStringLiteral("Jira"); }

    void syncList(const TodoListEntry& entry) override;
    void postWorklog(const TodoListEntry& entry,
                     qint64 taskId,
                     qint64 activityLogId,
                     const QString& issueKey,
                     const QDateTime& startTimeUtc,
                     quint32 runtimeSeconds,
                     bool forceBearerAuth,
                     const QString& baseUrl) override;
    void updateDescription(const TodoListEntry& entry,
                           const QString& issueKey,
                           const QString& markdownDetails,
                           bool forceBearerAuth,
                           bool useV2Fallback,
                           const QString& baseUrl) override;
    void updateStatusRemote(const TodoListEntry& entry,
                            const QString& issueKey,
                            bool done,
                            const QString& localStatus,
                            bool forceBearerAuth,
                            const QString& baseUrl,
                            const StatusReplyHandler& handleReply,
                            const StatusErrorHandler& failWithError) override;
    void postComment(const TodoListEntry& entry,
                     const QString& issueKey,
                     const QString& markdownComment,
                     bool forceBearerAuth,
                     bool useV2Fallback,
                     const QString& baseUrl) override;
    void fetchRemoteStatuses(const QString& baseUrl,
                             const TodoListEntry& tempEntry,
                             const QVariantMap& existingMapping) override;
    void fetchRemotePriorities(const QString& baseUrl,
                               const TodoListEntry& tempEntry,
                               const QVariantMap& existingMapping) override;
    [[nodiscard]] QVariantList defaultPriorities(const QVariantMap& existingMapping) const override;
    [[nodiscard]] QUrl buildBrowserUrl(const TodoListEntry& entry,
                                       const QString& issueKey) const override;
};

} // namespace gofi
