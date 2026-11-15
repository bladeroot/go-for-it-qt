#pragma once

#include "ListRepository.h"
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

namespace gofi {

class IssueTrackerService;

class IssueTrackerProvider {
public:
    using StatusReplyHandler = std::function<void(QNetworkReply*)>;
    using StatusErrorHandler = std::function<void(int, const QString&)>;

    explicit IssueTrackerProvider(IssueTrackerService* service);
    virtual ~IssueTrackerProvider() = default;

    [[nodiscard]] virtual QString name() const = 0;

    virtual void syncList(const TodoListEntry& entry) = 0;
    virtual void postWorklog(const TodoListEntry& entry,
                             qint64 taskId,
                             qint64 activityLogId,
                             const QString& issueKey,
                             const QDateTime& startTimeUtc,
                             quint32 runtimeSeconds,
                             bool forceBearerAuth,
                             const QString& baseUrl) = 0;
    virtual void updateDescription(const TodoListEntry& entry,
                                   const QString& issueKey,
                                   const QString& markdownDetails,
                                   bool forceBearerAuth,
                                   bool useV2Fallback,
                                   const QString& baseUrl) = 0;
    virtual void updateStatusRemote(const TodoListEntry& entry,
                                    const QString& issueKey,
                                    bool done,
                                    const QString& localStatus,
                                    bool forceBearerAuth,
                                    const QString& baseUrl,
                                    const StatusReplyHandler& handleReply,
                                    const StatusErrorHandler& failWithError) = 0;
    virtual void postComment(const TodoListEntry& entry,
                             const QString& issueKey,
                             const QString& markdownComment,
                             bool forceBearerAuth,
                             bool useV2Fallback,
                             const QString& baseUrl) = 0;
    virtual void fetchRemoteStatuses(const QString& baseUrl,
                                     const TodoListEntry& tempEntry,
                                     const QVariantMap& existingMapping) = 0;
    virtual void fetchRemotePriorities(const QString& baseUrl,
                                       const TodoListEntry& tempEntry,
                                       const QVariantMap& existingMapping) = 0;
    [[nodiscard]] virtual QVariantList defaultPriorities(const QVariantMap& existingMapping) const = 0;
    [[nodiscard]] virtual QUrl buildBrowserUrl(const TodoListEntry& entry,
                                               const QString& issueKey) const = 0;

protected:
    [[nodiscard]] IssueTrackerService* service() const noexcept { return m_service; }

private:
    IssueTrackerService* m_service;
};

} // namespace gofi
