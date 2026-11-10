#include "core/ActivityLog.h"
#include "core/AmbientSoundService.h"
#include "core/AnalyticsService.h"
#include "core/AppController.h"
#include "core/IssueTrackerService.h"
#include "core/NotificationService.h"
#include "core/Schedule.h"
#include "core/ShortcutManager.h"
#include "core/SystemHookService.h"
#include "todotxt/TaskFilterProxyModel.h"
#include "todotxt/TaskManager.h"
#include "todotxt/TaskStoreModel.h"
#include "todotxt/TxtTask.h"
#include "tui/TuiApp.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDesktopServices>
#include <QtTest>
#include "ShortcutManager.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlContext>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>

using namespace gofi;

namespace {
// Minimal HTTP/1.1 responder for tracker tests: buffers each request until its
// Content-Length body is complete, then answers once via the handler.
using HttpHandler = std::function<QByteArray(const QByteArray& method, const QByteArray& target, const QByteArray& body)>;
using HttpFullHandler = std::function<QByteArray(const QByteArray& method, const QByteArray& target, const QByteArray& head, const QByteArray& body)>;

void serveHttpFull(QTcpServer& server, const HttpFullHandler& handler) {
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, handler]() {
        while (server.hasPendingConnections()) {
            QTcpSocket* socket = server.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, buffer, handler]() {
                buffer->append(socket->readAll());
                while (true) {
                    const int headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    const QByteArray head = buffer->left(headerEnd);
                    qsizetype contentLength = 0;
                    for (const QByteArray& line : head.split('\n')) {
                        if (line.trimmed().toLower().startsWith("content-length:")) {
                            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                        }
                    }
                    if (buffer->size() < headerEnd + 4 + contentLength) return;
                    const QByteArray body = buffer->mid(headerEnd + 4, contentLength);
                    buffer->remove(0, headerEnd + 4 + contentLength);
                    const QList<QByteArray> requestLine = head.left(head.indexOf("\r\n")).split(' ');
                    const QByteArray method = requestLine.value(0);
                    const QByteArray target = requestLine.value(1);
                    socket->write(handler(method, target, head, body));
                }
            });
        }
    });
}

void serveHttp(QTcpServer& server, const HttpHandler& handler) {
    serveHttpFull(server, [handler](const QByteArray& method, const QByteArray& target, const QByteArray&, const QByteArray& body) {
        return handler(method, target, body);
    });
}

QByteArray httpResponse(const QByteArray& status, const QByteArray& contentType, const QByteArray& body,
                        const QByteArray& extraHeaders = QByteArray()) {
    return "HTTP/1.1 " + status + "\r\n"
           "Content-Type: " + contentType + "\r\n" + extraHeaders +
           "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
}
} // namespace

// Captures URLs that would be handed to the desktop, so tests never open anything.
class UrlOpenSpy : public QObject {
    Q_OBJECT
public:
    QList<QUrl> opened;
public slots:
    void handle(const QUrl& url) { opened.append(url); }
};

class TestTodoTxt : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_testSandbox;

private slots:
    void initTestCase() {
        NotificationService::setAudioPlaybackEnabled(false);
        SystemHookService::setSystemIntegrationEnabled(false);
        QVERIFY(m_testSandbox.isValid());
        const QString configDir = m_testSandbox.filePath(QStringLiteral("config"));
        const QString dataDir = m_testSandbox.filePath(QStringLiteral("data"));
        QDir().mkpath(configDir);
        QDir().mkpath(dataDir);

        qputenv("HOME", m_testSandbox.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", configDir.toUtf8());
        qputenv("XDG_DATA_HOME", dataDir.toUtf8());
        QStandardPaths::setTestModeEnabled(true);

        QSettings probe(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("go-for-it"));
        QVERIFY(probe.fileName().startsWith(m_testSandbox.path()));
    }

    void cleanupTestCase() {
        if (m_testSandbox.isValid()) {
            m_testSandbox.remove();
        }
    }

    void cleanup() {
        DatabaseManager::instance().close();
        SystemHookService::setSystemIntegrationEnabled(false);
    }

    void testParseTodoTxtFullLine() {
        const QString line = QStringLiteral("x (B) 2026-11-05 2026-11-01 Write unit tests +GoForItQt @coding timer:1h-15m-30s duration:2h");
        TxtTask task = TxtTask::fromTodoTxt(line, false);

        QVERIFY(task.isValid());
        QVERIFY(task.isDone());
        QCOMPARE(task.priority(), static_cast<quint8>(1)); // 'B' -> 1
        QCOMPARE(task.priorityString(), QStringLiteral("(B)"));
        QCOMPARE(task.completionDate(), QDate(2026, 11, 5));
        QCOMPARE(task.creationDate(), QDate(2026, 11, 1));
        QCOMPARE(task.description(), QStringLiteral("Write unit tests +GoForItQt @coding"));
        QCOMPARE(task.timerValue(), 4530U);
        QCOMPARE(task.duration(), 7200U);
        QCOMPARE(task.toTodoTxt(true), line);
    }

    // KB-29: duration components that do not fit in quint32 seconds must remain
    // ordinary text. Silently wrapping them produces an unrelated, plausible
    // timer value and removes the user's original token from the description.
    void testDurationTokensRejectArithmeticOverflow() {
        const QString hugeHours = QStringLiteral("1193047h");
        const TxtTask durationTask = TxtTask::fromSimpleTxt(
            QStringLiteral("Estimate duration:") + hugeHours, false, false);
        const TxtTask timerTask = TxtTask::fromTodoTxt(
            QStringLiteral("Track timer:") + hugeHours, false);

        const bool durationRejected = durationTask.duration() == 0
            && durationTask.description().contains(QStringLiteral("duration:") + hugeHours);
        const bool timerRejected = timerTask.timerValue() == 0
            && timerTask.description().contains(QStringLiteral("timer:") + hugeHours);

        QVERIFY(durationRejected && timerRejected);
    }

    void testSimpleTxtAndComparison() {
        TxtTask tA = TxtTask::fromSimpleTxt(QStringLiteral("(A) Urgent task @work duration:25m"), false, false);
        TxtTask tC = TxtTask::fromSimpleTxt(QStringLiteral("(C) Later task +home"), false, false);
        TxtTask tNone = TxtTask::fromSimpleTxt(QStringLiteral("Unprioritized task"), false, false);

        QCOMPARE(tA.priority(), static_cast<quint8>(0));
        QCOMPARE(tA.duration(), 1500U);
        QVERIFY(tA.compare(tC) < 0);
        QVERIFY(tC.compare(tNone) < 0);
    }

    void testFilterProxyModelAndPriorityConstraints() {
        TaskStoreModel store(false);
        store.appendTask(TxtTask::fromSimpleTxt(QStringLiteral("(A) Fix bug +GoForIt @desk"), false, false));
        store.appendTask(TxtTask::fromSimpleTxt(QStringLiteral("(C) Buy groceries @market"), false, false));
        store.appendTask(TxtTask::fromSimpleTxt(QStringLiteral("Read book @home"), false, false));

        TaskFilterProxyModel proxy;
        proxy.setSourceModel(&store);
        QCOMPARE(proxy.rowCount(), 3);

        proxy.setFilterQuery(QStringLiteral("+goforit"));
        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.data(proxy.index(0, 0), TaskStoreModel::DescriptionRole).toString(),
                 QStringLiteral("Fix bug +GoForIt @desk"));

        proxy.setFilterQuery(QStringLiteral("priority:a"));
        QCOMPARE(proxy.rowCount(), 1);

        proxy.setFilterQuery(QStringLiteral("priority:a-c"));
        QCOMPARE(proxy.rowCount(), 2);
    }

    void testSchedule() {
        Schedule sched({1500, 300, 1500, 900});
        QVERIFY(sched.isValid());
        QCOMPARE(sched.length(), 2);
        QCOMPARE(sched.taskDuration(0), 1500);
        QCOMPARE(sched.breakDuration(0), 300);
        QCOMPARE(sched.taskDuration(1), 1500);
        QCOMPARE(sched.breakDuration(1), 900);
        QCOMPARE(sched.breakDuration(2), 300);
    }

    // KB-83: qAbs(INT_MIN) cannot be represented by int and leaves a negative
    // duration in the schedule. Schedule inputs can come from persisted settings
    // and QVariant lists, so normalization must handle the full int domain.
    void testScheduleNormalizesMinimumIntegerDurations() {
        const int minimum = std::numeric_limits<int>::min();
        if (qEnvironmentVariableIsSet("GOFI_KB83_CHILD")) {
            Schedule imported({minimum, minimum});
            Schedule appended;
            appended.append(minimum, minimum);
            QVERIFY(imported.taskDuration(0) >= 0);
            QVERIFY(imported.breakDuration(0) >= 0);
            QVERIFY(appended.taskDuration(0) >= 0);
            QVERIFY(appended.breakDuration(0) >= 0);
            return;
        }

        QProcess child;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("GOFI_KB83_CHILD"), QStringLiteral("1"));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("testScheduleNormalizesMinimumIntegerDurations"), QStringLiteral("-silent")});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForFinished(30000));

        const bool normalizedWithoutCrash = child.exitStatus() == QProcess::NormalExit
            && child.exitCode() == 0;

        QVERIFY(normalizedWithoutCrash);
    }

    void testShortcutsAndSoundResolution() {
        ShortcutManager sm;
        sm.resetDefaults();
        QCOMPARE(sm.shortcutFor(QStringLiteral("filter")), QStringLiteral("Ctrl+F"));

        // Assign "Ctrl+F" to "sort" -> should clear "filter" due to conflict resolution
        sm.setShortcut(QStringLiteral("sort"), QStringLiteral("Ctrl+F"));
        QCOMPARE(sm.shortcutFor(QStringLiteral("sort")), QStringLiteral("Ctrl+F"));
        QCOMPARE(sm.shortcutFor(QStringLiteral("filter")), QString());
        sm.resetDefaults();

        const QUrl bellUrl = NotificationService::resolveSoundUrl(QStringLiteral("bell.ogg"));
        QVERIFY(bellUrl.isValid());
        QVERIFY(QFileInfo::exists(bellUrl.toLocalFile()));
    }

    void testAppControllerAndSqlitePersistence() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString dbPath = tempDir.filePath(QStringLiteral("test_goforit.db"));
        const QString csvPath = tempDir.filePath(QStringLiteral("activity.csv"));

        QString listId;
        int initialCount = 0;
        {
            AppController ctrl(csvPath, dbPath);
            listId = ctrl.listManager()->addList(QStringLiteral("SQLiteTestList"));
            ctrl.selectListById(listId);
            QCOMPARE(ctrl.currentListId(), listId);

            initialCount = ctrl.todoProxyModel()->rowCount();
            ctrl.addNewTask(QStringLiteral("(A) Port Go-For-It to SQLite +QtPort @dev duration:30m"));
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), initialCount + 1);

            // Move task row and persist sort_order in SQLite
            ctrl.setActiveTodoRow(0);
            ctrl.moveActiveTaskByOffset(1);
            QCOMPARE(ctrl.activeTodoRow(), 1);

            QVERIFY(ctrl.timer()->hasActiveTask());
            ctrl.markActiveTaskDone();
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), initialCount);
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 1);
        }

        // Verify persistence by reloading from the same SQLite database
        const QVector<TxtTask> reloadedTodo = DatabaseManager::instance().loadTasks(listId, false);
        const QVector<TxtTask> reloadedDone = DatabaseManager::instance().loadTasks(listId, true);
        QCOMPARE(reloadedTodo.size(), initialCount);
        QCOMPARE(reloadedDone.size(), 1);
    }

    void testJiraAndYouTrackIntegration() {
        const QByteArray jiraSample = R"({
            "issues": [
                {
                    "key": "PROJ-101",
                    "fields": {
                        "summary": "Implement SQLite storage",
                        "description": "Detailed task specification for SQLite.",
                        "comment": {
                            "comments": [
                                {
                                    "author": { "displayName": "Alice" },
                                    "created": "2026-11-05T14:20:00.000+0000",
                                    "body": "Looks good to me!"
                                }
                            ]
                        },
                        "project": { "key": "PROJ" },
                        "status": { "name": "In Progress" },
                        "priority": { "name": "High" },
                        "timetracking": { "originalEstimateSeconds": 1800 }
                    }
                }
            ]
        })";
        const QVector<RemoteIssueItem> jiraItems = IssueTrackerService::parseJiraIssuesJson(jiraSample);
        QCOMPARE(jiraItems.size(), 1);
        QCOMPARE(jiraItems[0].key, QStringLiteral("PROJ-101"));
        QCOMPARE(jiraItems[0].priority, static_cast<quint8>(1)); // High -> (B)
        QCOMPARE(jiraItems[0].durationSeconds, 1800U);
        QCOMPARE(jiraItems[0].description, QStringLiteral("PROJ-101 Implement SQLite storage +PROJ @In_Progress"));
        QCOMPARE(jiraItems[0].details, QStringLiteral("Detailed task specification for SQLite."));
        QCOMPARE(jiraItems[0].comments.size(), 1);

        const QByteArray ytSample = R"([
            {
                "idReadable": "YT-55",
                "summary": "Add YouTrack workItems sync",
                "project": { "shortName": "YT" },
                "customFields": [
                    { "name": "Priority", "value": { "name": "Critical" } },
                    { "name": "State", "value": { "name": "Open" } },
                    { "name": "Estimation", "value": { "minutes": 45 } }
                ]
            }
        ])";
        const QVector<RemoteIssueItem> ytItems = IssueTrackerService::parseYouTrackIssuesJson(ytSample);
        QCOMPARE(ytItems.size(), 1);
        QCOMPARE(ytItems[0].key, QStringLiteral("YT-55"));
        QCOMPARE(ytItems[0].priority, static_cast<quint8>(1)); // Critical -> (B)
        QCOMPARE(ytItems[0].durationSeconds, 2700U);
        QCOMPARE(ytItems[0].description, QStringLiteral("YT-55 Add YouTrack workItems sync +YT @Open"));
        QCOMPARE(IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("In Progress")), QStringLiteral("in_progress"));
        QCOMPARE(IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("On Hold")), QStringLiteral("paused"));
        QCOMPARE(IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("Resolved")), QStringLiteral("done"));
        QCOMPARE(IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("Won't Fix")), QStringLiteral("canceled"));
        QCOMPARE(IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("merged")), QStringLiteral("done"));
        QCOMPARE(IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("draft")), QStringLiteral("paused"));

        QVariantMap customMap{{QStringLiteral("Custom Review"), QStringLiteral("paused")}};
        QCOMPARE(IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("Custom Review"), {}, customMap), QStringLiteral("paused"));
    }

    void testGitHubIssuesAndPullRequestsParser() {
        const QByteArray ghJson = QByteArray(
            "{"
            "  \"total_count\": 2,"
            "  \"items\": ["
            "    {"
            "      \"number\": 42,"
            "      \"title\": \"Fix timer crash on startup\","
            "      \"body\": \"Detailed markdown issue body\","
            "      \"state\": \"open\","
            "      \"draft\": false,"
            "      \"labels\": [{\"name\": \"bug\"}, {\"name\": \"urgent\"}],"
            "      \"repository_url\": \"https://api.github.com/repos/bladeroot/go-for-it-qt\""
            "    },"
            "    {"
            "      \"number\": 99,"
            "      \"title\": \"Add GitHub and GitLab integrations\","
            "      \"body\": \"PR description with markdown\","
            "      \"state\": \"open\","
            "      \"draft\": true,"
            "      \"pull_request\": {\"url\": \"https://api.github.com/repos/bladeroot/go-for-it-qt/pulls/99\"},"
            "      \"labels\": [{\"name\": \"feature\"}],"
            "      \"repository_url\": \"https://api.github.com/repos/bladeroot/go-for-it-qt\""
            "    }"
            "  ]"
            "}"
        );

        const QVector<RemoteIssueItem> items = IssueTrackerService::parseGitHubIssuesJson(ghJson);
        QCOMPARE(items.size(), 2);

        // Item 1: Issue
        QCOMPARE(items[0].key, QStringLiteral("GH#bladeroot/go-for-it-qt#42"));
        QVERIFY(items[0].description.contains(QStringLiteral("GH#42 Fix timer crash on startup")));
        QVERIFY(items[0].description.contains(QStringLiteral("+go-for-it-qt")));
        QVERIFY(items[0].description.contains(QStringLiteral("+bug")));
        QVERIFY(items[0].description.contains(QStringLiteral("@open")));
        QCOMPARE(items[0].details, QStringLiteral("Detailed markdown issue body"));
        QCOMPARE(items[0].remoteStatus, QStringLiteral("open"));

        // Item 2: Draft Pull Request
        QCOMPARE(items[1].key, QStringLiteral("PR#bladeroot/go-for-it-qt#99"));
        QVERIFY(items[1].description.startsWith(QStringLiteral("[DRAFT PR] PR#99 Add GitHub and GitLab integrations")));
        QCOMPARE(items[1].remoteStatus, QStringLiteral("draft"));
    }

    void testGitLabIssuesAndMergeRequestsParser() {
        const QByteArray glIssuesJson = QByteArray(
            "["
            "  {"
            "    \"id\": 101,"
            "    \"iid\": 15,"
            "    \"title\": \"Implement dark mode toggle\","
            "    \"description\": \"GitLab issue details\","
            "    \"state\": \"opened\","
            "    \"labels\": [\"enhancement\"],"
            "    \"web_url\": \"https://gitlab.com/group/myproject/-/issues/15\","
            "    \"time_stats\": {"
            "      \"time_estimate\": 3600"
            "    }"
            "  }"
            "]"
        );

        const QVector<RemoteIssueItem> issues = IssueTrackerService::parseGitLabIssuesJson(glIssuesJson);
        QCOMPARE(issues.size(), 1);
        QCOMPARE(issues[0].key, QStringLiteral("GL#group/myproject#15"));
        QVERIFY(issues[0].description.contains(QStringLiteral("GL#15 Implement dark mode toggle")));
        QVERIFY(issues[0].description.contains(QStringLiteral("+myproject")));
        QCOMPARE(issues[0].remoteStatus, QStringLiteral("opened"));
        QCOMPARE(issues[0].durationSeconds, 3600U);

        const QByteArray glMrsJson = QByteArray(
            "["
            "  {"
            "    \"id\": 202,"
            "    \"iid\": 7,"
            "    \"title\": \"Draft: Refactor network layer\","
            "    \"description\": \"MR details\","
            "    \"state\": \"opened\","
            "    \"draft\": true,"
            "    \"labels\": [\"refactoring\"],"
            "    \"web_url\": \"https://gitlab.com/group/myproject/-/merge_requests/7\","
            "    \"time_stats\": {"
            "      \"time_estimate\": 1800"
            "    }"
            "  }"
            "]"
        );

        const QVector<RemoteIssueItem> mrs = IssueTrackerService::parseGitLabMergeRequestsJson(glMrsJson);
        QCOMPARE(mrs.size(), 1);
        QCOMPARE(mrs[0].key, QStringLiteral("MR!group/myproject!7"));
        QVERIFY(mrs[0].description.startsWith(QStringLiteral("[DRAFT MR] MR!7 Draft: Refactor network layer")));
        QCOMPARE(mrs[0].remoteStatus, QStringLiteral("draft"));
        QCOMPARE(mrs[0].durationSeconds, 1800U);
    }

    void testRedmineIssuesParser() {
        const QByteArray rmJson = QByteArray(
            "{"
            "  \"issues\": ["
            "    {"
            "      \"id\": 1024,"
            "      \"project\": {\"id\": 3, \"name\": \"Mobile Client\", \"identifier\": \"mobile-client\"},"
            "      \"tracker\": {\"id\": 1, \"name\": \"Defect\"},"
            "      \"status\": {\"id\": 2, \"name\": \"In Progress\", \"is_closed\": false},"
            "      \"priority\": {\"id\": 4, \"name\": \"High\"},"
            "      \"subject\": \"Crash on push notification\","
            "      \"description\": \"Stacktrace details\","
            "      \"estimated_hours\": 2.5"
            "    }"
            "  ],"
            "  \"total_count\": 1"
            "}"
        );

        const QVector<RemoteIssueItem> items = IssueTrackerService::parseRedmineIssuesJson(rmJson);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].key, QStringLiteral("RM#mobile-client#1024"));
        QVERIFY(items[0].description.contains(QStringLiteral("RM#1024 Crash on push notification")));
        QVERIFY(items[0].description.contains(QStringLiteral("+mobile-client")));
        QVERIFY(items[0].description.contains(QStringLiteral("+Defect")));
        QVERIFY(items[0].description.contains(QStringLiteral("@In_Progress")));
        QCOMPARE(items[0].details, QStringLiteral("Stacktrace details"));
        QCOMPARE(items[0].remoteStatus, QStringLiteral("In Progress"));
        QCOMPARE(items[0].durationSeconds, 9000U);
    }

    void testTodoistTasksParser() {
        const QByteArray tdJson = QByteArray(
            "["
            "  {"
            "    \"id\": \"7723910\","
            "    \"content\": \"Refactor database migration\","
            "    \"description\": \"Clean up old columns\","
            "    \"is_completed\": false,"
            "    \"priority\": 4,"
            "    \"labels\": [\"backend\", \"p1\"]"
            "  }"
            "]"
        );

        const QVector<RemoteIssueItem> items = IssueTrackerService::parseTodoistTasksJson(tdJson);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].key, QStringLiteral("TD#7723910"));
        QVERIFY(items[0].description.contains(QStringLiteral("TD#7723910 Refactor database migration")));
        QVERIFY(items[0].description.contains(QStringLiteral("+backend")));
        QVERIFY(items[0].description.contains(QStringLiteral("@open")));
        QCOMPARE(items[0].priority, 0);
        QCOMPARE(items[0].details, QStringLiteral("Clean up old columns"));
        QCOMPARE(items[0].remoteStatus, QStringLiteral("open"));
    }

    void testNextcloudCalDavTasksParser() {
        const QByteArray icsData = QByteArray(
            "BEGIN:VCALENDAR\r\n"
            "VERSION:2.0\r\n"
            "PRODID:-//Nextcloud Tasks v0.15.0//EN\r\n"
            "BEGIN:VTODO\r\n"
            "UID:9c3f2b1a-8e4d-4c7a-b912-3f8d1e2a3b4c\r\n"
            "CREATED:20261101T100000Z\r\n"
            "SUMMARY:Review Nextcloud server backup logs\r\n"
            "DESCRIPTION:Check restic snapshots on backup NAS\r\n"
            "PRIORITY:1\r\n"
            "STATUS:NEEDS-ACTION\r\n"
            "CATEGORIES:sysadmin,backups\r\n"
            "DURATION:PT1H30M\r\n"
            "COMMENT:Verified disk space is at 45%\r\n"
            "END:VTODO\r\n"
            "END:VCALENDAR\r\n"
        );

        const QVector<RemoteIssueItem> items = IssueTrackerService::parseNextcloudTasksIcs(icsData);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].key, QStringLiteral("NC#9c3f2b1a-8e4d-4c7a-b912-3f8d1e2a3b4c"));
        QVERIFY(items[0].description.contains(QStringLiteral("NC#9c3f2b1a Review Nextcloud server backup logs")));
        QVERIFY(items[0].description.contains(QStringLiteral("+sysadmin")));
        QVERIFY(items[0].description.contains(QStringLiteral("+backups")));
        QVERIFY(items[0].description.contains(QStringLiteral("@needs-action")));
        QCOMPARE(items[0].priority, 0);
        QCOMPARE(items[0].details, QStringLiteral("Check restic snapshots on backup NAS"));
        QCOMPARE(items[0].remoteStatus, QStringLiteral("needs-action"));
        QCOMPARE(items[0].durationSeconds, 5400U);
        QCOMPARE(items[0].comments.size(), 1);
    }

    void testMultiListDatabaseLoading() {
        DatabaseManager& db = DatabaseManager::instance();
        db.initialize(QStringLiteral(":memory:"));

        TodoListEntry l1;
        l1.id = QStringLiteral("list_gh");
        l1.name = QStringLiteral("GitHub Repo");
        l1.provider = QStringLiteral("GitHub");
        db.saveList(l1, 0);

        TodoListEntry l2;
        l2.id = QStringLiteral("list_local");
        l2.name = QStringLiteral("Personal");
        l2.provider = QStringLiteral("SQLite");
        db.saveList(l2, 1);

        TxtTask t1 = TxtTask::fromSimpleTxt(QStringLiteral("PR#1 Fix bug"), false, false);
        db.insertTask(l1.id, t1, 0);

        TxtTask t2 = TxtTask::fromSimpleTxt(QStringLiteral("Buy milk"), false, false);
        db.insertTask(l2.id, t2, 0);

        // Load specific list
        const QVector<TxtTask> ghTasks = db.loadTasks(l1.id, false);
        QCOMPARE(ghTasks.size(), 1);
        QCOMPARE(ghTasks[0].listName(), QStringLiteral("GitHub Repo"));
        QCOMPARE(ghTasks[0].listProvider(), QStringLiteral("GitHub"));

        // Load multiple lists
        const QVector<TxtTask> bothTasks = db.loadTasks(QStringList{l1.id, l2.id}, false);
        QCOMPARE(bothTasks.size(), 2);

        // Load all lists with "__ALL__"
        const QVector<TxtTask> allTasks = db.loadTasks(QStringList{QStringLiteral("__ALL__")}, false);
        QCOMPARE(allTasks.size(), 2);
    }

    void testUnviewedAndTotalCounters() {
        TaskStoreModel store(false);
        TxtTask t1 = TxtTask::fromSimpleTxt(QStringLiteral("Task 1"), false, false);
        t1.setViewed(false);
        t1.setStatus(QStringLiteral("todo"));

        TxtTask t2 = TxtTask::fromSimpleTxt(QStringLiteral("Task 2"), false, false);
        t2.setViewed(true);
        t2.setStatus(QStringLiteral("todo"));

        TxtTask t3 = TxtTask::fromSimpleTxt(QStringLiteral("Task 3"), false, false);
        t3.setViewed(false);
        t3.setStatus(QStringLiteral("in_progress")); // not "todo" status

        store.appendTask(t1);
        store.appendTask(t2);
        store.appendTask(t3);

        QCOMPARE(store.totalTodoCount(), 2);    // t1 and t2
        QCOMPARE(store.unviewedTodoCount(), 1); // only t1

        store.markTaskViewed(0);
        QCOMPARE(store.unviewedTodoCount(), 0);
        QCOMPARE(store.totalTodoCount(), 2);
    }

    void testBuildIssueBrowserUrl() {
        TodoListEntry ghEntry;
        ghEntry.provider = QStringLiteral("GitHub");
        ghEntry.jiraUrl = QStringLiteral("https://github.com");

        // GitHub PR with owner/repo in key
        const QUrl ghPrUrl = IssueTrackerService::buildIssueBrowserUrl(ghEntry, QStringLiteral("PR#myorg/myrepo#42"));
        QCOMPARE(ghPrUrl.toString(), QStringLiteral("https://github.com/myorg/myrepo/pull/42"));

        // GitHub Issue with owner/repo in key
        const QUrl ghIssueUrl = IssueTrackerService::buildIssueBrowserUrl(ghEntry, QStringLiteral("GH#myorg/myrepo#108"));
        QCOMPARE(ghIssueUrl.toString(), QStringLiteral("https://github.com/myorg/myrepo/issues/108"));

        // GitHub with repo in jiraJql
        ghEntry.jiraJql = QStringLiteral("repo:myorg/customrepo");
        const QUrl ghJqlPrUrl = IssueTrackerService::buildIssueBrowserUrl(ghEntry, QStringLiteral("PR#77"));
        QCOMPARE(ghJqlPrUrl.toString(), QStringLiteral("https://github.com/myorg/customrepo/pull/77"));

        // GitHub Enterprise self-hosted
        ghEntry.jiraUrl = QStringLiteral("https://ghe.mycorp.internal/api/v3");
        ghEntry.jiraJql = QStringLiteral("");
        const QUrl gheUrl = IssueTrackerService::buildIssueBrowserUrl(ghEntry, QStringLiteral("GH#corp/secret#999"));
        QCOMPARE(gheUrl.toString(), QStringLiteral("https://ghe.mycorp.internal/corp/secret/issues/999"));

        // GitLab MR
        TodoListEntry glEntry;
        glEntry.provider = QStringLiteral("GitLab");
        glEntry.jiraUrl = QStringLiteral("https://gitlab.com");
        const QUrl glMrUrl = IssueTrackerService::buildIssueBrowserUrl(glEntry, QStringLiteral("MR!mygroup/subproj!15"));
        QCOMPARE(glMrUrl.toString(), QStringLiteral("https://gitlab.com/mygroup/subproj/-/merge_requests/15"));

        // GitLab Issue
        const QUrl glIssueUrl = IssueTrackerService::buildIssueBrowserUrl(glEntry, QStringLiteral("GL#mygroup/subproj#25"));
        QCOMPARE(glIssueUrl.toString(), QStringLiteral("https://gitlab.com/mygroup/subproj/-/issues/25"));

        // Jira
        TodoListEntry jiraEntry;
        jiraEntry.provider = QStringLiteral("Jira");
        jiraEntry.jiraUrl = QStringLiteral("https://mycompany.atlassian.net/");
        const QUrl jiraUrl = IssueTrackerService::buildIssueBrowserUrl(jiraEntry, QStringLiteral("PROJ-456"));
        QCOMPARE(jiraUrl.toString(), QStringLiteral("https://mycompany.atlassian.net/browse/PROJ-456"));

        // YouTrack
        TodoListEntry ytEntry;
        ytEntry.provider = QStringLiteral("YouTrack");
        ytEntry.jiraUrl = QStringLiteral("https://company.youtrack.cloud");
        const QUrl ytUrl = IssueTrackerService::buildIssueBrowserUrl(ytEntry, QStringLiteral("DEV-789"));
        QCOMPARE(ytUrl.toString(), QStringLiteral("https://company.youtrack.cloud/issue/DEV-789"));

        // Redmine
        TodoListEntry rmEntry;
        rmEntry.provider = QStringLiteral("Redmine");
        rmEntry.jiraUrl = QStringLiteral("https://redmine.org");
        const QUrl rmUrl = IssueTrackerService::buildIssueBrowserUrl(rmEntry, QStringLiteral("RM#1234"));
        QCOMPARE(rmUrl.toString(), QStringLiteral("https://redmine.org/issues/1234"));

        // Todoist
        TodoListEntry tdEntry;
        tdEntry.provider = QStringLiteral("Todoist");
        const QUrl tdUrl = IssueTrackerService::buildIssueBrowserUrl(tdEntry, QStringLiteral("TD#99887766"));
        QCOMPARE(tdUrl.toString(), QStringLiteral("https://todoist.com/app/task/99887766"));

        // Nextcloud
        TodoListEntry ncEntry;
        ncEntry.provider = QStringLiteral("Nextcloud");
        ncEntry.jiraUrl = QStringLiteral("https://cloud.example.org");
        const QUrl ncUrl = IssueTrackerService::buildIssueBrowserUrl(ncEntry, QStringLiteral("uid-abc"));
        QCOMPARE(ncUrl.toString(), QStringLiteral("https://cloud.example.org/apps/tasks/"));

        // Fallback: direct URL in task description
        TodoListEntry localEntry;
        localEntry.provider = QStringLiteral("SQLite");
        const QUrl localUrl = IssueTrackerService::buildIssueBrowserUrl(localEntry, QString(), QStringLiteral("Fix bug https://docs.qt.io/qt-6/qurl.html +research"));
        QCOMPARE(localUrl.toString(), QStringLiteral("https://docs.qt.io/qt-6/qurl.html"));
    }

    void testAnalyticsAndBilling() {
        // Tag extraction tests
        const QString sample = QStringLiteral("Implement billing report +Analytics +GoForIt @office @urgent priority:high");
        const QStringList projs = AnalyticsService::extractProjects(sample);
        QCOMPARE(projs.size(), 2);
        QVERIFY(projs.contains(QStringLiteral("Analytics")));
        QVERIFY(projs.contains(QStringLiteral("GoForIt")));

        const QStringList ctxs = AnalyticsService::extractContexts(sample);
        QCOMPARE(ctxs.size(), 2);
        QVERIFY(ctxs.contains(QStringLiteral("office")));
        QVERIFY(ctxs.contains(QStringLiteral("urgent")));

        // Duration formatting
        QCOMPARE(AnalyticsService::formatDuration(0), QStringLiteral("0m"));
        QCOMPARE(AnalyticsService::formatDuration(1500), QStringLiteral("25m"));
        QCOMPARE(AnalyticsService::formatDuration(3660), QStringLiteral("1h 01m"));
        QCOMPARE(AnalyticsService::formatDuration(7200), QStringLiteral("2h 00m"));

        // Temporary Database for Activity Logs
        const QString testDbPath = m_testSandbox.filePath(QStringLiteral("gofi_analytics_test.db"));
        QFile::remove(testDbPath);
        DatabaseManager& db = DatabaseManager::instance();
        db.initialize(testDbPath);

        const QDateTime now = QDateTime::currentDateTimeUtc();
        // Insert 2 sessions: 1 completed pomodoro (1500s) + 1 short session (600s)
        const qint64 id1 = db.insertActivityLog(
            QStringLiteral("list-1"), QStringLiteral("Work"), 101, QStringLiteral("PR#1"),
            QStringLiteral("Feature A +ProjectX @coding"), now.addSecs(-7200), 1500, now.addSecs(-5700), false, true
        );
        QVERIFY(id1 > 0);

        const qint64 id2 = db.insertActivityLog(
            QStringLiteral("list-1"), QStringLiteral("Work"), 102, QStringLiteral("GH#2"),
            QStringLiteral("Bugfix B +ProjectY @testing"), now.addSecs(-3600), 600, now.addSecs(-3000), false, false
        );
        QVERIFY(id2 > 0);

        // Load records
        const QVector<ActivityRecord> logs = db.loadActivityLogs();
        QVERIFY(logs.size() >= 2);

        AnalyticsService analytics;
        const QVariantMap stats = analytics.getProductivityStats(QStringLiteral("all"));
        QCOMPARE(stats.value(QStringLiteral("totalSessions")).toInt(), 2);
        QCOMPARE(stats.value(QStringLiteral("totalSeconds")).toULongLong(), 2100ULL);
        QCOMPARE(stats.value(QStringLiteral("completedPomodoros")).toInt(), 1);

        const QVariantList pBreakdown = stats.value(QStringLiteral("projectBreakdown")).toList();
        QCOMPARE(pBreakdown.size(), 2);

        // Heatmap
        const QVariantList heatmap = analytics.getActivityHeatmap(14);
        QCOMPARE(heatmap.size(), 14);

        // CSV Export
        const QString csvPath = m_testSandbox.filePath(QStringLiteral("test_billing.csv"));
        QFile::remove(csvPath);
        QVERIFY(analytics.exportBillingCsv(csvPath, QStringLiteral("all")));
        QVERIFY(QFile::exists(csvPath));
        {
            QFile f(csvPath);
            QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
            const QString content = QString::fromUtf8(f.readAll());
            QVERIFY(content.contains(QStringLiteral("Feature A")));
            QVERIFY(content.contains(QStringLiteral("ProjectX")));
            QVERIFY(content.contains(QStringLiteral("TOTAL SUMMARY")));
        }
        QFile::remove(csvPath);

        // PDF Export
        const QString pdfPath = m_testSandbox.filePath(QStringLiteral("test_billing.pdf"));
        QFile::remove(pdfPath);
        QVERIFY(analytics.exportBillingPdf(pdfPath, QStringLiteral("all")));
        QVERIFY(QFile::exists(pdfPath));
        QVERIFY(QFileInfo(pdfPath).size() > 500);
        QFile::remove(pdfPath);
        QFile::remove(testDbPath);
    }

    void testDueDatesAndSmartFilters() {
        const QDate refDate(2026, 11, 7);
        const QDate yesterday = refDate.addDays(-1);
        const QDate inThreeDays = refDate.addDays(3);
        const QDate nextMonth = refDate.addMonths(1);

        TxtTask taskOverdue = TxtTask::fromSimpleTxt(
            QStringLiteral("Fix critical bug due:%1 +security").arg(yesterday.toString(Qt::ISODate)), false, false
        );
        QVERIFY(taskOverdue.dueDate().isValid());
        QCOMPARE(taskOverdue.dueDate(), yesterday);
        QVERIFY(taskOverdue.isOverdue(refDate));
        QVERIFY(!taskOverdue.isDueToday(refDate));
        QVERIFY(!taskOverdue.isDueThisWeek(refDate));

        TxtTask taskToday = TxtTask::fromSimpleTxt(
            QStringLiteral("Ship release v2 due:%1 @release").arg(refDate.toString(Qt::ISODate)), false, false
        );
        QVERIFY(taskToday.dueDate().isValid());
        QCOMPARE(taskToday.dueDate(), refDate);
        QVERIFY(!taskToday.isOverdue(refDate));
        QVERIFY(taskToday.isDueToday(refDate));
        QVERIFY(taskToday.isDueThisWeek(refDate));

        TxtTask taskWeek = TxtTask::fromSimpleTxt(
            QStringLiteral("Weekly meeting due:%1 +sync").arg(inThreeDays.toString(Qt::ISODate)), false, false
        );
        QVERIFY(taskWeek.dueDate().isValid());
        QVERIFY(!taskWeek.isOverdue(refDate));
        QVERIFY(!taskWeek.isDueToday(refDate));
        QVERIFY(taskWeek.isDueThisWeek(refDate));

        TxtTask taskFar = TxtTask::fromSimpleTxt(
            QStringLiteral("Future planning due:%1").arg(nextMonth.toString(Qt::ISODate)), false, false
        );
        QVERIFY(taskFar.dueDate().isValid());
        QVERIFY(!taskFar.isOverdue(refDate));
        QVERIFY(!taskFar.isDueToday(refDate));
        QVERIFY(!taskFar.isDueThisWeek(refDate));

        // Model & Proxy filtering tests
        TaskStoreModel model;
        model.appendTask(taskOverdue);
        model.appendTask(taskToday);
        model.appendTask(taskWeek);
        model.appendTask(taskFar);
        QCOMPARE(model.rowCount(), 4);

        TaskFilterProxyModel proxy;
        proxy.setReferenceDate(refDate);
        proxy.setSourceModel(&model);
        QCOMPARE(proxy.rowCount(), 4);

        // Filter: overdue
        proxy.setDueFilter(QStringLiteral("overdue"));
        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.data(proxy.index(0, 0), TaskStoreModel::DueDateStringRole).toString(), yesterday.toString(Qt::ISODate));

        // Filter: today
        proxy.setDueFilter(QStringLiteral("today"));
        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.data(proxy.index(0, 0), TaskStoreModel::DueDateStringRole).toString(), refDate.toString(Qt::ISODate));

        // Filter: week
        proxy.setDueFilter(QStringLiteral("week"));
        QCOMPARE(proxy.rowCount(), 2); // today + inThreeDays

        // Reset filter
        proxy.setDueFilter(QString());
        QCOMPARE(proxy.rowCount(), 4);

        // Filter via query string "due:today"
        proxy.setFilterQuery(QStringLiteral("due:today"));
        QCOMPARE(proxy.rowCount(), 1);

        proxy.setFilterQuery(QStringLiteral("due:overdue"));
        QCOMPARE(proxy.rowCount(), 1);

        proxy.setFilterQuery(QStringLiteral("due:week"));
        QCOMPARE(proxy.rowCount(), 2);
    }

    void testAmbientSoundGeneration() {
        const QString whiteNoisePath = m_testSandbox.filePath(QStringLiteral("test_ambient_white.wav"));
        const QString rainPath = m_testSandbox.filePath(QStringLiteral("test_ambient_rain.wav"));
        const QString clockPath = m_testSandbox.filePath(QStringLiteral("test_ambient_clock.wav"));

        QFile::remove(whiteNoisePath);
        QFile::remove(rainPath);
        QFile::remove(clockPath);

        QVERIFY(AmbientSoundService::generateAmbientWav(QStringLiteral("white_noise"), whiteNoisePath));
        QVERIFY(QFile::exists(whiteNoisePath));
        QVERIFY(QFileInfo(whiteNoisePath).size() > 44);

        QVERIFY(AmbientSoundService::generateAmbientWav(QStringLiteral("rain"), rainPath));
        QVERIFY(QFile::exists(rainPath));
        QVERIFY(QFileInfo(rainPath).size() > 44);

        QVERIFY(AmbientSoundService::generateAmbientWav(QStringLiteral("clock"), clockPath));
        QVERIFY(QFile::exists(clockPath));
        QVERIFY(QFileInfo(clockPath).size() > 44);

        // Verify RIFF WAVE header in one of the files
        QFile f(clockPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray hdr = f.read(12);
        QVERIFY(hdr.startsWith("RIFF"));
        QVERIFY(hdr.endsWith("WAVE"));
        f.close();

        QFile::remove(whiteNoisePath);
        QFile::remove(rainPath);
        QFile::remove(clockPath);
    }

    void testIntegrationPriorityMapping() {
        IssueTrackerService service;

        // 1. Default presets for different providers
        const QVariantList jiraDefaults = service.defaultPrioritiesForProvider(QStringLiteral("Jira"));
        QVERIFY(!jiraDefaults.isEmpty());
        bool foundHighest = false;
        for (const QVariant& item : jiraDefaults) {
            const QVariantMap m = item.toMap();
            if (m.value(QStringLiteral("remoteName")).toString() == QStringLiteral("Highest")) {
                QCOMPARE(m.value(QStringLiteral("localPriority")).toString(), QStringLiteral("A"));
                foundHighest = true;
            }
        }
        QVERIFY(foundHighest);

        const QVariantList ytDefaults = service.defaultPrioritiesForProvider(QStringLiteral("YouTrack"));
        QVERIFY(!ytDefaults.isEmpty());
        bool foundShowStopper = false;
        for (const QVariant& item : ytDefaults) {
            const QVariantMap m = item.toMap();
            if (m.value(QStringLiteral("remoteName")).toString() == QStringLiteral("Show-stopper")) {
                QCOMPARE(m.value(QStringLiteral("localPriority")).toString(), QStringLiteral("A"));
                foundShowStopper = true;
            }
        }
        QVERIFY(foundShowStopper);

        const QVariantList ghDefaults = service.defaultPrioritiesForProvider(QStringLiteral("GitHub"));
        QVERIFY(!ghDefaults.isEmpty());

        const QVariantList todoistDefaults = service.defaultPrioritiesForProvider(QStringLiteral("Todoist"));
        QVERIFY(!todoistDefaults.isEmpty());

        const QVariantList ncDefaults = service.defaultPrioritiesForProvider(QStringLiteral("Nextcloud"));
        QVERIFY(!ncDefaults.isEmpty());

        // 2. Custom priority mapping logic
        QVariantMap customJiraMapping;
        customJiraMapping[QStringLiteral("Blocker")] = QStringLiteral("A");
        customJiraMapping[QStringLiteral("Critical")] = QStringLiteral("A");
        customJiraMapping[QStringLiteral("Minor")] = QStringLiteral("D");
        customJiraMapping[QStringLiteral("Trivial")] = QStringLiteral("none");

        // Custom mapping takes precedence
        QCOMPARE(IssueTrackerService::mapRemotePriorityToLocal(QStringLiteral("Blocker"), customJiraMapping), static_cast<quint8>(0));
        QCOMPARE(IssueTrackerService::mapRemotePriorityToLocal(QStringLiteral("blocker"), customJiraMapping), static_cast<quint8>(0)); // Case-insensitive
        QCOMPARE(IssueTrackerService::mapRemotePriorityToLocal(QStringLiteral("Minor"), customJiraMapping), static_cast<quint8>(3));
        QCOMPARE(IssueTrackerService::mapRemotePriorityToLocal(QStringLiteral("Trivial"), customJiraMapping), TxtTask::NoPriority);

        // Fallback to default heuristic if not in custom mapping
        QCOMPARE(IssueTrackerService::mapRemotePriorityToLocal(QStringLiteral("High"), customJiraMapping), static_cast<quint8>(1));

        // 3. Database persistence of priorityMapping in TodoListEntry
        const QString testDb = m_testSandbox.filePath(QStringLiteral("gofi_prio_mapping_test.db"));
        QFile::remove(testDb);
        DatabaseManager& db = DatabaseManager::instance();
        db.initialize(testDb);

        TodoListEntry entry;
        entry.id = QStringLiteral("jira-custom-prio");
        entry.name = QStringLiteral("Jira Integration");
        entry.provider = QStringLiteral("Jira");
        entry.jiraUrl = QStringLiteral("https://jira.example.com");
        entry.priorityMapping = customJiraMapping;

        QVERIFY(db.saveList(entry, 0));

        const QVector<TodoListEntry> loadedLists = db.loadLists();
        QCOMPARE(loadedLists.size(), 1);
        QCOMPARE(loadedLists.first().id, QStringLiteral("jira-custom-prio"));
        QCOMPARE(loadedLists.first().priorityMapping.size(), customJiraMapping.size());
        QCOMPARE(loadedLists.first().priorityMapping.value(QStringLiteral("Blocker")).toString(), QStringLiteral("A"));
        QCOMPARE(loadedLists.first().priorityMapping.value(QStringLiteral("Trivial")).toString(), QStringLiteral("none"));

        // 4. Remote issue sync priority assignment
        QByteArray sampleJiraJson = R"({
            "issues": [
                {
                    "key": "PROJ-1",
                    "fields": {
                        "summary": "Blocker bug in login",
                        "status": {"name": "In Progress"},
                        "priority": {"name": "Blocker"}
                    }
                },
                {
                    "key": "PROJ-2",
                    "fields": {
                        "summary": "Trivial typo",
                        "status": {"name": "Open"},
                        "priority": {"name": "Trivial"}
                    }
                }
            ]
        })";

        const QVector<RemoteIssueItem> parsed = IssueTrackerService::parseJiraIssuesJson(sampleJiraJson);
        QCOMPARE(parsed.size(), 2);
        QCOMPARE(parsed[0].remotePriority, QStringLiteral("Blocker"));
        QCOMPARE(parsed[1].remotePriority, QStringLiteral("Trivial"));

        // When upserted using the entry's custom priority mapping:
        const quint8 prio1 = IssueTrackerService::mapRemotePriorityToLocal(parsed[0].remotePriority, entry.priorityMapping);
        const quint8 prio2 = IssueTrackerService::mapRemotePriorityToLocal(parsed[1].remotePriority, entry.priorityMapping);
        QCOMPARE(prio1, static_cast<quint8>(0)); // (A)
        QCOMPARE(prio2, TxtTask::NoPriority);    // none

        QFile::remove(testDb);
    }

    void testSystemHookServiceAndFocusSettings() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("test_gofi_hooks.db"));
        QFile::remove(testDb);

        AppController controller(QString(), testDb);
        SettingsManager* settings = controller.settings();
        SystemHookService* hooks = controller.systemHooks();
        QVERIFY(settings != nullptr);
        QVERIFY(hooks != nullptr);

        // Test focus & hook settings
        bool signalEmitted = false;
        connect(settings, &SettingsManager::hookSettingsChanged, [&signalEmitted]() {
            signalEmitted = true;
        });

        settings->setDndFocusEnabled(true);
        QVERIFY(settings->dndFocusEnabled());
        QVERIFY(signalEmitted);

        signalEmitted = false;
        settings->setInhibitSleepEnabled(true);
        QVERIFY(settings->inhibitSleepEnabled());
        QVERIFY(signalEmitted);

        signalEmitted = false;
        settings->setCustomHooksEnabled(true);
        QVERIFY(settings->customHooksEnabled());
        QVERIFY(signalEmitted);

        settings->setHookWorkStartCommand(QStringLiteral("echo 'working'"));
        QCOMPARE(settings->hookWorkStartCommand(), QStringLiteral("echo 'working'"));

        settings->setHookTaskCompletedCommand(QStringLiteral("echo 'done'"));
        QCOMPARE(settings->hookTaskCompletedCommand(), QStringLiteral("echo 'done'"));

        // Test hooks directory creation and sample script
        const QString hooksDir = m_testSandbox.filePath(QStringLiteral("test_gofi_hooks"));
        QDir(hooksDir).removeRecursively();
        hooks->setCustomHooksDirectory(hooksDir);
        const QString dirPath = hooks->hooksDirectory();
        QCOMPARE(dirPath, hooksDir);
        QVERIFY(QDir(dirPath).exists());
        QVERIFY(QFile::exists(QDir(dirPath).filePath(QStringLiteral("sample_hook.sh"))));

        // Test testHook execution
        QVERIFY(hooks->testHook(QStringLiteral("work_start")));
        QVERIFY(hooks->testHook(QStringLiteral("task_completed")));
        QVERIFY(!hooks->testHook(QStringLiteral("invalid_event")));

        // Test timer start triggers work_start hook / DND
        controller.addNewTask(QStringLiteral("Test Task for Hook @focus"));
        controller.setActiveTodoRow(0);
        controller.timer()->start();
        QVERIFY(controller.timer()->isRunning());
        controller.timer()->stop();
        QVERIFY(!controller.timer()->isRunning());

        QFile::remove(testDb);
        QDir(hooksDir).removeRecursively();
    }

    void testListManagerModelEntryAtAndGet() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("test_list_mgr.db"));
        QFile::remove(testDb);

        AppController controller(QString(), testDb);
        ListManagerModel* mgr = controller.listManager();
        QVERIFY(mgr != nullptr);
        QVERIFY(mgr->rowCount() > 0);

        // Test Q_INVOKABLE entryAt and get returning QVariantMap for QML
        const QVariantMap entry0 = mgr->entryAt(0);
        QVERIFY(!entry0.isEmpty());
        QVERIFY(entry0.contains(QStringLiteral("id")));
        QVERIFY(entry0.contains(QStringLiteral("name")));
        QVERIFY(entry0.contains(QStringLiteral("provider")));
        QCOMPARE(entry0.value(QStringLiteral("provider")).toString(), QStringLiteral("SQLite"));

        const QVariantMap get0 = mgr->get(0);
        QCOMPARE(get0.value(QStringLiteral("id")).toString(), entry0.value(QStringLiteral("id")).toString());

        // Out of bounds check
        const QVariantMap outOfBounds = mgr->entryAt(999);
        QVERIFY(outOfBounds.isEmpty());

        // C++ entry() method check
        const TodoListEntry cppEntry = mgr->entry(0);
        QCOMPARE(cppEntry.id, entry0.value(QStringLiteral("id")).toString());

        QFile::remove(testDb);
    }

    void testRealHookProcessExecutionInSandbox() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("test_real_hook.db"));
        QFile::remove(testDb);

        AppController controller(QString(), testDb);
        SettingsManager* settings = controller.settings();
        SystemHookService* hooks = controller.systemHooks();
        QVERIFY(settings != nullptr);
        QVERIFY(hooks != nullptr);

        SystemHookService::setSystemIntegrationEnabled(true);
        const auto resetIntegration = qScopeGuard([]() {
            SystemHookService::setSystemIntegrationEnabled(false);
        });

        const QString hookOutputDir = m_testSandbox.filePath(QStringLiteral("hook_run"));
        QDir().mkpath(hookOutputDir);
        const QString markerFile = QDir(hookOutputDir).filePath(QStringLiteral("hook_executed.marker"));
        QFile::remove(markerFile);

        settings->setCustomHooksEnabled(true);
        settings->setHookWorkStartCommand(QStringLiteral("echo 'test_payload' > '%1'").arg(markerFile));

        QSignalSpy spy(hooks, &SystemHookService::hookExecuted);
        QVERIFY(hooks->testHook(QStringLiteral("work_start")));

        QVERIFY(spy.wait(2000));
        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(0).toString(), QStringLiteral("work_start"));
        QCOMPARE(args.at(1).toBool(), true);

        QVERIFY(QFile::exists(markerFile));
        QFile f(markerFile);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        QCOMPARE(QString::fromUtf8(f.readAll()).trimmed(), QStringLiteral("test_payload"));

        SystemHookService::setSystemIntegrationEnabled(false);
        QFile::remove(testDb);
        QFile::remove(markerFile);
    }

    void testDatabaseBackupImportSafety() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("safety_test.db"));
        QFile::remove(testDb);

        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(testDb));
        QVERIFY(db.isOpen());

        const QFile::Permissions perms = QFile::permissions(testDb);
        QVERIFY(perms & QFile::ReadOwner);
        QVERIFY(perms & QFile::WriteOwner);
        QVERIFY(!(perms & QFile::ReadOther));
        QVERIFY(!(perms & QFile::WriteOther));

        TodoListEntry entry;
        entry.id = QStringLiteral("list1");
        entry.name = QStringLiteral("Primary List");
        QVERIFY(db.saveList(entry, 0));

        TxtTask t1 = TxtTask::fromSimpleTxt(QStringLiteral("Task 1 to stay safe"), false, false);
        QVERIFY(db.insertTask(entry.id, t1, 0) > 0);

        TxtTask t2 = TxtTask::fromSimpleTxt(QStringLiteral("Task 2 to stay safe"), false, false);
        QVERIFY(db.insertTask(entry.id, t2, 1) > 0);

        QCOMPARE(db.countAllTasksInList(entry.id), 2);

        // 1. Export-to-self test: must fail and NOT delete live DB!
        QVERIFY(!db.exportDatabaseBackup(testDb));
        QVERIFY(QFile::exists(testDb));
        QCOMPARE(db.countAllTasksInList(entry.id), 2);

        const QString relativeSelf = QDir::current().relativeFilePath(testDb);
        QVERIFY(!db.exportDatabaseBackup(relativeSelf));
        QVERIFY(QFile::exists(testDb));
        QCOMPARE(db.countAllTasksInList(entry.id), 2);

        // 2. Import-from-self test: must fail and NOT destroy live DB!
        QVERIFY(!db.importDatabaseBackup(testDb));
        QVERIFY(QFile::exists(testDb));
        QCOMPARE(db.countAllTasksInList(entry.id), 2);

        // 3. Import non-existent file test: must fail, live DB remains
        QVERIFY(!db.importDatabaseBackup(m_testSandbox.filePath(QStringLiteral("non_existent.db"))));
        QCOMPARE(db.countAllTasksInList(entry.id), 2);

        // 4. Import arbitrary non-SQLite file test: must fail, live DB remains intact
        const QString corruptFile = m_testSandbox.filePath(QStringLiteral("corrupt.txt"));
        {
            QFile f(corruptFile);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            f.write("This is not an SQLite database!");
            f.close();
        }
        QVERIFY(!db.importDatabaseBackup(corruptFile));
        QCOMPARE(db.countAllTasksInList(entry.id), 2);

        // 5. Successful export and restore
        const QString validBackup = m_testSandbox.filePath(QStringLiteral("valid_backup.db"));
        QVERIFY(db.exportDatabaseBackup(validBackup));
        QVERIFY(QFile::exists(validBackup));

        TxtTask t3 = TxtTask::fromSimpleTxt(QStringLiteral("Task 3 added after backup"), false, false);
        QVERIFY(db.insertTask(entry.id, t3, 2) > 0);
        QCOMPARE(db.countAllTasksInList(entry.id), 3);

        QVERIFY(db.importDatabaseBackup(validBackup));
        QVERIFY(db.isOpen());
        QCOMPARE(db.countAllTasksInList(entry.id), 2);

        const QVector<TxtTask> tasks = db.loadTasks(entry.id, false);
        QCOMPARE(tasks.size(), 2);
        QCOMPARE(tasks.at(0).description(), QStringLiteral("Task 1 to stay safe"));

        db.close();
        QFile::remove(testDb);
        QFile::remove(validBackup);
        QFile::remove(corruptFile);
    }

    void testTaskCompletedHookCallback() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("hook_test.db"));
        QFile::remove(testDb);

        AppController controller(QString(), testDb);
        SettingsManager* settings = controller.settings();
        SystemHookService* hooks = controller.systemHooks();
        QVERIFY(settings != nullptr);
        QVERIFY(hooks != nullptr);

        SystemHookService::setSystemIntegrationEnabled(true);
        const auto resetIntegration = qScopeGuard([]() {
            SystemHookService::setSystemIntegrationEnabled(false);
        });

        const QString markerFile = m_testSandbox.filePath(QStringLiteral("task_completed.marker"));
        QFile::remove(markerFile);

        settings->setCustomHooksEnabled(true);
        settings->setHookTaskCompletedCommand(QStringLiteral("echo \"$GOFI_TASK_TITLE|$GOFI_TASK_KEY\" >> '%1'").arg(markerFile));

        const QString listId = controller.listManager()->addList(QStringLiteral("HookList"));
        controller.selectListById(listId);
        const int initialCount = controller.todoProxyModel()->rowCount();

        controller.addNewTask(QStringLiteral("First task GH#123"));
        controller.addNewTask(QStringLiteral("Second task PR#456"));

        QCOMPARE(controller.todoProxyModel()->rowCount(), initialCount + 2);

        const QString completedDesc = controller.todoProxyModel()->data(controller.todoProxyModel()->index(0, 0), TaskStoreModel::DescriptionRole).toString();

        QSignalSpy spy(hooks, &SystemHookService::hookExecuted);

        controller.toggleTaskDone(false, 0);

        QVERIFY(spy.wait(2000));
        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args.at(0).toString(), QStringLiteral("task_completed"));

        QVERIFY(QFile::exists(markerFile));
        {
            QFile f(markerFile);
            QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
            const QString content = QString::fromUtf8(f.readAll());
            f.close();
            QVERIFY(content.contains(completedDesc));
        }

        // Test changeTaskStatus with lowercase "done"
        QCOMPARE(controller.todoProxyModel()->rowCount(), initialCount + 1);
        const QString secondDesc = controller.todoProxyModel()->data(controller.todoProxyModel()->index(0, 0), TaskStoreModel::DescriptionRole).toString();

        controller.changeTaskStatus(false, 0, QStringLiteral("done"));

        QVERIFY(spy.wait(2000));
        QCOMPARE(spy.count(), 1);
        const QList<QVariant> args2 = spy.takeFirst();
        QCOMPARE(args2.at(0).toString(), QStringLiteral("task_completed"));

        {
            QFile f(markerFile);
            QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
            const QString content = QString::fromUtf8(f.readAll());
            f.close();
            QVERIFY(content.contains(secondDesc));
        }

        SystemHookService::setSystemIntegrationEnabled(false);
        DatabaseManager::instance().close();
        QFile::remove(testDb);
        QFile::remove(markerFile);
    }

    void testTerminalSanitization() {
        const QString osc52Attack = QStringLiteral("Normal Task\033]52;c;cGFzc3dvcmQ=\007 Title");
        const QString cleanOsc = TuiApp::sanitizeTerminalText(osc52Attack);
        QVERIFY(!cleanOsc.contains(QLatin1Char('\033')));
        QVERIFY(!cleanOsc.contains(QStringLiteral("52;c;")));
        QCOMPARE(cleanOsc, QStringLiteral("Normal Task Title"));

        const QString oscTitle = QStringLiteral("Task\033]0;Hacked Title\033\\ Done");
        const QString cleanTitle = TuiApp::sanitizeTerminalText(oscTitle);
        QVERIFY(!cleanTitle.contains(QStringLiteral("Hacked Title")));
        QCOMPARE(cleanTitle, QStringLiteral("Task Done"));

        const QString csiAttack = QStringLiteral("Before\033[2J\033[?25lAfter");
        const QString cleanCsi = TuiApp::sanitizeTerminalText(csiAttack);
        QCOMPARE(cleanCsi, QStringLiteral("BeforeAfter"));

        const QString controlChars = QStringLiteral("Line1\x01\x02\x07\x1bLine2");
        const QString cleanCtrl = TuiApp::sanitizeTerminalText(controlChars);
        QVERIFY(!cleanCtrl.contains(QLatin1Char('\x01')));
        QVERIFY(!cleanCtrl.contains(QLatin1Char('\x07')));
        QVERIFY(!cleanCtrl.contains(QLatin1Char('\x1b')));
    }

    void testSafeUriSchemeAndCsvSanitization() {
        // 1. TxtTask URL scheme whitelist
        const TxtTask taskFile = TxtTask::fromSimpleTxt(QStringLiteral("file:///etc/passwd smb://share/dir ssh://server/path"), false, false);
        for (const TxtPart& p : taskFile.parts()) {
            QVERIFY(p.type != TxtPartType::Uri);
        }

        const TxtTask taskSafe = TxtTask::fromSimpleTxt(QStringLiteral("Visit http://example.com and https://secure.com or mailto:user@mail.com"), false, false);
        int uriCount = 0;
        for (const TxtPart& p : taskSafe.parts()) {
            if (p.type == TxtPartType::Uri) {
                uriCount++;
            }
        }
        QCOMPARE(uriCount, 3);

        // 2. CSV formula injection sanitization
        const QString testCsv = m_testSandbox.filePath(QStringLiteral("formula_test.csv"));
        QFile::remove(testCsv);

        const QString testDb = m_testSandbox.filePath(QStringLiteral("analytics_test.db"));
        QFile::remove(testDb);

        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(testDb));

        TodoListEntry entry;
        entry.id = QStringLiteral("csv_list");
        entry.name = QStringLiteral("=CmdList");
        QVERIFY(db.saveList(entry, 0));

        db.insertActivityLog(
            entry.id,
            entry.name,
            -1,
            QStringLiteral("KEY-1"),
            QStringLiteral("=SUM(1+1) dangerous task"),
            QDateTime::currentDateTimeUtc().addSecs(-3600),
            3600,
            QDateTime::currentDateTimeUtc(),
            false,
            true
        );

        AnalyticsService analytics;
        QVERIFY(analytics.exportBillingCsv(testCsv, QStringLiteral("all"), entry.id));

        QFile csvFile(testCsv);
        QVERIFY(csvFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString csvContent = QString::fromUtf8(csvFile.readAll());
        csvFile.close();

        QVERIFY(csvContent.contains(QStringLiteral("\"'=SUM(1+1) dangerous task\"")));
        QVERIFY(csvContent.contains(QStringLiteral("\"'=CmdList\"")));

        // 3. ActivityLog::logTaskInFile CSV formula injection sanitization
        const QString actCsv = m_testSandbox.filePath(QStringLiteral("act_formula_test.csv"));
        QFile::remove(actCsv);
        QVERIFY(ActivityLog::logTaskInFile(actCsv, QStringLiteral("+DangerousList"), QStringLiteral("@SUM(1+1)"), QDateTime::currentDateTimeUtc(), 60, QDateTime::currentDateTimeUtc()));
        QFile actCsvFile(actCsv);
        QVERIFY(actCsvFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString actCsvContent = QString::fromUtf8(actCsvFile.readAll());
        actCsvFile.close();
        QVERIFY(actCsvContent.contains(QStringLiteral("\"'+DangerousList\"")));
        QVERIFY(actCsvContent.contains(QStringLiteral("\"'@SUM(1+1)\"")));
        QFile::remove(actCsv);

        db.close();
        QFile::remove(testCsv);
        QFile::remove(testDb);
    }

    void testMultiListOperationsAndCommentPreservation() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("multilist_test.db"));
        QFile::remove(testDb);
        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(testDb));

        TodoListEntry list1;
        list1.id = QStringLiteral("list1");
        list1.name = QStringLiteral("List One");
        QVERIFY(db.saveList(list1, 0));

        TodoListEntry list2;
        list2.id = QStringLiteral("list2");
        list2.name = QStringLiteral("List Two");
        QVERIFY(db.saveList(list2, 1));

        // Insert tasks in list1 and list2
        TxtTask t1 = TxtTask::fromSimpleTxt(QStringLiteral("Task 1 in List 1"), false, false);
        const qint64 id1 = db.insertTask(list1.id, t1, 0);
        QVERIFY(id1 >= 0);

        TxtTask t2 = TxtTask::fromSimpleTxt(QStringLiteral("x Done task in List 1"), false, false);
        t2.setDone(true);
        const qint64 id2 = db.insertTask(list1.id, t2, 1);
        QVERIFY(id2 >= 0);

        TxtTask t3 = TxtTask::fromSimpleTxt(QStringLiteral("x Done task in List 2"), false, false);
        t3.setDone(true);
        const qint64 id3 = db.insertTask(list2.id, t3, 0);
        QVERIFY(id3 >= 0);

        // Test multi-list clearDoneList via TaskManager
        TaskManager mgr;
        mgr.setActiveLists(QStringList{list1.id, list2.id});
        QCOMPARE(mgr.doneModel()->rowCount(), 2);
        mgr.clearDoneList();
        QCOMPARE(mgr.doneModel()->rowCount(), 0);

        // Verify done tasks were deleted in BOTH list1 and list2 in the database
        const auto remainingList1Done = db.loadTasks(list1.id, true);
        const auto remainingList2Done = db.loadTasks(list2.id, true);
        QCOMPARE(remainingList1Done.size(), 0);
        QCOMPARE(remainingList2Done.size(), 0);

        // But todo task in list1 remains
        const auto remainingList1Todo = db.loadTasks(list1.id, false);
        QCOMPARE(remainingList1Todo.size(), 1);

        // Test TaskManager::addNewTask failing safely when DB insertion fails (foreign key violation)
        TaskManager invalidDbMgr;
        invalidDbMgr.setActiveList(QStringLiteral("non_existent_list_id"));
        QSignalSpy errorSpy(&invalidDbMgr, &TaskManager::errorOccurred);
        invalidDbMgr.addNewTask(QStringLiteral("Phantom task"), false, false);
        QCOMPARE(invalidDbMgr.todoModel()->rowCount(), 0);
        QCOMPARE(errorSpy.count(), 1);

        // Test remote issue comments preservation
        QVariantList initialComments;
        initialComments.append(QVariantMap{
            {QStringLiteral("author"), QStringLiteral("Alice")},
            {QStringLiteral("body"), QStringLiteral("Initial comment")},
            {QStringLiteral("created"), QStringLiteral("2026-11-08T00:00:00Z")}
        });

        bool isNew = false;
        QString oldStatus;
        QVERIFY(db.upsertRemoteIssueTask(list1.id, QStringLiteral("TEST-100"), QStringLiteral("Test issue"),
                                         QStringLiteral("todo"), 2, 60, QStringLiteral("Details"),
                                         initialComments, &isNew, &oldStatus));
        QVERIFY(isNew);

        // Verify comments saved
        auto loadedTasks = db.loadTasks(list1.id, false);
        TxtTask savedTask;
        for (const auto& t : loadedTasks) {
            if (t.jiraKey() == QStringLiteral("TEST-100")) {
                savedTask = t;
                break;
            }
        }
        QVERIFY(savedTask.isValid());
        QCOMPARE(savedTask.comments().size(), 1);

        // Update task with empty comments (simulating issue list sync where comments are omitted)
        QVariantList emptyComments;
        QVERIFY(db.upsertRemoteIssueTask(list1.id, QStringLiteral("TEST-100"), QStringLiteral("Updated description"),
                                         QStringLiteral("todo"), 2, 120, QStringLiteral("Updated details"),
                                         emptyComments, &isNew, &oldStatus));
        QVERIFY(!isNew);

        // Verify existing comments were preserved
        loadedTasks = db.loadTasks(list1.id, false);
        for (const auto& t : loadedTasks) {
            if (t.jiraKey() == QStringLiteral("TEST-100")) {
                savedTask = t;
                break;
            }
        }
        QCOMPARE(savedTask.comments().size(), 1);
        QCOMPARE(savedTask.description(), QStringLiteral("Updated description"));
        QCOMPARE(savedTask.details(), QStringLiteral("Updated details"));

        // Verify unique constraint idx_tasks_list_jira_key prevents duplicates on raw insert
        TxtTask duplicateTask = TxtTask::fromSimpleTxt(QStringLiteral("Duplicate TEST-100"), false, false);
        duplicateTask.setJiraKey(QStringLiteral("TEST-100"));
        const qint64 dupId = db.insertTask(list1.id, duplicateTask, 99);
        QCOMPARE(dupId, -1);

        // Test deduplication consolidating timer_seconds, duration_seconds, and jira_synced_seconds
        {
            {
                QSqlDatabase testSqlDb = QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn"));
                QSqlQuery q(testSqlDb);
                q.exec(QStringLiteral("DROP INDEX IF EXISTS idx_tasks_list_jira_key;"));
                q.exec(QStringLiteral(
                    "INSERT INTO tasks (list_id, description, done, status, priority, duration_seconds, timer_seconds, jira_key, jira_synced_seconds) "
                    "VALUES ('list1', 'Dup Task 1', 0, 'todo', 2, 500, 1000, 'DEDUP-1', 400);"
                ));
                q.exec(QStringLiteral(
                    "INSERT INTO tasks (list_id, description, done, status, priority, duration_seconds, timer_seconds, jira_key, jira_synced_seconds) "
                    "VALUES ('list1', 'Dup Task 2', 0, 'todo', 2, 1200, 4400, 'DEDUP-1', 600);"
                ));
            }

            // Re-initialize which runs consolidation and re-creates unique index
            db.close();
            QVERIFY(db.initialize(testDb));

            QSqlQuery qVerify(QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn")));
            qVerify.prepare(QStringLiteral("SELECT count(*), timer_seconds, duration_seconds, jira_synced_seconds FROM tasks WHERE list_id = 'list1' AND jira_key = 'DEDUP-1'"));
            QVERIFY(qVerify.exec() && qVerify.next());
            QCOMPARE(qVerify.value(0).toInt(), 1); // Only 1 survivor
            QCOMPARE(qVerify.value(1).toUInt(), 5400U); // 1000 + 4400 = 5400
            QCOMPARE(qVerify.value(2).toUInt(), 1200U); // max(500, 1200) = 1200
            QCOMPARE(qVerify.value(3).toUInt(), 1000U); // 400 + 600 = 1000
        }

        // Test GitHub sync without token and without repo fails cleanly
        {
            TodoListEntry ghList;
            ghList.id = QStringLiteral("gh_public");
            ghList.provider = QStringLiteral("GitHub");
            ghList.jiraUrl = QStringLiteral("https://github.com");
            ghList.apiToken = QString();
            ghList.jiraJql = QString();

            IssueTrackerService svc;
            QSignalSpy failSpy(&svc, &IssueTrackerService::syncFailed);
            svc.syncListIssues(ghList);
            QCOMPARE(failSpy.count(), 1);
        }

        db.close();
        QFile::remove(testDb);
    }

    void testJiraBearerAuthRetryWithLocalServer() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        const quint16 port = server.serverPort();

        int requestCount = 0;
        bool hadBasicAuth = false;
        bool hadBearerAuth = false;

        serveHttpFull(server, [&requestCount, &hadBasicAuth, &hadBearerAuth](const QByteArray&, const QByteArray&, const QByteArray& head, const QByteArray&) {
            requestCount++;
            const QByteArray lowerHead = head.toLower();
            if (lowerHead.contains("authorization: basic")) {
                hadBasicAuth = true;
                return httpResponse("401 Unauthorized", "application/json", "{}", "X-Seraph-Loginreason: AUTHENTICATED_FAILED\r\n");
            } else if (lowerHead.contains("authorization: bearer")) {
                hadBearerAuth = true;
                return httpResponse("200 OK", "application/json", "{\"issues\":[]}");
            }
            return httpResponse("400 Bad Request", "text/plain", "Missing auth");
        });

        IssueTrackerService svc;
        TodoListEntry entry;
        entry.id = QStringLiteral("jira_retry_test");
        entry.name = QStringLiteral("Jira Retry List");
        entry.provider = QStringLiteral("Jira");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
        entry.jiraUser = QStringLiteral("user@example.com");
        entry.apiToken = QStringLiteral("test_pat_token");

        QSignalSpy syncSpy(&svc, &IssueTrackerService::listIssuesSynced);
        svc.syncListIssues(entry);

        QVERIFY(svc.isSyncing());
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 30000);
        QCOMPARE(requestCount, 2);
        QVERIFY(hadBasicAuth);
        QVERIFY(hadBearerAuth);
        QCOMPARE(syncSpy.count(), 1);
        server.close();
    }

    void testRemoteClosedIssuesReconciliation() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("reconcile_test.db"));
        QFile::remove(testDb);
        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(testDb));

        TodoListEntry ghList;
        ghList.id = QStringLiteral("gh_reconcile");
        ghList.name = QStringLiteral("GitHub Reconcile");
        ghList.provider = QStringLiteral("GitHub");
        ghList.jiraUrl = QStringLiteral("https://github.com");
        ghList.apiToken = QStringLiteral("token");
        ghList.jiraJql = QStringLiteral("repo:test/proj");
        QVERIFY(db.saveList(ghList, 0));

        // Insert two open remote tasks
        TxtTask t1 = TxtTask::fromSimpleTxt(QStringLiteral("Issue 1"), false, false);
        t1.setJiraKey(QStringLiteral("TEST#test/proj#1"));
        QVERIFY(db.insertTask(ghList.id, t1, 0) >= 0);

        TxtTask t2 = TxtTask::fromSimpleTxt(QStringLiteral("Issue 2"), false, false);
        t2.setJiraKey(QStringLiteral("TEST#test/proj#2"));
        QVERIFY(db.insertTask(ghList.id, t2, 1) >= 0);

        QCOMPARE(db.loadTasks(ghList.id, false).size(), 2);
        QCOMPARE(db.loadTasks(ghList.id, true).size(), 0);

        // Simulate incoming sync where only Issue 2 is returned (Issue 1 was closed remotely)
        QVector<RemoteIssueItem> remoteItems;
        RemoteIssueItem item2;
        item2.key = QStringLiteral("TEST#test/proj#2");
        item2.description = QStringLiteral("Issue 2 updated");
        item2.remoteStatus = QStringLiteral("open");
        item2.priority = 2;
        remoteItems.append(item2);

        IssueTrackerService svc;
        QSignalSpy changesSpy(&svc, &IssueTrackerService::trackerChangesDetected);
        svc.processSyncedItems(ghList, remoteItems, QStringLiteral("GitHub"), false);

        // Verify Issue 1 was marked done locally because it was no longer in the open issues returned
        const auto openTasks = db.loadTasks(ghList.id, false);
        const auto doneTasks = db.loadTasks(ghList.id, true);
        QCOMPARE(openTasks.size(), 1);
        QCOMPARE(openTasks.first().jiraKey(), QStringLiteral("TEST#test/proj#2"));
        QCOMPARE(doneTasks.size(), 1);
        QCOMPARE(doneTasks.first().jiraKey(), QStringLiteral("TEST#test/proj#1"));
        QVERIFY(doneTasks.first().isDone());
        QCOMPARE(doneTasks.first().status(), QStringLiteral("done"));
        QCOMPARE(changesSpy.count(), 1);

        // Test 1: Truncated page / limit reached (>= 100 items) must NOT close any remaining open tasks
        {
            // Insert another task 3
            TxtTask t3 = TxtTask::fromSimpleTxt(QStringLiteral("Issue 3"), false, false);
            t3.setJiraKey(QStringLiteral("TEST#test/proj#3"));
            QVERIFY(db.insertTask(ghList.id, t3, 2) >= 0);

            QVector<RemoteIssueItem> pageOf100;
            for (int i = 0; i < 100; ++i) {
                RemoteIssueItem r;
                r.key = QStringLiteral("TEST#test/proj#%1").arg(100 + i);
                r.description = QStringLiteral("Page item %1").arg(i);
                r.remoteStatus = QStringLiteral("open");
                pageOf100.append(r);
            }
            // Neither item 2 nor item 3 are in pageOf100, but because size >= 100, reconciliation must NOT close them!
            svc.processSyncedItems(ghList, pageOf100, QStringLiteral("GitHub"), true);
            const auto remainingOpen = db.loadTasks(ghList.id, false);
            bool hasItem2 = false;
            bool hasItem3 = false;
            for (const auto& t : remainingOpen) {
                if (t.jiraKey() == QStringLiteral("TEST#test/proj#2")) hasItem2 = true;
                if (t.jiraKey() == QStringLiteral("TEST#test/proj#3")) hasItem3 = true;
            }
            QVERIFY(hasItem2);
            QVERIFY(hasItem3);
        }

        // Test 2: Empty response (items.isEmpty()) must NOT close any local open tasks
        {
            const auto openBeforeEmpty = db.loadTasks(ghList.id, false);
            QVERIFY(!openBeforeEmpty.isEmpty());
            svc.processSyncedItems(ghList, {}, QStringLiteral("GitHub"), false);
            const auto openAfterEmpty = db.loadTasks(ghList.id, false);
            QCOMPARE(openAfterEmpty.size(), openBeforeEmpty.size());
        }

        // Test 3: Error during sync (hasError == true) must NOT close any open tasks
        {
            const auto openBeforeError = db.loadTasks(ghList.id, false);
            QVector<RemoteIssueItem> partialItems;
            RemoteIssueItem r;
            r.key = QStringLiteral("TEST#test/proj#9999");
            r.description = QStringLiteral("Unrelated item");
            r.remoteStatus = QStringLiteral("open");
            partialItems.append(r);
            svc.processSyncedItems(ghList, partialItems, QStringLiteral("GitHub"), false, true /* hasError */);
            const auto openAfterError = db.loadTasks(ghList.id, false);
            // Verify existing local tasks were NOT closed
            bool hasItem2 = false;
            for (const auto& t : openAfterError) {
                if (t.jiraKey() == QStringLiteral("TEST#test/proj#2")) hasItem2 = true;
            }
            QVERIFY(hasItem2);
        }

        // Test 4: Task with an active timer running is protected from remote reconciliation
        {
            TxtTask activeT = TxtTask::fromSimpleTxt(QStringLiteral("Active Timer Task"), false, false);
            activeT.setJiraKey(QStringLiteral("TEST#test/proj#active"));
            QVERIFY(db.insertTask(ghList.id, activeT, 3) >= 0);

            svc.setActiveTaskChecker([](const QString& listId, const QString& jiraKey) {
                Q_UNUSED(listId)
                return jiraKey == QStringLiteral("TEST#test/proj#active");
            });

            // Reconcile with only item2 returned (item 'active' is omitted from remote response)
            svc.processSyncedItems(ghList, remoteItems, QStringLiteral("GitHub"), false);

            const auto openAfterActiveSync = db.loadTasks(ghList.id, false);
            bool activeStillOpen = false;
            for (const auto& t : openAfterActiveSync) {
                if (t.jiraKey() == QStringLiteral("TEST#test/proj#active")) activeStillOpen = true;
            }
            QVERIFY(activeStillOpen);
        }

        // Test 5: syncStatusToRemote persistence in DatabaseManager
        {
            ghList.syncStatusToRemote = true;
            QVERIFY(db.saveList(ghList, 0));
            const auto loadedLists = db.loadLists();
            bool found = false;
            for (const auto& l : loadedLists) {
                if (l.id == ghList.id) {
                    found = true;
                    QVERIFY(l.syncStatusToRemote);
                }
            }
            QVERIFY(found);
        }

        // Test 6: Legitimate empty response with explicitZeroTotal == true (last open task was closed remotely)
        {
            svc.setActiveTaskChecker(nullptr);
            const auto openBeforeReconcile = db.loadTasks(ghList.id, false);
            QVERIFY(!openBeforeReconcile.isEmpty());
            // Remote tracker authoritatively reports total == 0 with empty items list and no errors
            svc.processSyncedItems(ghList, {}, QStringLiteral("GitHub"), false, false /* hasError */, true /* explicitZeroTotal */);
            const auto openAfterReconcile = db.loadTasks(ghList.id, false);
            QCOMPARE(openAfterReconcile.size(), 0);
        }

        // Test 7: PR and MR keys are never updated on remote tracker
        {
            // updateIssueStatusRemote should do nothing for PRs/MRs
            svc.updateIssueStatusRemote(ghList, QStringLiteral("PR#owner/repo#42"), true, QStringLiteral("done"));
            svc.updateIssueStatusRemote(ghList, QStringLiteral("MR#owner/repo!99"), true, QStringLiteral("done"));
            svc.updateIssueStatusRemote(ghList, QStringLiteral("GL#owner/repo!99"), true, QStringLiteral("done"));
        }

        // Test 8: Redmine statuses parsing and ID resolution
        {
            const QByteArray redmineJson = R"({
                "issue_statuses": [
                    {"id": 1, "name": "New", "is_closed": false},
                    {"id": 3, "name": "Resolved", "is_closed": true},
                    {"id": 5, "name": "Closed", "is_closed": true}
                ]
            })";
            const auto redmineStatuses = IssueTrackerService::parseRedmineStatusesJson(redmineJson);
            QCOMPARE(redmineStatuses.size(), 3);
            QCOMPARE(redmineStatuses.at(0).toMap().value(QStringLiteral("remoteId")).toInt(), 1);
            QCOMPARE(redmineStatuses.at(2).toMap().value(QStringLiteral("remoteId")).toInt(), 5);
        }

        // Test 9: UkTranslator new translations and JSON loading
        {
            UkTranslator translator;
            translator.setLanguageCode(QStringLiteral("uk"));
            const QString trSync = translator.translate("TxtListEditDialog", "Sync task completion back to tracker");
            QCOMPARE(trSync, QStringLiteral("Синхронізувати завершення задач із трекером"));

            const QString trTuiSync = translator.translate("TuiApp", "8. Sync Status Back to Tracker  : %1");
            QCOMPARE(trTuiSync, QStringLiteral("8. Синхронізувати статус із трекером : %1"));

            const QString trTuiSave9 = translator.translate("TuiApp", "9. >>> [ SAVE & ACTIVATE LIST ] <<<");
            QCOMPARE(trTuiSave9, QStringLiteral("9. >>> [ ЗБЕРЕГТИ ТА АКТИВУВАТИ СПИСОК ] <<<"));

            const QString trUpdatedClosed = translator.translate("IssueTrackerService", "Updated %1 on %2: closed");
            QCOMPARE(trUpdatedClosed, QStringLiteral("Оновлено %1 на %2: закрито"));

            const QString trUpdatedOpen = translator.translate("IssueTrackerService", "Updated %1 on %2: open");
            QCOMPARE(trUpdatedOpen, QStringLiteral("Оновлено %1 на %2: відкрито"));

            const QString trInsecure = translator.translate("IssueTrackerService", " ⚠️ Insecure HTTP!");
            QCOMPARE(trInsecure, QStringLiteral(" ⚠️ Незахищений HTTP!"));

            // Test loading external translation JSON file with region suffix (e.g. de_AT.json)
            const QString tempJsonPath = m_testSandbox.filePath(QStringLiteral("custom_de_AT.json"));
            QFile f(tempJsonPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(R"({"Sync task completion back to tracker": "Synchronisiere Aufgabenabschluss zuruck zum Tracker"})");
            f.close();

            QVERIFY(translator.loadTranslationsFromJson(tempJsonPath, QStringLiteral("de_at")));
            // Test lookup with regional code
            translator.setLanguageCode(QStringLiteral("de_at"));
            QCOMPARE(translator.translate("TxtListEditDialog", "Sync task completion back to tracker"),
                     QStringLiteral("Synchronisiere Aufgabenabschluss zuruck zum Tracker"));

            // Test fallback from de_DE to base de
            translator.setLanguageCode(QStringLiteral("de_DE"));
            translator.loadTranslationsFromJson(tempJsonPath, QStringLiteral("de"));
            QCOMPARE(translator.translate("TxtListEditDialog", "Sync task completion back to tracker"),
                     QStringLiteral("Synchronisiere Aufgabenabschluss zuruck zum Tracker"));
            QFile::remove(tempJsonPath);

            // Verify embedded translations loaded via Q_INIT_RESOURCE
            QVERIFY(translator.hasLanguage(QStringLiteral("uk")));
            QVERIFY(translator.hasLanguage(QStringLiteral("de")));
            QVERIFY(translator.hasLanguage(QStringLiteral("fr")));
            QVERIFY(translator.hasLanguage(QStringLiteral("es")));
            QVERIFY(translator.hasLanguage(QStringLiteral("it")));

            // Test dynamic language addition via directory (e.g. ru.json)
            const QString customTransDir = m_testSandbox.filePath(QStringLiteral("dynamic_translations"));
            QDir().mkpath(customTransDir);
            const QString ruJsonPath = customTransDir + QStringLiteral("/ru.json");
            QFile ruFile(ruJsonPath);
            QVERIFY(ruFile.open(QIODevice::WriteOnly));
            ruFile.write(R"({"Settings": "Настройки", "Tasks": "Задачи"})");
            ruFile.close();

            // Also place an invalid filename (non-language code, e.g. backup.json)
            const QString backupJsonPath = customTransDir + QStringLiteral("/backup.json");
            QFile backupFile(backupJsonPath);
            QVERIFY(backupFile.open(QIODevice::WriteOnly));
            backupFile.write(R"({"Settings": "Резерв"})");
            backupFile.close();

            translator.loadTranslationsFromDirectory(customTransDir);
            QVERIFY(translator.hasLanguage(QStringLiteral("ru")));
            QVERIFY(translator.availableLanguageCodes().contains(QStringLiteral("ru")));
            QVERIFY(translator.languageDisplayName(QStringLiteral("ru")).contains(QStringLiteral("Русский"), Qt::CaseInsensitive));

            // backup.json must NOT be registered as a language!
            QVERIFY(!translator.hasLanguage(QStringLiteral("backup")));
            QVERIFY(!translator.availableLanguageCodes().contains(QStringLiteral("backup")));

            // Verify effective language resolution
            QCOMPARE(translator.resolveEffectiveLanguage(QStringLiteral("ru")), QStringLiteral("ru"));
            translator.setLanguageCode(QStringLiteral("ru"));
            QCOMPARE(translator.translate("SettingsDialog", "Settings"), QStringLiteral("Настройки"));
            QCOMPARE(translator.translate("TaskListPage", "Tasks"), QStringLiteral("Задачи"));

            // Verify SettingsManager integration with dynamic languages
            SettingsManager sm;
            sm.setSupportedLanguages(translator.availableLanguageCodes());
            sm.setLanguage(QStringLiteral("ru"));
            QCOMPARE(sm.effectiveLanguage(), QStringLiteral("ru"));

            QFile::remove(ruJsonPath);
            QFile::remove(backupJsonPath);
            QDir().rmdir(customTransDir);
        }

        // Test 10: Explicit Reproduction of User Case A & Case B from scratchpad
        {
            TodoListEntry e;
            e.id = QStringLiteral("case_ab");
            e.name = QStringLiteral("case_ab");
            e.provider = QStringLiteral("GitHub");
            e.apiToken = QStringLiteral("token");
            QVERIFY(db.saveList(e, 0));

            QVector<RemoteIssueItem> items150;
            for (int i = 0; i < 150; ++i) {
                RemoteIssueItem r;
                r.key = QStringLiteral("GH#o/r#%1").arg(i);
                r.description = QStringLiteral("issue %1").arg(i);
                r.remoteStatus = QStringLiteral("open");
                items150.append(r);
            }

            // Initial full sync with 150 open issues
            svc.processSyncedItems(e, items150, QStringLiteral("GitHub"), false, false);
            QCOMPARE(db.loadTasks(e.id, false).size(), 150);
            QCOMPARE(db.loadTasks(e.id, true).size(), 0);

            // Case A: Sync reaches page limit (pageLimitReached = true)
            // MUST NOT close the remaining 50 items!
            svc.processSyncedItems(e, items150.mid(0, 100), QStringLiteral("GitHub"), true /* pageLimitReached */, false);
            QCOMPARE(db.loadTasks(e.id, false).size(), 150);
            QCOMPARE(db.loadTasks(e.id, true).size(), 0);

            // Case B: 200 OK with empty/unparseable body (items is empty, explicitZeroTotal is false)
            // MUST NOT wipe out/close all open tasks!
            svc.processSyncedItems(e, {}, QStringLiteral("GitHub"), false, false);
            QCOMPARE(db.loadTasks(e.id, false).size(), 150);
            QCOMPARE(db.loadTasks(e.id, true).size(), 0);

            // Case C: Legitimate partial sync without limit reached (e.g. 5 open items returned, size < 100)
            // The unreturned items ARE reconciled and marked closed!
            svc.processSyncedItems(e, items150.mid(0, 5), QStringLiteral("GitHub"), false, false);
            QCOMPARE(db.loadTasks(e.id, false).size(), 5);
            QCOMPARE(db.loadTasks(e.id, true).size(), 145);

            // Case D: Sync with partial network failure (hasError == true, e.g. GitLab MR request 403)
            // MUST NOT close any of the remaining 5 tasks!
            svc.processSyncedItems(e, items150.mid(0, 2), QStringLiteral("GitLab"), false, true /* hasError */);
            QCOMPARE(db.loadTasks(e.id, false).size(), 5);
        }

        // Test 11: updateListStatusMapping preserves sort order
        {
            TodoListEntry e;
            e.id = QStringLiteral("status_mapping_test");
            e.name = QStringLiteral("Status Mapping Test");
            e.provider = QStringLiteral("Redmine");
            e.statusMapping = {{QStringLiteral("Resolved"), QStringLiteral("done")}};
            QVERIFY(db.saveList(e, 42));

            QVariantMap newMapping = e.statusMapping;
            newMapping[QStringLiteral("_id_Closed")] = 5;
            newMapping[QStringLiteral("_id_Resolved")] = 3;

            QVERIFY(db.updateListStatusMapping(e.id, newMapping));

            QVector<TodoListEntry> allLists = db.loadLists();
            bool found = false;
            for (const auto& l : allLists) {
                if (l.id == e.id) {
                    found = true;
                    QCOMPARE(l.statusMapping.value(QStringLiteral("_id_Closed")).toInt(), 5);
                    QCOMPARE(l.statusMapping.value(QStringLiteral("_id_Resolved")).toInt(), 3);
                    break;
                }
            }
            QVERIFY(found);

            QSqlQuery q(QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn")));
            QVERIFY(q.exec(QStringLiteral("SELECT sort_order FROM lists WHERE id = 'status_mapping_test'")));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 42); // sort_order MUST be preserved!
        }

        db.close();
        QFile::remove(testDb);
    }

    void testTaskTimerTrackingAndDatabaseSafety() {
        // 1. Timer distinguishes tasks with identical descriptions via taskId
        {
            SettingsManager sm;
            TaskTimer timer(&sm);
            timer.setActiveTask(101, QStringLiteral("Duplicate Description"), 0, 0);
            timer.start();
            QVERIFY(timer.isRunning());
            QCOMPARE(timer.activeTaskId(), 101);

            // Switching to another task with the same description must stop timer
            timer.setActiveTask(102, QStringLiteral("Duplicate Description"), 0, 0);
            QVERIFY(!timer.isRunning());
            QCOMPARE(timer.activeTaskId(), 102);

            // Setting the same task must keep state
            timer.start();
            QVERIFY(timer.isRunning());
            timer.setActiveTask(102, QStringLiteral("Duplicate Description"), timer.activeTaskTimerValue(), 0);
            QVERIFY(timer.isRunning());
            timer.stop();
        }

        // 2. Short sessions do not inflate time to whole seconds
        {
            SettingsManager sm;
            TaskTimer timer(&sm);
            timer.setActiveTask(201, QStringLiteral("Short Session Task"), 0, 0);
            // Run a few 50ms sessions
            for (int i = 0; i < 4; ++i) {
                timer.start();
                QThread::msleep(40);
                timer.stop();
            }
            // 4 * 40ms = ~160ms, which must be 0 seconds accumulated, NOT 4 seconds!
            QCOMPARE(timer.activeTaskTimerValue(), 0u);
        }

        // 3. Database initialization failure does not silently open default database
        {
            DatabaseManager& db = DatabaseManager::instance();
            db.close();
            const bool failed = db.initialize(QStringLiteral("/proc/gofi-review/blocked.db"));
            QVERIFY(!failed);
            QVERIFY(!db.isOpen());
            db.close();
        }

        // 4. Task reordering and row removal keeps active task locked by taskId
        {
            const QString testDb = m_testSandbox.filePath(QStringLiteral("reorder_safety.db"));
            QFile::remove(testDb);
            {
                AppController ctrl(QString(), testDb);
                while (ctrl.todoProxyModel()->rowCount() > 0) {
                    ctrl.removeTask(false, 0);
                }

                ctrl.addNewTask(QStringLiteral("Task 0"));
                ctrl.addNewTask(QStringLiteral("Task 1"));
                ctrl.addNewTask(QStringLiteral("Task 2"));

                ctrl.setActiveTodoRow(2); // Select Task 2
                QCOMPARE(ctrl.activeTodoRow(), 2);
                QCOMPARE(ctrl.timer()->activeTaskDescription(), QStringLiteral("Task 2"));

                // Move Task 0 to Row 1
                ctrl.moveTaskRow(false, 0, 1);
                // Active task was at index 2, still at index 2
                QCOMPARE(ctrl.activeTodoRow(), 2);
                QCOMPARE(ctrl.timer()->activeTaskDescription(), QStringLiteral("Task 2"));

                // Move Task 2 to Row 0
                ctrl.moveTaskRow(false, 2, 0);
                QCOMPARE(ctrl.activeTodoRow(), 0);
                QCOMPARE(ctrl.timer()->activeTaskDescription(), QStringLiteral("Task 2"));

                // Delete Task 1 (which is at index 1, after active task)
                ctrl.removeTask(false, 1);
                QCOMPARE(ctrl.activeTodoRow(), 0);
                QCOMPARE(ctrl.timer()->activeTaskDescription(), QStringLiteral("Task 2"));

                // Create a task at index 0 (prepend) or move row 1 before row 0
                ctrl.moveTaskRow(false, 1, 0);
                // Task 2 moved to row 1!
                QCOMPARE(ctrl.activeTodoRow(), 1);
                QCOMPARE(ctrl.timer()->activeTaskDescription(), QStringLiteral("Task 2"));
            }
            DatabaseManager::instance().close();
            QFile::remove(testDb);
        }

        // 5. Multi-list activity CSV routing
        {
            const QString testDb = m_testSandbox.filePath(QStringLiteral("multilist_csv.db"));
            const QString csvA = m_testSandbox.filePath(QStringLiteral("listA_activity.csv"));
            const QString csvB = m_testSandbox.filePath(QStringLiteral("listB_activity.csv"));
            QFile::remove(testDb);
            QFile::remove(csvA);
            QFile::remove(csvB);

            {
                AppController ctrl(QString(), testDb);
                const QString listA = ctrl.listManager()->addList(QStringLiteral("List A"));
                const QString listB = ctrl.listManager()->addList(QStringLiteral("List B"));

                QVariantMap cfgA = ctrl.listManager()->getListConfig(listA);
                cfgA[QStringLiteral("activityLogPath")] = csvA;
                ctrl.listManager()->saveListConfig(cfgA);

                QVariantMap cfgB = ctrl.listManager()->getListConfig(listB);
                cfgB[QStringLiteral("activityLogPath")] = csvB;
                ctrl.listManager()->saveListConfig(cfgB);

                // Add task in List B
                ctrl.selectListById(listB);
                ctrl.addNewTask(QStringLiteral("Task in List B"));
                ctrl.setActiveTodoRow(0);

                // Switch to All Lists view
                ctrl.showAllLists();
                for (int r = 0; r < ctrl.todoProxyModel()->rowCount(); ++r) {
                    if (ctrl.todoProxyModel()->data(ctrl.todoProxyModel()->index(r, 0), TaskStoreModel::DescriptionRole).toString().contains(QStringLiteral("Task in List B"))) {
                        ctrl.setActiveTodoRow(r);
                        break;
                    }
                }
                // Trigger timer stop
                ctrl.timer()->start();
                QThread::msleep(1050);
                ctrl.timer()->stop();

                // Check that activity was written to csvB, not csvA!
                QVERIFY(QFile::exists(csvB));
                QFile f(csvB);
                QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
                const QString content = QString::fromUtf8(f.readAll());
                f.close();
                QVERIFY(content.contains(QStringLiteral("Task in List B")));
                QVERIFY(!QFile::exists(csvA));
            }

            DatabaseManager::instance().close();
            QFile::remove(testDb);
            QFile::remove(csvA);
            QFile::remove(csvB);
        }
    }

    void testTaskTimerSwitchingAndDatabaseAtomicity() {
        // 1. isDatabaseReady check
        {
            AppController invalidCtrl(QString(), QStringLiteral("/proc/gofi-review-blocked/db.sqlite"));
            QVERIFY(!invalidCtrl.isDatabaseReady());
        }

        const QString testDb = m_testSandbox.filePath(QStringLiteral("gofi-timer-switch-test.db"));
        QFile::remove(testDb);

        {
            AppController ctrl(QString(), testDb);
            QVERIFY(ctrl.isDatabaseReady());

            const QString listId = ctrl.listManager()->addList(QStringLiteral("Timer Switch List"));
            ctrl.selectListById(listId);

            // Remove any default tasks created by addDefaultsIfEmpty
            while (ctrl.todoProxyModel()->rowCount() > 0) {
                ctrl.removeTask(false, 0);
            }

            ctrl.addNewTask(QStringLiteral("Task Alpha"));
            ctrl.addNewTask(QStringLiteral("Task Beta"));
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 2);

            // Select Task Alpha (row 0) and start timer
            ctrl.setActiveTodoRow(0);
            QCOMPARE(ctrl.activeTodoRow(), 0);
            ctrl.timer()->start();
            QVERIFY(ctrl.timer()->isRunning());
            QThread::msleep(1050);

            // Switch to Task Beta (row 1) while timer is running
            ctrl.setActiveTodoRow(1);
            QCOMPARE(ctrl.activeTodoRow(), 1);
            QVERIFY(!ctrl.timer()->isRunning());

            // Task Alpha (row 0) must have recorded >= 1 second
            const uint alphaSeconds = ctrl.todoProxyModel()->data(
                ctrl.todoProxyModel()->index(0, 0), TaskStoreModel::TimerValueRole
            ).toUInt();
            QVERIFY(alphaSeconds >= 1);

            // Task Beta (row 1) must still have 0 seconds (time of Task Alpha not leaked to Task Beta)
            const uint betaSeconds = ctrl.todoProxyModel()->data(
                ctrl.todoProxyModel()->index(1, 0), TaskStoreModel::TimerValueRole
            ).toUInt();
            QCOMPARE(betaSeconds, 0u);

            // 2. DatabaseManager batch ordering transaction atomicity with failure & rollback verification
            DatabaseManager& db = DatabaseManager::instance();
            ctrl.addNewTask(QStringLiteral("Task Gamma"));
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 3);

            QVector<TxtTask> initialTasks = db.loadTasks(listId, false);
            QCOMPARE(initialTasks.size(), 3);

            const QSqlDatabase testDbConn = QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn"));
            QSqlQuery initialQuery(testDbConn);
            initialQuery.exec(QStringLiteral("SELECT id, sort_order FROM tasks WHERE list_id = '") + listId + QStringLiteral("' ORDER BY id ASC"));
            QMap<qint64, int> originalOrders;
            while (initialQuery.next()) {
                originalOrders[initialQuery.value(0).toLongLong()] = initialQuery.value(1).toInt();
            }

            // Invert the tasks order
            QVector<TxtTask> reversedTasks = { initialTasks[2], initialTasks[1], initialTasks[0] };

            // Create a trigger that fails when sort_order = 1
            QSqlQuery triggerQuery(testDbConn);
            QVERIFY(triggerQuery.exec(QStringLiteral(
                "CREATE TRIGGER test_abort_sort_order BEFORE UPDATE OF sort_order ON tasks "
                "WHEN NEW.sort_order = 1 "
                "BEGIN SELECT RAISE(ABORT, 'Simulated batch failure on second item'); END;"
            )));

            // Attempt batch update - must fail and rollback!
            const bool batchResult = db.replaceListTasksOrders(listId, false, reversedTasks);
            QVERIFY(!batchResult);

            // Verify that sort_order for all tasks is STILL their original value (item 0 rolled back!)
            QSqlQuery checkQuery(testDbConn);
            checkQuery.exec(QStringLiteral("SELECT id, sort_order FROM tasks WHERE list_id = '") + listId + QStringLiteral("' ORDER BY id ASC"));
            while (checkQuery.next()) {
                const qint64 id = checkQuery.value(0).toLongLong();
                const int sortOrder = checkQuery.value(1).toInt();
                QCOMPARE(sortOrder, originalOrders[id]);
            }

            // Drop trigger and verify success
            triggerQuery.exec(QStringLiteral("DROP TRIGGER test_abort_sort_order"));
            QVERIFY(db.replaceListTasksOrders(listId, false, reversedTasks));

            // 3. Regression: Active task completed via toggleTaskDone while timer is running
            ctrl.addNewTask(QStringLiteral("Task Delta"));
            int deltaRow = -1;
            for (int r = 0; r < ctrl.todoProxyModel()->rowCount(); ++r) {
                if (ctrl.todoProxyModel()->data(ctrl.todoProxyModel()->index(r, 0), TaskStoreModel::DescriptionRole).toString() == QStringLiteral("Task Delta")) {
                    deltaRow = r;
                    break;
                }
            }
            QVERIFY(deltaRow >= 0);
            ctrl.setActiveTodoRow(deltaRow);
            ctrl.timer()->start();
            QVERIFY(ctrl.timer()->isRunning());
            QThread::msleep(1050);

            // Toggle done while timer running
            ctrl.toggleTaskDone(false, deltaRow);
            QVERIFY(!ctrl.timer()->isRunning());

            // Task Delta in doneModel must have >= 1 second
            bool deltaFoundInDone = false;
            for (int r = 0; r < ctrl.doneProxyModel()->rowCount(); ++r) {
                const QString d = ctrl.doneProxyModel()->data(ctrl.doneProxyModel()->index(r, 0), TaskStoreModel::DescriptionRole).toString();
                if (d == QStringLiteral("Task Delta")) {
                    const uint secs = ctrl.doneProxyModel()->data(ctrl.doneProxyModel()->index(r, 0), TaskStoreModel::TimerValueRole).toUInt();
                    QVERIFY(secs >= 1);
                    deltaFoundInDone = true;
                    break;
                }
            }
            QVERIFY(deltaFoundInDone);

            // Remaining active task in todoModel must NOT have received Delta's time
            if (ctrl.activeTodoRow() >= 0 && ctrl.activeTodoRow() < ctrl.todoProxyModel()->rowCount()) {
                const QString activeDesc = ctrl.todoProxyModel()->data(ctrl.todoProxyModel()->index(ctrl.activeTodoRow(), 0), TaskStoreModel::DescriptionRole).toString();
                if (activeDesc != QStringLiteral("Task Alpha")) {
                    const uint currentSecs = ctrl.todoProxyModel()->data(ctrl.todoProxyModel()->index(ctrl.activeTodoRow(), 0), TaskStoreModel::TimerValueRole).toUInt();
                    QCOMPARE(currentSecs, 0u);
                }
            }

            // 4. Regression: Active task removed via removeTask while timer is running
            ctrl.addNewTask(QStringLiteral("Task Epsilon"));
            ctrl.addNewTask(QStringLiteral("Task Zeta"));
            int epsRow = -1;
            for (int r = 0; r < ctrl.todoProxyModel()->rowCount(); ++r) {
                if (ctrl.todoProxyModel()->data(ctrl.todoProxyModel()->index(r, 0), TaskStoreModel::DescriptionRole).toString() == QStringLiteral("Task Epsilon")) {
                    epsRow = r;
                    break;
                }
            }
            QVERIFY(epsRow >= 0);
            ctrl.setActiveTodoRow(epsRow);
            ctrl.timer()->start();
            QVERIFY(ctrl.timer()->isRunning());
            QThread::msleep(1050);

            int deleteDatabaseErrorCount = 0;
            const QMetaObject::Connection deleteErrorConnection = QObject::connect(
                &ctrl, &AppController::databaseError, [&deleteDatabaseErrorCount](const QString&) {
                    ++deleteDatabaseErrorCount;
                });
            ctrl.removeTask(false, epsRow);
            QVERIFY(!ctrl.timer()->isRunning());
            QCOMPARE(deleteDatabaseErrorCount, 0);
            QObject::disconnect(deleteErrorConnection);

            // Verify Task Zeta has 0 seconds
            for (int r = 0; r < ctrl.todoProxyModel()->rowCount(); ++r) {
                if (ctrl.todoProxyModel()->data(ctrl.todoProxyModel()->index(r, 0), TaskStoreModel::DescriptionRole).toString() == QStringLiteral("Task Zeta")) {
                    const uint zSecs = ctrl.todoProxyModel()->data(ctrl.todoProxyModel()->index(r, 0), TaskStoreModel::TimerValueRole).toUInt();
                    QCOMPARE(zSecs, 0u);
                    break;
                }
            }

            // 5. Regression: Deleting the very last task while timer is running
            while (ctrl.todoProxyModel()->rowCount() > 1) {
                ctrl.removeTask(false, 0);
            }
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 1);
            ctrl.setActiveTodoRow(0);
            ctrl.timer()->start();
            QVERIFY(ctrl.timer()->isRunning());
            QThread::msleep(1050);

            ctrl.removeTask(false, 0);
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 0);
            QCOMPARE(ctrl.activeTodoRow(), -1);
            QVERIFY(!ctrl.timer()->isRunning());
        }

        DatabaseManager::instance().close();
        QFile::remove(testDb);
    }

    void testPaginationReconciliationForLargeLists() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("gofi-reconcile-test.db"));
        QFile::remove(testDb);

        {
            AppController ctrl(QString(), testDb);
            const QString listId = ctrl.listManager()->addList(QStringLiteral("Reconcile Tracker"));
            QVariantMap cfg = ctrl.listManager()->getListConfig(listId);
            cfg[QStringLiteral("provider")] = QStringLiteral("Jira");
            cfg[QStringLiteral("jiraUrl")] = QStringLiteral("https://jira.example.com");
            ctrl.listManager()->saveListConfig(cfg);

            TodoListEntry entry = ctrl.listManager()->entry(ctrl.listManager()->indexOfId(listId));

            DatabaseManager& db = DatabaseManager::instance();
            QVector<RemoteIssueItem> items;
            for (int i = 1; i <= 105; ++i) {
                RemoteIssueItem it;
                it.key = QStringLiteral("PROJ-%1").arg(i);
                it.description = QStringLiteral("Issue %1").arg(i);
                it.remoteStatus = QStringLiteral("Open");
                items.append(it);
            }

            // First sync with 105 items: pageLimitReached = false
            ctrl.issueTracker()->processSyncedItems(entry, items, QStringLiteral("Jira"), false, false, false);
            QCOMPARE(db.countAllTasksInList(listId), 105);

            // PROJ-105 is closed remotely -> remote items now has 104 issues
            items.removeLast();
            QCOMPARE(items.size(), 104);

            // Second sync with 104 items: pageLimitReached = false
            // Must reconcile and close PROJ-105 locally!
            ctrl.issueTracker()->processSyncedItems(entry, items, QStringLiteral("Jira"), false, false, false);

            const QVector<TxtTask> openTasks = db.loadTasks(listId, false);
            QCOMPARE(openTasks.size(), 104);

            const QVector<TxtTask> doneTasks = db.loadTasks(listId, true);
            QCOMPARE(doneTasks.size(), 1);
            QCOMPARE(doneTasks.first().jiraKey(), QStringLiteral("PROJ-105"));
            QVERIFY(doneTasks.first().isDone());
        }

        DatabaseManager::instance().close();
        QFile::remove(testDb);
    }

    void testDatabaseErrorHandlingAndNonFinalStatusTimer() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("gofi-dberr-test.db"));
        QFile::remove(testDb);

        {
            AppController ctrl(QString(), testDb);
            const QSqlDatabase dbConn = QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn"));

            // 1. Non-final status change does NOT stop timer
            const QString listId = ctrl.listManager()->addList(QStringLiteral("Status Timer List"));
            ctrl.selectListById(listId);
            while (ctrl.todoProxyModel()->rowCount() > 0) {
                ctrl.removeTask(false, 0);
            }
            ctrl.addNewTask(QStringLiteral("Work in progress task"));
            ctrl.setActiveTodoRow(0);
            ctrl.timer()->start();
            QVERIFY(ctrl.timer()->isRunning());

            // Change to 'in_progress' - timer must CONTINUE running
            ctrl.changeTaskStatus(false, 0, QStringLiteral("in_progress"));
            QVERIFY(ctrl.timer()->isRunning());

            // Setting same status again - timer must CONTINUE running
            ctrl.changeTaskStatus(false, 0, QStringLiteral("in_progress"));
            QVERIFY(ctrl.timer()->isRunning());

            // Only completing to 'done' stops the timer
            ctrl.changeTaskStatus(false, 0, QStringLiteral("done"));
            QVERIFY(!ctrl.timer()->isRunning());

            // 2. addList database failure handling
            QSqlQuery trig(dbConn);
            QVERIFY(trig.exec(QStringLiteral(
                "CREATE TRIGGER test_fail_add_list BEFORE INSERT ON lists "
                "BEGIN SELECT RAISE(ABORT, 'Simulated addList error'); END;"
            )));

            bool errReceived = false;
            QObject::connect(&ctrl, &AppController::databaseError, [&errReceived](const QString&) {
                errReceived = true;
            });

            const QString failedId = ctrl.listManager()->addList(QStringLiteral("Will Fail"));
            QVERIFY(failedId.isEmpty());
            QVERIFY(errReceived);
            trig.exec(QStringLiteral("DROP TRIGGER test_fail_add_list"));

            // 3. clearDoneList database failure handling
            QVERIFY(ctrl.doneProxyModel()->rowCount() > 0);
            const int doneCountBefore = ctrl.doneProxyModel()->rowCount();
            QVERIFY(trig.exec(QStringLiteral(
                "CREATE TRIGGER test_fail_clear_done BEFORE DELETE ON tasks "
                "BEGIN SELECT RAISE(ABORT, 'Simulated clearDone error'); END;"
            )));

            errReceived = false;
            ctrl.clearDoneTasks();
            QVERIFY(errReceived);
            // Done tasks must NOT be cleared from UI when database delete failed
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), doneCountBefore);
            trig.exec(QStringLiteral("DROP TRIGGER test_fail_clear_done"));

            // Now clean clear succeeds
            ctrl.clearDoneTasks();
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 0);

            // 4. Remote sync write error stops reconciliation and reports syncFailed
            TodoListEntry entry = ctrl.listManager()->entry(ctrl.listManager()->indexOfId(listId));
            QVector<RemoteIssueItem> items;
            RemoteIssueItem it;
            it.key = QStringLiteral("ERR-1");
            it.description = QStringLiteral("Sync error task");
            items.append(it);

            QVERIFY(trig.exec(QStringLiteral(
                "CREATE TRIGGER test_fail_upsert BEFORE INSERT ON tasks "
                "BEGIN SELECT RAISE(ABORT, 'Simulated upsert error'); END;"
            )));

            bool syncFailedCalled = false;
            QObject::connect(ctrl.issueTracker(), &IssueTrackerService::syncFailed, [&syncFailedCalled](const QString&, const QString&) {
                syncFailedCalled = true;
            });

            ctrl.issueTracker()->processSyncedItems(entry, items, QStringLiteral("Jira"), false, false, false);
            QVERIFY(syncFailedCalled);
            trig.exec(QStringLiteral("DROP TRIGGER test_fail_upsert"));
        }

        DatabaseManager::instance().close();
        QFile::remove(testDb);
    }

    void testAllListsClearDoneAndStatusSideEffectsSafety() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("gofi-alllists-safety-test.db"));
        QFile::remove(testDb);

        {
            AppController ctrl(QString(), testDb);
            const QSqlDatabase dbConn = QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn"));

            // 1. "Clear Done" in All Lists (__ALL__) mode deletes done tasks across all lists
            const QString list1Id = ctrl.listManager()->addList(QStringLiteral("List 1"));
            const QString list2Id = ctrl.listManager()->addList(QStringLiteral("List 2"));
            QVERIFY(!list1Id.isEmpty() && !list2Id.isEmpty());

            ctrl.selectListById(list1Id);
            while (ctrl.todoProxyModel()->rowCount() > 0) {
                ctrl.removeTask(false, 0);
            }
            while (ctrl.doneProxyModel()->rowCount() > 0) {
                ctrl.removeTask(true, 0);
            }
            ctrl.addNewTask(QStringLiteral("Task 1 in List 1"));
            ctrl.toggleTaskDone(false, 0);

            ctrl.selectListById(list2Id);
            while (ctrl.todoProxyModel()->rowCount() > 0) {
                ctrl.removeTask(false, 0);
            }
            while (ctrl.doneProxyModel()->rowCount() > 0) {
                ctrl.removeTask(true, 0);
            }
            ctrl.addNewTask(QStringLiteral("Task 2 in List 2"));
            ctrl.toggleTaskDone(false, 0);

            // Directly verify SQLite has 2 completed tasks
            QSqlQuery countQ(QStringLiteral("SELECT COUNT(*) FROM tasks WHERE done = 1"), dbConn);
            QVERIFY(countQ.next());
            QCOMPARE(countQ.value(0).toInt(), 2);

            // Switch to All Lists mode
            ctrl.showAllLists();
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 2);

            // Clear done tasks in All Lists mode
            ctrl.clearDoneTasks();
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 0);

            // Verify SQLite has 0 completed tasks
            countQ.exec(QStringLiteral("SELECT COUNT(*) FROM tasks WHERE done = 1"));
            QVERIFY(countQ.next());
            QCOMPARE(countQ.value(0).toInt(), 0);

            // Re-check individual lists to ensure done tasks are gone permanently
            ctrl.selectListById(list1Id);
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 0);
            ctrl.selectListById(list2Id);
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 0);

            // 2. Atomic deletion across multiple lists with rollback on failure
            ctrl.addNewTask(QStringLiteral("Done task A"));
            ctrl.toggleTaskDone(false, 0);
            ctrl.selectListById(list1Id);
            ctrl.addNewTask(QStringLiteral("Done task B"));
            ctrl.toggleTaskDone(false, 0);

            countQ.exec(QStringLiteral("SELECT COUNT(*) FROM tasks WHERE done = 1"));
            QVERIFY(countQ.next());
            QCOMPARE(countQ.value(0).toInt(), 2);

            QSqlQuery trig(dbConn);
            QVERIFY(trig.exec(QStringLiteral(
                "CREATE TRIGGER test_fail_multi_delete BEFORE DELETE ON tasks "
                "BEGIN SELECT RAISE(ABORT, 'Simulated delete error'); END;"
            )));

            // deleteDoneTasks on list IDs should fail and rollback
            const bool delOk = DatabaseManager::instance().deleteDoneTasks(QStringList{list1Id, list2Id});
            QVERIFY(!delOk);

            // Both tasks must still exist in DB due to atomic rollback
            countQ.exec(QStringLiteral("SELECT COUNT(*) FROM tasks WHERE done = 1"));
            QVERIFY(countQ.next());
            QCOMPARE(countQ.value(0).toInt(), 2);
            trig.exec(QStringLiteral("DROP TRIGGER test_fail_multi_delete"));

            ctrl.showAllLists();
            ctrl.clearDoneTasks();
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 0);

            // 3. Status side-effects blocked on SQLite persistence failure
            ctrl.selectListById(list1Id);
            while (ctrl.todoProxyModel()->rowCount() > 0) {
                ctrl.removeTask(false, 0);
            }
            while (ctrl.doneProxyModel()->rowCount() > 0) {
                ctrl.removeTask(true, 0);
            }
            ctrl.addNewTask(QStringLiteral("Side effect test task"));
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 1);

            QVERIFY(trig.exec(QStringLiteral(
                "CREATE TRIGGER test_fail_status_update BEFORE UPDATE ON tasks "
                "BEGIN SELECT RAISE(ABORT, 'Simulated update error'); END;"
            )));

            bool statusChangedNotifReceived = false;
            QObject::connect(&ctrl, &AppController::notificationRequested, [&statusChangedNotifReceived](const QString& title, const QString&) {
                if (title == QObject::tr("Task status changed")) {
                    statusChangedNotifReceived = true;
                }
            });

            // Attempt toggleTaskDone: must fail, suppress "Task status changed" notification, and keep task in todo
            ctrl.toggleTaskDone(false, 0);
            QVERIFY(!statusChangedNotifReceived);
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 1);
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 0);

            // Attempt changeTaskStatus: must fail, suppress "Task status changed" notification, and keep task in todo
            ctrl.changeTaskStatus(false, 0, QStringLiteral("done"));
            QVERIFY(!statusChangedNotifReceived);
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 1);
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 0);
            trig.exec(QStringLiteral("DROP TRIGGER test_fail_status_update"));

            // 4. Status 'canceled' is treated as closed and handled consistently
            ctrl.setActiveTodoRow(0);
            ctrl.timer()->start();
            QVERIFY(ctrl.timer()->isRunning());

            ctrl.changeTaskStatus(false, 0, QStringLiteral("canceled"));
            // Timer must be stopped when transitioning to 'canceled'
            QVERIFY(!ctrl.timer()->isRunning());
            // Task moves from todo to done
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 0);
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 1);

            // Check SQLite state for 'canceled'
            QSqlQuery checkStatus(dbConn);
            checkStatus.prepare(QStringLiteral("SELECT done, status FROM tasks WHERE list_id = :lid"));
            checkStatus.bindValue(QStringLiteral(":lid"), list1Id);
            QVERIFY(checkStatus.exec() && checkStatus.next());
            QCOMPARE(checkStatus.value(0).toInt(), 1);
            QCOMPARE(checkStatus.value(1).toString(), QStringLiteral("canceled"));

            // Reopen from 'canceled' to 'todo'
            ctrl.changeTaskStatus(true, 0, QStringLiteral("todo"));
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 1);
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 0);

            checkStatus.bindValue(QStringLiteral(":lid"), list1Id);
            QVERIFY(checkStatus.exec() && checkStatus.next());
            QCOMPARE(checkStatus.value(0).toInt(), 0);
            QCOMPARE(checkStatus.value(1).toString(), QStringLiteral("todo"));

            // 5. Remote sync transaction atomic rollback on SQLite error
            TodoListEntry entry = ctrl.listManager()->entry(ctrl.listManager()->indexOfId(list1Id));
            QVector<RemoteIssueItem> syncItems;
            RemoteIssueItem item1;
            item1.key = QStringLiteral("TX-1");
            item1.description = QStringLiteral("First item");
            syncItems.append(item1);

            RemoteIssueItem item2;
            item2.key = QStringLiteral("TX-FAIL");
            item2.description = QStringLiteral("Second item fails");
            syncItems.append(item2);

            QVERIFY(trig.exec(QStringLiteral(
                "CREATE TRIGGER test_fail_tx_item BEFORE INSERT ON tasks "
                "WHEN NEW.jira_key = 'TX-FAIL' "
                "BEGIN SELECT RAISE(ABORT, 'Simulated second item failure'); END;"
            )));

            bool syncFailedCalled = false;
            QObject::connect(ctrl.issueTracker(), &IssueTrackerService::syncFailed, [&syncFailedCalled](const QString&, const QString&) {
                syncFailedCalled = true;
            });

            ctrl.issueTracker()->processSyncedItems(entry, syncItems, QStringLiteral("Jira"), false, false, false);
            QVERIFY(syncFailedCalled);
            trig.exec(QStringLiteral("DROP TRIGGER test_fail_tx_item"));

            // Because transaction rolled back, item1 (TX-1) must NOT be present in database
            QSqlQuery txCheck(QStringLiteral("SELECT COUNT(*) FROM tasks WHERE jira_key = 'TX-1'"), dbConn);
            QVERIFY(txCheck.next());
            QCOMPARE(txCheck.value(0).toInt(), 0);
        }

        DatabaseManager::instance().close();
        QFile::remove(testDb);
    }

    void testStatusNormalizationAndAtomicReorderAndSyncTx() {
        // 1. TxtTask::isClosedStatus and normalizeStatus handles unnormalized cases
        QVERIFY(TxtTask::isClosedStatus(QStringLiteral("DONE")));
        QVERIFY(TxtTask::isClosedStatus(QStringLiteral("Canceled")));
        QVERIFY(TxtTask::isClosedStatus(QStringLiteral("  done  ")));
        QVERIFY(TxtTask::isClosedStatus(QStringLiteral("  CANCELED  ")));
        QVERIFY(!TxtTask::isClosedStatus(QStringLiteral("TODO")));
        QVERIFY(!TxtTask::isClosedStatus(QStringLiteral("in_progress")));
        QCOMPARE(TxtTask::normalizeStatus(QStringLiteral("  DONE  ")), QStringLiteral("done"));
        QCOMPARE(TxtTask::normalizeStatus(QStringLiteral(" Canceled ")), QStringLiteral("canceled"));
        QCOMPARE(TxtTask::normalizeStatus(QStringLiteral("   ")), QStringLiteral("todo"));

        const QString testDb = m_testSandbox.filePath(QStringLiteral("gofi-reorder-atomic-test.db"));
        QFile::remove(testDb);

        {
            AppController ctrl(QString(), testDb);
            const QSqlDatabase dbConn = QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn"));

            const QString listId = ctrl.listManager()->addList(QStringLiteral("Atomic List"));
            ctrl.selectListById(listId);
            while (ctrl.todoProxyModel()->rowCount() > 0) {
                ctrl.removeTask(false, 0);
            }
            while (ctrl.doneProxyModel()->rowCount() > 0) {
                ctrl.removeTask(true, 0);
            }

            ctrl.addNewTask(QStringLiteral("Task 1"));
            ctrl.addNewTask(QStringLiteral("Task 2"));
            ctrl.setActiveTodoRow(0);
            ctrl.timer()->start();
            QVERIFY(ctrl.timer()->isRunning());

            // 2. Change status with unnormalized string " DONE "
            ctrl.changeTaskStatus(false, 0, QStringLiteral("  DONE  "));
            QVERIFY(!ctrl.timer()->isRunning());
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 1);
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 1);
            QCOMPARE(ctrl.doneProxyModel()->data(ctrl.doneProxyModel()->index(0, 0), TaskStoreModel::StatusRole).toString(), QStringLiteral("done"));

            // 3. Atomic move and reorder rollback when reorder update fails
            ctrl.addNewTask(QStringLiteral("Task 3"));
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 2);

            QSqlQuery trig(dbConn);
            // Trigger failure on updating sort_order of remaining tasks
            QVERIFY(trig.exec(QStringLiteral(
                "CREATE TRIGGER test_fail_reorder BEFORE UPDATE OF sort_order ON tasks "
                "WHEN OLD.done = 0 AND NEW.done = 0 "
                "BEGIN SELECT RAISE(ABORT, 'Simulated reorder error'); END;"
            )));

            // Attempt toggleTaskDone: reordering remaining tasks fails, so entire transaction must rollback
            ctrl.toggleTaskDone(false, 0);
            // Task must NOT have moved because transaction rolled back
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 2);
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 1);

            trig.exec(QStringLiteral("DROP TRIGGER test_fail_reorder"));

            // 4. Remote sync aborts immediately if transaction cannot start
            TodoListEntry entry = ctrl.listManager()->entry(ctrl.listManager()->indexOfId(listId));
            QVector<RemoteIssueItem> syncItems;
            RemoteIssueItem it;
            it.key = QStringLiteral("NO-TX-1");
            it.description = QStringLiteral("No tx task");
            syncItems.append(it);

            // Intentionally hold open a transaction so beginTransaction inside processSyncedItems fails
            QVERIFY(DatabaseManager::instance().beginTransaction());
            bool syncFailedCalled = false;
            QObject::connect(ctrl.issueTracker(), &IssueTrackerService::syncFailed, [&syncFailedCalled](const QString&, const QString&) {
                syncFailedCalled = true;
            });

            ctrl.issueTracker()->processSyncedItems(entry, syncItems, QStringLiteral("Jira"), false, false, false);
            QVERIFY(syncFailedCalled);
            DatabaseManager::instance().rollbackTransaction();

            // 5. Default tasks insertion fails cleanly if transaction fails
            const QString emptyListId = ctrl.listManager()->addList(QStringLiteral("Empty List"));
            QVERIFY(trig.exec(QStringLiteral(
                "CREATE TRIGGER test_fail_default_insert BEFORE INSERT ON tasks "
                "BEGIN SELECT RAISE(ABORT, 'Simulated default insert error'); END;"
            )));

            bool dbErr = false;
            QObject::connect(&ctrl, &AppController::databaseError, [&dbErr](const QString&) {
                dbErr = true;
            });
            ctrl.selectListById(emptyListId);
            QVERIFY(dbErr);
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 0);
            trig.exec(QStringLiteral("DROP TRIGGER test_fail_default_insert"));
        }

        DatabaseManager::instance().close();
        QFile::remove(testDb);
    }

    void testSyncCompletionDateTimerRestorationAndMultiListOrder() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("gofi-sync-timer-order-test.db"));
        QFile::remove(testDb);

        {
            AppController ctrl(QString(), testDb);
            const QSqlDatabase dbConn = QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn"));

            // 1. Remote-sync completion_date for existing tasks
            const QString list1Id = ctrl.listManager()->addList(QStringLiteral("List 1"));
            const QString list2Id = ctrl.listManager()->addList(QStringLiteral("List 2"));

            TodoListEntry entry1 = ctrl.listManager()->entry(ctrl.listManager()->indexOfId(list1Id));
            QVector<RemoteIssueItem> syncItems;
            RemoteIssueItem it;
            it.key = QStringLiteral("REM-1");
            it.description = QStringLiteral("Remote task 1");
            it.remoteStatus = QStringLiteral("todo");
            syncItems.append(it);

            ctrl.issueTracker()->processSyncedItems(entry1, syncItems, QStringLiteral("Jira"), false, false, false);
            QSqlQuery checkDate(QStringLiteral("SELECT done, completion_date FROM tasks WHERE jira_key = 'REM-1'"), dbConn);
            QVERIFY(checkDate.next());
            QCOMPARE(checkDate.value(0).toInt(), 0);
            QVERIFY(checkDate.value(1).toString().isEmpty());

            // Transition existing task to 'done' remotely
            syncItems[0].remoteStatus = QStringLiteral("done");
            ctrl.issueTracker()->processSyncedItems(entry1, syncItems, QStringLiteral("Jira"), false, false, false);
            checkDate.exec(QStringLiteral("SELECT done, completion_date FROM tasks WHERE jira_key = 'REM-1'"));
            QVERIFY(checkDate.next());
            QCOMPARE(checkDate.value(0).toInt(), 1);
            QCOMPARE(checkDate.value(1).toString(), QDate::currentDate().toString(Qt::ISODate));

            // Reopen existing task to 'todo' remotely
            syncItems[0].remoteStatus = QStringLiteral("todo");
            ctrl.issueTracker()->processSyncedItems(entry1, syncItems, QStringLiteral("Jira"), false, false, false);
            checkDate.exec(QStringLiteral("SELECT done, completion_date FROM tasks WHERE jira_key = 'REM-1'"));
            QVERIFY(checkDate.next());
            QCOMPARE(checkDate.value(0).toInt(), 0);
            QVERIFY(checkDate.value(1).toString().isEmpty());

            // 2. Failed Done toggle restores running timer
            ctrl.selectListById(list1Id);
            while (ctrl.todoProxyModel()->rowCount() > 0) {
                ctrl.removeTask(false, 0);
            }
            while (ctrl.doneProxyModel()->rowCount() > 0) {
                ctrl.removeTask(true, 0);
            }
            ctrl.addNewTask(QStringLiteral("Timer test task"));
            ctrl.setActiveTodoRow(0);
            ctrl.timer()->start();
            QVERIFY(ctrl.timer()->isRunning());

            QSqlQuery trig(dbConn);
            QVERIFY(trig.exec(QStringLiteral(
                "CREATE TRIGGER test_fail_timer_toggle BEFORE UPDATE ON tasks "
                "BEGIN SELECT RAISE(ABORT, 'Simulated update failure'); END;"
            )));

            // Attempt toggle: fails, so timer must be restored to running
            ctrl.toggleTaskDone(false, 0);
            QVERIFY(ctrl.timer()->isRunning());

            // Attempt status change to done: fails, so timer must be restored to running
            ctrl.changeTaskStatus(false, 0, QStringLiteral("done"));
            QVERIFY(ctrl.timer()->isRunning());
            trig.exec(QStringLiteral("DROP TRIGGER test_fail_timer_toggle"));

            // 3. Multi-list sort_order isolation for new done tasks
            QSqlQuery cleanAll(dbConn);
            cleanAll.exec(QStringLiteral("DELETE FROM tasks"));

            // List 1 gets 3 done tasks
            ctrl.selectListById(list1Id);
            while (ctrl.todoProxyModel()->rowCount() > 0) {
                ctrl.removeTask(false, 0);
            }
            while (ctrl.doneProxyModel()->rowCount() > 0) {
                ctrl.removeTask(true, 0);
            }
            ctrl.addNewTask(QStringLiteral("L1 Done 1"));
            ctrl.toggleTaskDone(false, 0);
            ctrl.addNewTask(QStringLiteral("L1 Done 2"));
            ctrl.toggleTaskDone(false, 0);
            ctrl.addNewTask(QStringLiteral("L1 Done 3"));
            ctrl.toggleTaskDone(false, 0);

            // List 2 has 0 done tasks, 1 todo task
            ctrl.selectListById(list2Id);
            while (ctrl.todoProxyModel()->rowCount() > 0) {
                ctrl.removeTask(false, 0);
            }
            while (ctrl.doneProxyModel()->rowCount() > 0) {
                ctrl.removeTask(true, 0);
            }
            ctrl.addNewTask(QStringLiteral("L2 Task 1"));

            // Switch to All Lists view (shows 3 done tasks from L1, 1 todo from L2)
            ctrl.showAllLists();
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 3);
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), 1);

            // Toggle L2 Task 1 to done from All Lists view
            ctrl.toggleTaskDone(false, 0);

            // In SQLite, L2 Task 1's sort_order must be 0 (isolated to List 2), NOT 3!
            QSqlQuery orderCheck(dbConn);
            orderCheck.prepare(QStringLiteral("SELECT sort_order FROM tasks WHERE list_id = :lid AND description LIKE '%L2 Task 1%'"));
            orderCheck.bindValue(QStringLiteral(":lid"), list2Id);
            QVERIFY(orderCheck.exec() && orderCheck.next());
            QCOMPARE(orderCheck.value(0).toInt(), 0);

            // 4. numRowsAffected check on non-existent task ID
            TxtTask nonExistentTask = TxtTask::fromSimpleTxt(QStringLiteral("Ghost"), false, false);
            nonExistentTask.setId(999999);
            QVERIFY(!DatabaseManager::instance().updateTask(nonExistentTask));
            QVERIFY(!DatabaseManager::instance().deleteTask(999999));
            QVERIFY(!DatabaseManager::instance().markTaskViewed(999999));
        }

        DatabaseManager::instance().close();
        QFile::remove(testDb);
    }

    void testAtomicDoneAndWorklogSyncAndOrderCompaction() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("gofi_atomic_done_test.db"));
        QFile::remove(testDb);

        {
            const QString logPath = m_testSandbox.filePath(QStringLiteral("gofi_atomic_ctrl.log"));
            QFile::remove(logPath);
            AppController ctrl(logPath, testDb);
            const QSqlDatabase dbConn = QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn"));
            const QString listId = ctrl.listManager()->addList(QStringLiteral("Atomic Done Test"));
            ctrl.selectListById(listId);
            while (ctrl.todoProxyModel()->rowCount() > 0) {
                ctrl.removeTask(false, 0);
            }

            // 1. Direct TaskStoreModel::setData(DoneRole) atomic move & rollback
            {
                ctrl.addNewTask(QStringLiteral("Task 1"));
                ctrl.addNewTask(QStringLiteral("Task 2"));
                ctrl.addNewTask(QStringLiteral("Task 3"));
                QCOMPARE(ctrl.todoProxyModel()->rowCount(), 3);
                QCOMPARE(ctrl.doneProxyModel()->rowCount(), 0);

                // Toggle Task 2 to done directly via TaskStoreModel::setData (simulates QML toggle)
                auto* todoStore = qobject_cast<TaskStoreModel*>(ctrl.todoProxyModel()->sourceModel());
                auto* doneStore = qobject_cast<TaskStoreModel*>(ctrl.doneProxyModel()->sourceModel());
                QVERIFY(todoStore && doneStore);
                QModelIndex idx = todoStore->index(1, 0);
                QVERIFY(todoStore->setData(idx, true, TaskStoreModel::DoneRole));
                QCOMPARE(ctrl.todoProxyModel()->rowCount(), 2);
                QCOMPARE(ctrl.doneProxyModel()->rowCount(), 1);
                QCOMPARE(doneStore->taskAt(0).description(), QStringLiteral("Task 2"));

                // Check SQLite orders: Todo tasks 1 and 3 should have orders 0 and 1
                auto todoTasks = DatabaseManager::instance().loadTasks(listId, false);
                QCOMPARE(todoTasks.size(), 2);
                QCOMPARE(todoTasks[0].description(), QStringLiteral("Task 1"));
                QCOMPARE(todoTasks[1].description(), QStringLiteral("Task 3"));
                auto doneTasks = DatabaseManager::instance().loadTasks(listId, true);
                QCOMPARE(doneTasks.size(), 1);
                QCOMPARE(doneTasks[0].description(), QStringLiteral("Task 2"));

                // Toggle back to todo via setData on doneModel
                QModelIndex doneIdx = doneStore->index(0, 0);
                QVERIFY(doneStore->setData(doneIdx, false, TaskStoreModel::DoneRole));
                QCOMPARE(ctrl.todoProxyModel()->rowCount(), 3);
                QCOMPARE(ctrl.doneProxyModel()->rowCount(), 0);

                // Failure path with trigger on UPDATE tasks
                QSqlQuery triggerQ(dbConn);
                triggerQ.exec(QStringLiteral(
                    "CREATE TRIGGER test_fail_done BEFORE UPDATE OF done ON tasks "
                    "WHEN NEW.description LIKE '%Task 1%' "
                    "BEGIN "
                    "  SELECT RAISE(ABORT, 'Simulated DB failure on done'); "
                    "END;"
                ));

                bool errorEmitted = false;
                QObject::connect(&ctrl, &AppController::databaseError, [&errorEmitted](const QString&) {
                    errorEmitted = true;
                });

                // Trying to mark Task 1 done must trigger DB failure, loadFromDatabase rollback, and return false
                QModelIndex idx1 = todoStore->index(0, 0);
                QVERIFY(!todoStore->setData(idx1, true, TaskStoreModel::DoneRole));
                QVERIFY(errorEmitted);
                QCOMPARE(ctrl.todoProxyModel()->rowCount(), 3);
                QCOMPARE(ctrl.doneProxyModel()->rowCount(), 0);

                triggerQ.exec(QStringLiteral("DROP TRIGGER test_fail_done"));

                // Test appendTaskComment & markTaskViewed failure handling and persistence
                QSqlQuery failCommentTrigger(dbConn);
                failCommentTrigger.exec(QStringLiteral(
                    "CREATE TRIGGER test_fail_comment BEFORE UPDATE OF comments_json ON tasks "
                    "BEGIN "
                    "  SELECT RAISE(ABORT, 'Simulated comment failure'); "
                    "END;"
                ));
                QVERIFY(!todoStore->appendTaskComment(0, QStringLiteral("Alice"), QStringLiteral("Test comment")));
                QCOMPARE(todoStore->taskAt(0).comments().size(), 0);
                failCommentTrigger.exec(QStringLiteral("DROP TRIGGER test_fail_comment"));

                // Successful comment
                QVERIFY(todoStore->appendTaskComment(0, QStringLiteral("Alice"), QStringLiteral("Test comment")));
                QCOMPARE(todoStore->taskAt(0).comments().size(), 1);

                QSqlQuery resetView(dbConn);
                QVERIFY(resetView.exec(QStringLiteral("UPDATE tasks SET is_viewed = 0")));
                ctrl.selectListById(listId);
                QCOMPARE(todoStore->taskAt(1).isViewed(), false);

                QSqlQuery failViewedTrigger(dbConn);
                failViewedTrigger.exec(QStringLiteral(
                    "CREATE TRIGGER test_fail_viewed BEFORE UPDATE OF is_viewed ON tasks "
                    "BEGIN "
                    "  SELECT RAISE(ABORT, 'Simulated view failure'); "
                    "END;"
                ));
                QVERIFY(!todoStore->markTaskViewed(1));
                failViewedTrigger.exec(QStringLiteral("DROP TRIGGER test_fail_viewed"));
                QVERIFY(todoStore->markTaskViewed(1));
                QCOMPARE(todoStore->taskAt(1).isViewed(), true);

                // Controller level updateTaskDetails, addTaskComment, and updateTaskText error return
                QSqlQuery failDetailsTrigger(dbConn);
                failDetailsTrigger.exec(QStringLiteral(
                    "CREATE TRIGGER test_fail_details BEFORE UPDATE OF details ON tasks "
                    "BEGIN "
                    "  SELECT RAISE(ABORT, 'Simulated details failure'); "
                    "END;"
                ));
                QVERIFY(!ctrl.updateTaskDetails(false, 0, QStringLiteral("Remote details")));
                failDetailsTrigger.exec(QStringLiteral("DROP TRIGGER test_fail_details"));
                QVERIFY(ctrl.updateTaskDetails(false, 0, QStringLiteral("Valid details")));

                QSqlQuery failCommentCtrlTrigger(dbConn);
                failCommentCtrlTrigger.exec(QStringLiteral(
                    "CREATE TRIGGER test_fail_comm_ctrl BEFORE UPDATE OF comments_json ON tasks "
                    "BEGIN "
                    "  SELECT RAISE(ABORT, 'Simulated comment ctrl failure'); "
                    "END;"
                ));
                QVERIFY(!ctrl.addTaskComment(false, 0, QStringLiteral("Ctrl comment")));
                failCommentCtrlTrigger.exec(QStringLiteral("DROP TRIGGER test_fail_comm_ctrl"));
                QVERIFY(ctrl.addTaskComment(false, 0, QStringLiteral("Ctrl comment")));

                QSqlQuery failTextTrigger(dbConn);
                failTextTrigger.exec(QStringLiteral(
                    "CREATE TRIGGER test_fail_text BEFORE UPDATE OF description ON tasks "
                    "BEGIN "
                    "  SELECT RAISE(ABORT, 'Simulated text failure'); "
                    "END;"
                ));
                QVERIFY(!ctrl.updateTaskText(false, 0, QStringLiteral("Failed update text")));
                failTextTrigger.exec(QStringLiteral("DROP TRIGGER test_fail_text"));
                QVERIFY(ctrl.updateTaskText(false, 0, QStringLiteral("Success update text")));
            }

            // 2. Adding a task on top must roll back the INSERT if reordering fails
            {
                TaskManager isolatedManager;
                isolatedManager.setActiveList(listId, false, false);
                const int initialTaskCount = DatabaseManager::instance().countAllTasksInList(listId);

                QSqlQuery triggerQ(dbConn);
                QVERIFY(triggerQ.exec(QStringLiteral(
                    "CREATE TRIGGER test_fail_add_on_top_reorder BEFORE UPDATE OF sort_order ON tasks "
                    "BEGIN "
                    "  SELECT RAISE(ABORT, 'Simulated add-on-top reorder failure'); "
                    "END;"
                )));

                isolatedManager.addNewTask(QStringLiteral("Atomic top insert"), true, false);
                QCOMPARE(DatabaseManager::instance().countAllTasksInList(listId), initialTaskCount);

                QVERIFY(triggerQ.exec(QStringLiteral("DROP TRIGGER test_fail_add_on_top_reorder")));
                QSqlQuery cleanupQ(dbConn);
                cleanupQ.prepare(QStringLiteral("DELETE FROM tasks WHERE list_id = :list_id AND description = :description"));
                cleanupQ.bindValue(QStringLiteral(":list_id"), listId);
                cleanupQ.bindValue(QStringLiteral(":description"), QStringLiteral("Atomic top insert"));
                QVERIFY(cleanupQ.exec());
                ctrl.selectListById(listId);
            }

            // 3. Appending in a multi-list model must use the target list's order, not the global row count
            {
                const QString orderListA = ctrl.listManager()->addList(QStringLiteral("Append Order A"));
                const QString orderListB = ctrl.listManager()->addList(QStringLiteral("Append Order B"));
                QVERIFY(DatabaseManager::instance().insertTask(
                    orderListA, TxtTask::fromSimpleTxt(QStringLiteral("A existing"), false, false), 0) >= 0);
                QVERIFY(DatabaseManager::instance().insertTask(
                    orderListB, TxtTask::fromSimpleTxt(QStringLiteral("B existing"), false, false), 0) >= 0);

                TaskManager multiListManager;
                multiListManager.setActiveLists({orderListA, orderListB}, orderListA, false);
                multiListManager.addNewTask(QStringLiteral("A appended"), false, false);

                QSqlQuery orderQ(dbConn);
                orderQ.prepare(QStringLiteral(
                    "SELECT sort_order FROM tasks WHERE list_id = :list_id AND description = :description"
                ));
                orderQ.bindValue(QStringLiteral(":list_id"), orderListA);
                orderQ.bindValue(QStringLiteral(":description"), QStringLiteral("A appended"));
                QVERIFY(orderQ.exec() && orderQ.next());
                QCOMPARE(orderQ.value(0).toInt(), 1);
            }

            // 4. Prepend reorder must reject stale model rows that update no database record
            {
                const QString staleListId = ctrl.listManager()->addList(QStringLiteral("Stale Prepend Order"));
                TaskManager staleManager;
                staleManager.setActiveList(staleListId, false, false);
                staleManager.addNewTask(QStringLiteral("Stale A"), false, false);
                staleManager.addNewTask(QStringLiteral("Stale B"), false, false);
                QCOMPARE(staleManager.todoModel()->rowCount(), 2);

                const qint64 staleTaskId = staleManager.todoModel()->taskAt(0).id();
                QVERIFY(DatabaseManager::instance().deleteTask(staleTaskId));
                staleManager.addNewTask(QStringLiteral("Stale prepended"), true, false);

                QSqlQuery orderQ(dbConn);
                orderQ.prepare(QStringLiteral(
                    "SELECT sort_order FROM tasks WHERE list_id = :list_id AND description = :description"
                ));
                orderQ.bindValue(QStringLiteral(":list_id"), staleListId);
                orderQ.bindValue(QStringLiteral(":description"), QStringLiteral("Stale B"));
                QVERIFY(orderQ.exec() && orderQ.next());
                QCOMPARE(orderQ.value(0).toInt(), 1);
            }

            // 5. Deleting a task must compact orders before a subsequent append
            {
                const QString deleteOrderListId = ctrl.listManager()->addList(QStringLiteral("Delete Order Compaction"));
                TaskManager deleteOrderManager;
                deleteOrderManager.setActiveList(deleteOrderListId, false, false);
                deleteOrderManager.addNewTask(QStringLiteral("Delete order A"), false, false);
                deleteOrderManager.addNewTask(QStringLiteral("Delete order B"), false, false);
                deleteOrderManager.addNewTask(QStringLiteral("Delete order C"), false, false);
                QCOMPARE(deleteOrderManager.todoModel()->rowCount(), 3);

                QVERIFY(deleteOrderManager.todoModel()->removeAt(0));
                deleteOrderManager.addNewTask(QStringLiteral("Delete order D"), false, false);

                QSqlQuery orderQ(dbConn);
                orderQ.prepare(QStringLiteral(
                    "SELECT COUNT(*), COUNT(DISTINCT sort_order), MIN(sort_order), MAX(sort_order) "
                    "FROM tasks WHERE list_id = :list_id AND done = 0"
                ));
                orderQ.bindValue(QStringLiteral(":list_id"), deleteOrderListId);
                QVERIFY(orderQ.exec() && orderQ.next());
                QCOMPARE(orderQ.value(0).toInt(), 3);
                QCOMPARE(orderQ.value(1).toInt(), 3);
                QCOMPARE(orderQ.value(2).toInt(), 0);
                QCOMPARE(orderQ.value(3).toInt(), 2);
            }

            // 6. Delete reorder must reject stale remaining rows that update no database record
            {
                const QString staleDeleteListId = ctrl.listManager()->addList(QStringLiteral("Stale Delete Reorder"));
                TaskManager staleDeleteManager;
                staleDeleteManager.setActiveList(staleDeleteListId, false, false);
                staleDeleteManager.addNewTask(QStringLiteral("Delete stale A"), false, false);
                staleDeleteManager.addNewTask(QStringLiteral("Delete stale B"), false, false);
                staleDeleteManager.addNewTask(QStringLiteral("Delete stale C"), false, false);
                QCOMPARE(staleDeleteManager.todoModel()->rowCount(), 3);

                const qint64 externallyDeletedId = staleDeleteManager.todoModel()->taskAt(1).id();
                QVERIFY(DatabaseManager::instance().deleteTask(externallyDeletedId));
                const bool removed = staleDeleteManager.todoModel()->removeAt(0);
                QVERIFY(!removed);
            }

            // 7. Batch task reorder must reject stale task IDs
            {
                const QString staleBatchListId = ctrl.listManager()->addList(QStringLiteral("Stale Batch Reorder"));
                QVERIFY(DatabaseManager::instance().insertTask(
                    staleBatchListId, TxtTask::fromSimpleTxt(QStringLiteral("Batch A"), false, false), 0) >= 0);
                QVERIFY(DatabaseManager::instance().insertTask(
                    staleBatchListId, TxtTask::fromSimpleTxt(QStringLiteral("Batch B"), false, false), 1) >= 0);

                QVector<TxtTask> batchTasks = DatabaseManager::instance().loadTasks(staleBatchListId, false);
                QCOMPARE(batchTasks.size(), 2);
                const qint64 originalFirstTaskId = batchTasks.at(0).id();
                const qint64 originalSecondTaskId = batchTasks.at(1).id();
                std::reverse(batchTasks.begin(), batchTasks.end());
                TxtTask staleTask = TxtTask::fromSimpleTxt(QStringLiteral("Missing batch task"), false, false);
                staleTask.setId(99999999);
                batchTasks.insert(1, staleTask);

                QVERIFY(!DatabaseManager::instance().replaceListTasksOrders(staleBatchListId, false, batchTasks));
                const QVector<TxtTask> tasksAfterFailedReorder =
                    DatabaseManager::instance().loadTasks(staleBatchListId, false);
                QCOMPARE(tasksAfterFailedReorder.size(), 2);
                QCOMPARE(tasksAfterFailedReorder.at(0).id(), originalFirstTaskId);
                QCOMPARE(tasksAfterFailedReorder.at(1).id(), originalSecondTaskId);
            }

            // 8. Batch list reorder must reject stale list IDs
            {
                const QString staleListA = ctrl.listManager()->addList(QStringLiteral("Stale List Order A"));
                const QString staleListB = ctrl.listManager()->addList(QStringLiteral("Stale List Order B"));
                TodoListEntry listA = ctrl.listManager()->entry(ctrl.listManager()->indexOfId(staleListA));
                TodoListEntry listB = ctrl.listManager()->entry(ctrl.listManager()->indexOfId(staleListB));
                TodoListEntry missingList;
                missingList.id = QStringLiteral("missing-list-order-id");
                missingList.name = QStringLiteral("Missing list order");

                QSqlQuery orderBefore(dbConn);
                orderBefore.prepare(QStringLiteral("SELECT id, sort_order FROM lists WHERE id IN (:a, :b)"));
                orderBefore.bindValue(QStringLiteral(":a"), staleListA);
                orderBefore.bindValue(QStringLiteral(":b"), staleListB);
                QVERIFY(orderBefore.exec());
                QHash<QString, int> originalOrders;
                while (orderBefore.next()) {
                    originalOrders.insert(orderBefore.value(0).toString(), orderBefore.value(1).toInt());
                }
                QCOMPARE(originalOrders.size(), 2);

                const QVector<TodoListEntry> entries{listB, missingList, listA};
                QVERIFY(!DatabaseManager::instance().updateListsOrder(entries));

                QSqlQuery orderAfter(dbConn);
                orderAfter.prepare(QStringLiteral("SELECT id, sort_order FROM lists WHERE id IN (:a, :b)"));
                orderAfter.bindValue(QStringLiteral(":a"), staleListA);
                orderAfter.bindValue(QStringLiteral(":b"), staleListB);
                QVERIFY(orderAfter.exec());
                int verifiedLists = 0;
                while (orderAfter.next()) {
                    const QString id = orderAfter.value(0).toString();
                    QCOMPARE(orderAfter.value(1).toInt(), originalOrders.value(id));
                    ++verifiedLists;
                }
                QCOMPARE(verifiedLists, 2);
            }

            // 9. Moving a task must reject stale remaining rows and roll back the move
            {
                const QString staleMoveListId = ctrl.listManager()->addList(QStringLiteral("Stale Move Reorder"));
                QVERIFY(DatabaseManager::instance().insertTask(
                    staleMoveListId, TxtTask::fromSimpleTxt(QStringLiteral("Move A"), false, false), 0) >= 0);
                QVERIFY(DatabaseManager::instance().insertTask(
                    staleMoveListId, TxtTask::fromSimpleTxt(QStringLiteral("Move B"), false, false), 1) >= 0);
                QVector<TxtTask> moveTasks = DatabaseManager::instance().loadTasks(staleMoveListId, false);
                QCOMPARE(moveTasks.size(), 2);
                TxtTask taskToMove = moveTasks.at(0);
                taskToMove.setDone(true);
                taskToMove.setStatus(QStringLiteral("done"));
                taskToMove.setId(moveTasks.at(0).id());
                TxtTask staleRemaining = moveTasks.at(1);
                staleRemaining.setId(99999998);
                QMap<QString, QVector<TxtTask>> remaining;
                remaining.insert(staleMoveListId, QVector<TxtTask>{staleRemaining});

                const bool moveRejected = !DatabaseManager::instance().moveTaskDoneAndReorder(taskToMove, 0, remaining, false);

                QSqlQuery moveState(dbConn);
                moveState.prepare(QStringLiteral("SELECT done, status FROM tasks WHERE id = :id"));
                moveState.bindValue(QStringLiteral(":id"), moveTasks.at(0).id());
                QVERIFY(moveState.exec() && moveState.next());
                const bool moveRolledBack = moveState.value(0).toInt() == 0
                    && moveState.value(1).toString() == QStringLiteral("todo");
                QVERIFY(moveRejected && moveRolledBack);
            }

            // 10. Timer side-effects rollback safety: no activity log or worklog on failed toggleTaskDone
            {
                ctrl.setActiveTodoRow(0);
                ctrl.timer()->start();
                QVERIFY(ctrl.timer()->isRunning());
                QThread::msleep(1050);

                // Count existing activity logs
                QSqlQuery countQ(dbConn);
                QVERIFY(countQ.exec(QStringLiteral("SELECT COUNT(*) FROM activity_log")));
                QVERIFY(countQ.next());
                const int initialActivityCount = countQ.value(0).toInt();

                // Setup abort trigger
                QSqlQuery triggerQ(dbConn);
                triggerQ.exec(QStringLiteral(
                    "CREATE TRIGGER test_fail_timer_toggle BEFORE UPDATE OF done ON tasks "
                    "BEGIN "
                    "  SELECT RAISE(ABORT, 'Simulated toggleTaskDone failure'); "
                    "END;"
                ));

                // Attempt to toggle done while timer running - must fail
                ctrl.toggleTaskDone(false, 0);

                // Timer MUST still be running!
                QVERIFY(ctrl.timer()->isRunning());

                // NO new activity log must have been inserted
                QVERIFY(countQ.exec(QStringLiteral("SELECT COUNT(*) FROM activity_log")));
                QVERIFY(countQ.next());
                QCOMPARE(countQ.value(0).toInt(), initialActivityCount);

                // Drop trigger and complete task successfully
                triggerQ.exec(QStringLiteral("DROP TRIGGER test_fail_timer_toggle"));
                ctrl.toggleTaskDone(false, 0);
                QVERIFY(!ctrl.timer()->isRunning());

                // Now exactly 1 activity log entry must be created
                QVERIFY(countQ.exec(QStringLiteral("SELECT COUNT(*) FROM activity_log")));
                QVERIFY(countQ.next());
                QCOMPARE(countQ.value(0).toInt(), initialActivityCount + 1);
            }

            // 11. Timer persistence failure must not stop/log the active session or duplicate errors
            {
                ctrl.selectListById(listId);
                ctrl.setActiveTodoRow(0);
                ctrl.timer()->start();
                QVERIFY(ctrl.timer()->isRunning());
                QThread::msleep(1050);

                QSqlQuery countQ(dbConn);
                QVERIFY(countQ.exec(QStringLiteral("SELECT COUNT(*) FROM activity_log")));
                QVERIFY(countQ.next());
                const int initialActivityCount = countQ.value(0).toInt();

                QSqlQuery triggerQ(dbConn);
                QVERIFY(triggerQ.exec(QStringLiteral(
                    "CREATE TRIGGER test_fail_timer_progress BEFORE UPDATE OF timer_seconds ON tasks "
                    "BEGIN "
                    "  SELECT RAISE(ABORT, 'Simulated timer progress failure'); "
                    "END;"
                )));

                int databaseErrorCount = 0;
                const QMetaObject::Connection errorConnection = QObject::connect(
                    &ctrl, &AppController::databaseError, [&databaseErrorCount](const QString&) {
                        ++databaseErrorCount;
                    });

                QVERIFY(QMetaObject::invokeMethod(
                    &ctrl,
                    "onActiveTaskTimeUpdated",
                    Qt::DirectConnection,
                    Q_ARG(uint, ctrl.timer()->activeTaskTimerValue() + 1U)));

                QVERIFY(ctrl.timer()->isRunning());
                QVERIFY(countQ.exec(QStringLiteral("SELECT COUNT(*) FROM activity_log")));
                QVERIFY(countQ.next());
                QCOMPARE(countQ.value(0).toInt(), initialActivityCount);
                QCOMPARE(databaseErrorCount, 1);

                QObject::disconnect(errorConnection);
                QVERIFY(triggerQ.exec(QStringLiteral("DROP TRIGGER test_fail_timer_progress")));
                if (ctrl.timer()->isRunning()) {
                    ctrl.timer()->stop();
                }
            }

            // 12. Failed deletion of the active task must not stop/log its timer
            {
                ctrl.selectListById(listId);
                ctrl.setActiveTodoRow(0);
                ctrl.timer()->start();
                QVERIFY(ctrl.timer()->isRunning());
                QThread::msleep(1050);

                const int initialTodoCount = ctrl.todoProxyModel()->rowCount();
                QSqlQuery countQ(dbConn);
                QVERIFY(countQ.exec(QStringLiteral("SELECT COUNT(*) FROM activity_log")));
                QVERIFY(countQ.next());
                const int initialActivityCount = countQ.value(0).toInt();

                QSqlQuery triggerQ(dbConn);
                QVERIFY(triggerQ.exec(QStringLiteral(
                    "CREATE TRIGGER test_fail_active_delete BEFORE DELETE ON tasks "
                    "BEGIN "
                    "  SELECT RAISE(ABORT, 'Simulated active task deletion failure'); "
                    "END;"
                )));

                ctrl.removeTask(false, 0);
                QCOMPARE(ctrl.todoProxyModel()->rowCount(), initialTodoCount);
                QVERIFY(ctrl.timer()->isRunning());
                QVERIFY(countQ.exec(QStringLiteral("SELECT COUNT(*) FROM activity_log")));
                QVERIFY(countQ.next());
                QCOMPARE(countQ.value(0).toInt(), initialActivityCount);

                QVERIFY(triggerQ.exec(QStringLiteral("DROP TRIGGER test_fail_active_delete")));
                if (ctrl.timer()->isRunning()) {
                    ctrl.timer()->stop();
                }
            }

            // 13. Remote open <-> closed sort_order updates and list compaction in upsertRemoteIssueTask
            {
                auto& db = DatabaseManager::instance();
                const QString remListId = ctrl.listManager()->addList(QStringLiteral("Remote Sort Test"));

                bool isNew = false;
                QString oldStatus;
                QVERIFY(db.upsertRemoteIssueTask(remListId, QStringLiteral("KEY-1"), QStringLiteral("Task 1"), QStringLiteral("todo"), 0, 0, QString(), {}, &isNew, &oldStatus));
                QVERIFY(db.upsertRemoteIssueTask(remListId, QStringLiteral("KEY-2"), QStringLiteral("Task 2"), QStringLiteral("todo"), 0, 0, QString(), {}, &isNew, &oldStatus));
                QVERIFY(db.upsertRemoteIssueTask(remListId, QStringLiteral("KEY-3"), QStringLiteral("Task 3"), QStringLiteral("todo"), 0, 0, QString(), {}, &isNew, &oldStatus));

                // Verify initial sort orders: 0, 1, 2
                auto loadOrder = [&](const QString& key) -> int {
                    QSqlQuery q(dbConn);
                    q.prepare(QStringLiteral("SELECT sort_order FROM tasks WHERE list_id = :lid AND jira_key = :k"));
                    q.bindValue(QStringLiteral(":lid"), remListId);
                    q.bindValue(QStringLiteral(":k"), key);
                    if (q.exec() && q.next()) return q.value(0).toInt();
                    return -1;
                };
                QCOMPARE(loadOrder(QStringLiteral("KEY-1")), 0);
                QCOMPARE(loadOrder(QStringLiteral("KEY-2")), 1);
                QCOMPARE(loadOrder(QStringLiteral("KEY-3")), 2);

                // Close KEY-2 via remote sync
                QVERIFY(db.upsertRemoteIssueTask(remListId, QStringLiteral("KEY-2"), QStringLiteral("Task 2"), QStringLiteral("done"), 0, 0, QString(), {}, &isNew, &oldStatus));

                // KEY-2 in done list should have sort_order 0
                QCOMPARE(loadOrder(QStringLiteral("KEY-2")), 0);
                // In todo list: KEY-1 should still have 0, KEY-3 compacted from 2 to 1!
                QCOMPARE(loadOrder(QStringLiteral("KEY-1")), 0);
                QCOMPARE(loadOrder(QStringLiteral("KEY-3")), 1);

                // Reopen KEY-2 via remote sync
                QVERIFY(db.upsertRemoteIssueTask(remListId, QStringLiteral("KEY-2"), QStringLiteral("Task 2"), QStringLiteral("todo"), 0, 0, QString(), {}, &isNew, &oldStatus));

                // In todo list: KEY-2 should now be at end (sort_order 2)
                QCOMPARE(loadOrder(QStringLiteral("KEY-1")), 0);
                QCOMPARE(loadOrder(QStringLiteral("KEY-3")), 1);
                QCOMPARE(loadOrder(QStringLiteral("KEY-2")), 2);

                // Compaction failure rollback in upsertRemoteIssueTask
                QSqlQuery failCompactTrigger(dbConn);
                failCompactTrigger.exec(QStringLiteral(
                    "CREATE TRIGGER test_fail_compact BEFORE UPDATE OF sort_order ON tasks "
                    "WHEN NEW.description LIKE '%Task 3%' "
                    "BEGIN "
                    "  SELECT RAISE(ABORT, 'Simulated compaction failure'); "
                    "END;"
                ));

                // Moving KEY-1 from todo to done should fail and rollback because compacting KEY-3's sort_order aborts
                QVERIFY(!db.upsertRemoteIssueTask(remListId, QStringLiteral("KEY-1"), QStringLiteral("Task 1"), QStringLiteral("done"), 0, 0, QString(), {}, &isNew, &oldStatus));

                // KEY-1 must still be open in SQLite, sort orders must remain unchanged
                QCOMPARE(loadOrder(QStringLiteral("KEY-1")), 0);
                QCOMPARE(loadOrder(QStringLiteral("KEY-3")), 1);
                QCOMPARE(loadOrder(QStringLiteral("KEY-2")), 2);

                failCompactTrigger.exec(QStringLiteral("DROP TRIGGER test_fail_compact"));
            }

            // 14. markActivityWorklogSynced() validation
            {
                auto& db = DatabaseManager::instance();
                // Invalid IDs or non-existent log IDs must return false
                QVERIFY(!db.markActivityWorklogSynced(-1));
                QVERIFY(!db.markActivityWorklogSynced(999999));
            }

            // 15. loadTask distinction between not-found and query failure
            {
                auto& db = DatabaseManager::instance();
                bool queryError = true;
                // Non-existent task ID returns nullopt and sets queryError = false
                auto resNotFound = db.loadTask(999999, &queryError);
                QVERIFY(!resNotFound.has_value());
                QCOMPARE(queryError, false);

                // Negative task ID returns nullopt and sets queryError = false
                queryError = true;
                auto resNegative = db.loadTask(-1, &queryError);
                QVERIFY(!resNegative.has_value());
                QCOMPARE(queryError, false);
            }
        }

        DatabaseManager::instance().close();
        QFile::remove(testDb);
    }

    void testRedmineStatusMappingDoesNotResetTimerOrMultiList() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("gofi_status_mapping_safety.db"));
        QFile::remove(testDb);
        {
            AppController ctrl(QString(), testDb);
            const QString rmListId = ctrl.listManager()->addList(QStringLiteral("Redmine List"));
            QVariantMap cfg = ctrl.listManager()->getListConfig(rmListId);
            cfg[QStringLiteral("provider")] = QStringLiteral("Redmine");
            cfg[QStringLiteral("jiraUrl")] = QStringLiteral("https://redmine.example.com");
            cfg[QStringLiteral("apiToken")] = QStringLiteral("secret-token");
            ctrl.listManager()->saveListConfig(cfg);

            // Add 3 tasks
            ctrl.selectListById(rmListId);
            ctrl.addNewTask(QStringLiteral("Task 1"));
            ctrl.addNewTask(QStringLiteral("Task 2"));
            ctrl.addNewTask(QStringLiteral("Task 3"));

            // 1. Single-list view: last row active, timer started
            ctrl.setActiveTodoRow(2);
            QCOMPARE(ctrl.activeTodoRow(), 2);
            ctrl.timer()->start();
            QVERIFY(ctrl.timer()->isRunning());

            QVariantMap mappingWithNewKey;
            mappingWithNewKey.insert(QStringLiteral("_id_Closed"), 5);
            emit ctrl.issueTracker()->listStatusMappingUpdated(rmListId, mappingWithNewKey);

            // Timer must still be running and activeTodoRow must not have jumped to 0
            QVERIFY(ctrl.timer()->isRunning());
            QCOMPARE(ctrl.activeTodoRow(), 2);
            ctrl.timer()->stop();

            // 2. Multi-list view: showAllLists, timer started
            ctrl.showAllLists();
            QVERIFY(ctrl.isMultiListView());
            ctrl.setActiveTodoRow(1);
            ctrl.timer()->start();
            QVERIFY(ctrl.timer()->isRunning());

            emit ctrl.issueTracker()->listStatusMappingUpdated(rmListId, mappingWithNewKey);

            QVERIFY(ctrl.timer()->isRunning());
            QCOMPARE(ctrl.activeTodoRow(), 1);
            QVERIFY(ctrl.isMultiListView());
            ctrl.timer()->stop();
        }
        DatabaseManager::instance().close();
        QFile::remove(testDb);
    }

    void testTaskTimerNoDoubleCountingOnFlush() {
        SettingsManager settings;
        TaskTimer timer(&settings);
        timer.setActiveTask(QStringLiteral("Test Task"), 0, 3600);
        timer.start();
        QVERIFY(timer.isRunning());
        QTest::qWait(1100);
        const uint flushedSec = timer.flushActiveTaskSeconds();
        QCOMPARE(flushedSec, 1u);
        QTest::qWait(300);
        timer.stop();
        QCOMPARE(timer.activeTaskTimerValue(), 1u);
    }

    // KB-12: a 2xx CalDAV answer that is not a multistatus with VTODOs (e.g. an HTML
    // login page reached through a redirect) must not be treated as "explicitly empty".
    void testNextcloudNonCalDavBodyDoesNotCloseTasks() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        serveHttp(server, [](const QByteArray&, const QByteArray&, const QByteArray&) {
            return httpResponse("200 OK", "text/html; charset=utf-8",
                                "<!DOCTYPE html><html><body>Nextcloud login</body></html>");
        });

        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(m_testSandbox.filePath(QStringLiteral("kb12_nextcloud.db"))));

        TodoListEntry entry;
        entry.id = QStringLiteral("kb12_nc");
        entry.name = QStringLiteral("KB12 Nextcloud");
        entry.provider = QStringLiteral("Nextcloud");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1/remote.php/dav/calendars/user/tasks/").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user");
        entry.apiToken = QStringLiteral("app-password");
        QVERIFY(db.saveList(entry, 0));

        for (int i = 1; i <= 2; ++i) {
            TxtTask t = TxtTask::fromSimpleTxt(QStringLiteral("NC task %1").arg(i), false, false);
            t.setJiraKey(QStringLiteral("NC#uid-%1").arg(i));
            QVERIFY(db.insertTask(entry.id, t, i - 1) >= 0);
        }

        IssueTrackerService svc;
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);

        QCOMPARE(db.loadTasks(entry.id, false).size(), 2);
        server.close();
    }

    // KB-13: offset pagination skips an item when an earlier item leaves the result set
    // between page requests; reconciliation must not close the skipped, still-open issue.
    void testRedminePaginationShiftDoesNotCloseSkippedIssue() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        auto issueJson = [](int id) {
            return QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("subject"), QStringLiteral("Issue %1").arg(id)},
                {QStringLiteral("status"), QJsonObject{{QStringLiteral("id"), 1}, {QStringLiteral("name"), QStringLiteral("New")}}},
                {QStringLiteral("project"), QJsonObject{{QStringLiteral("name"), QStringLiteral("Proj")}, {QStringLiteral("identifier"), QStringLiteral("proj")}}}
            };
        };
        // Page 1 is served while issues 1..150 are open. Before page 2 is requested,
        // issue 5 is closed remotely, so every later issue shifts one position up.
        serveHttp(server, [&issueJson](const QByteArray&, const QByteArray& target, const QByteArray&) {
            if (!target.startsWith("/issues.json")) {
                return httpResponse("404 Not Found", "application/json", "{}");
            }
            QVector<int> open;
            const bool secondPage = target.contains("offset=");
            for (int id = 1; id <= 150; ++id) {
                if (secondPage && id == 5) continue;
                open.append(id);
            }
            const int offset = secondPage ? 100 : 0;
            QJsonArray issues;
            for (int i = offset; i < qMin(offset + 100, static_cast<int>(open.size())); ++i) {
                issues.append(issueJson(open.at(i)));
            }
            const QJsonObject root{
                {QStringLiteral("issues"), issues},
                {QStringLiteral("total_count"), static_cast<int>(open.size())},
                {QStringLiteral("offset"), offset},
                {QStringLiteral("limit"), 100}
            };
            return httpResponse("200 OK", "application/json", QJsonDocument(root).toJson(QJsonDocument::Compact));
        });

        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(m_testSandbox.filePath(QStringLiteral("kb13_redmine.db"))));

        TodoListEntry entry;
        entry.id = QStringLiteral("kb13_rm");
        entry.name = QStringLiteral("KB13 Redmine");
        entry.provider = QStringLiteral("Redmine");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.apiToken = QStringLiteral("token");
        entry.statusMapping[QStringLiteral("_statuses_fetched")] = 1;
        entry.statusMapping[QStringLiteral("_id_New")] = 1;
        QVERIFY(db.saveList(entry, 0));

        for (int id = 1; id <= 150; ++id) {
            TxtTask t = TxtTask::fromSimpleTxt(QStringLiteral("Issue %1").arg(id), false, false);
            t.setJiraKey(QStringLiteral("RM#proj#%1").arg(id));
            QVERIFY(db.insertTask(entry.id, t, id - 1) >= 0);
        }

        IssueTrackerService svc;
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);

        QSet<QString> openKeys;
        for (const TxtTask& t : db.loadTasks(entry.id, false)) {
            openKeys.insert(t.jiraKey());
        }
        // Issue 5 was still open when page 1 was served, so it legitimately stays open
        // until the next sync; nothing else was closed remotely.
        QVERIFY(openKeys.contains(QStringLiteral("RM#proj#5")));
        QVERIFY(openKeys.contains(QStringLiteral("RM#proj#101")));
        server.close();
    }

    // KB-14: when no preferred status name matches, the learned is_closed fallback must
    // skip rejected/canceled statuses, like the explicit-mapping path already does.
    void testRedmineLearnedClosedFallbackSkipsRejected() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        QByteArray putBody;
        QByteArray putTarget;
        serveHttp(server, [&putBody, &putTarget](const QByteArray& method, const QByteArray& target, const QByteArray& body) {
            if (method == "PUT") {
                putBody = body;
                putTarget = target;
            }
            return httpResponse("204 No Content", "application/json", QByteArray());
        });

        TodoListEntry entry;
        entry.id = QStringLiteral("kb14_rm");
        entry.name = QStringLiteral("KB14 Redmine");
        entry.provider = QStringLiteral("Redmine");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.apiToken = QStringLiteral("token");
        entry.syncStatusToRemote = true;
        // Learned from /issue_statuses.json; no explicit "<name>: done" mapping and no
        // status literally named Closed/Resolved.
        entry.statusMapping[QStringLiteral("_statuses_fetched")] = 1;
        entry.statusMapping[QStringLiteral("_id_Done")] = 6;
        entry.statusMapping[QStringLiteral("_id_Rejected")] = 10;
        entry.statusMapping[QStringLiteral("_is_closed_6")] = 1;
        entry.statusMapping[QStringLiteral("_is_closed_10")] = 1;

        IssueTrackerService svc;
        // updateIssueStatusRemote is a no-op in test mode; lift it only for this local request.
        QStandardPaths::setTestModeEnabled(false);
        svc.updateIssueStatusRemote(entry, QStringLiteral("RM#proj#42"), true, QStringLiteral("done"));
        QStandardPaths::setTestModeEnabled(true);

        QTRY_VERIFY_WITH_TIMEOUT(!putBody.isEmpty(), 5000);
        QCOMPARE(putTarget, QByteArray("/issues/42.json"));
        const int sentStatus = QJsonDocument::fromJson(putBody).object()
            .value(QStringLiteral("issue")).toObject().value(QStringLiteral("status_id")).toInt();
        QCOMPARE(sentStatus, 6);
        server.close();
    }

    // KB-15: the KB-13 shift guard (first-page total vs. later totals) was added only for
    // Jira, GitHub and Redmine. GitLab (page + X-Next-Page) still closes a skipped issue.
    void testGitLabPaginationShiftDoesNotCloseSkippedIssue() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        const quint16 port = server.serverPort();
        serveHttp(server, [port](const QByteArray&, const QByteArray& target, const QByteArray&) {
            if (target.contains("/merge_requests")) {
                return httpResponse("200 OK", "application/json", "[]", "X-Total: 0\r\n");
            }
            // Issue 5 is closed after page 1 was served; later issues shift up by one.
            const bool secondPage = target.contains("page=2");
            QVector<int> open;
            for (int iid = 1; iid <= 150; ++iid) {
                if (secondPage && iid == 5) continue;
                open.append(iid);
            }
            const int offset = secondPage ? 100 : 0;
            QJsonArray arr;
            for (int i = offset; i < qMin(offset + 100, static_cast<int>(open.size())); ++i) {
                const int iid = open.at(i);
                arr.append(QJsonObject{
                    {QStringLiteral("iid"), iid},
                    {QStringLiteral("title"), QStringLiteral("Issue %1").arg(iid)},
                    {QStringLiteral("state"), QStringLiteral("opened")},
                    {QStringLiteral("web_url"), QStringLiteral("http://127.0.0.1:%1/grp/proj/-/issues/%2").arg(port).arg(iid)}
                });
            }
            QByteArray headers = "X-Total: " + QByteArray::number(open.size()) + "\r\n";
            if (!secondPage) headers += "X-Next-Page: 2\r\n";
            return httpResponse("200 OK", "application/json", QJsonDocument(arr).toJson(QJsonDocument::Compact), headers);
        });

        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(m_testSandbox.filePath(QStringLiteral("kb15_gitlab.db"))));
        TodoListEntry entry;
        entry.id = QStringLiteral("kb15_gl");
        entry.name = QStringLiteral("KB15 GitLab");
        entry.provider = QStringLiteral("GitLab");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
        entry.jiraJql = QStringLiteral("grp/proj");
        entry.apiToken = QStringLiteral("token");
        QVERIFY(db.saveList(entry, 0));
        for (int iid = 1; iid <= 150; ++iid) {
            TxtTask t = TxtTask::fromSimpleTxt(QStringLiteral("Issue %1").arg(iid), false, false);
            t.setJiraKey(QStringLiteral("GL#grp/proj#%1").arg(iid));
            QVERIFY(db.insertTask(entry.id, t, iid - 1) >= 0);
        }

        IssueTrackerService svc;
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);

        QSet<QString> openKeys;
        for (const TxtTask& t : db.loadTasks(entry.id, false)) openKeys.insert(t.jiraKey());
        QVERIFY(openKeys.contains(QStringLiteral("GL#grp/proj#5")));   // seen open on page 1
        QVERIFY(openKeys.contains(QStringLiteral("GL#grp/proj#101")));
        server.close();
    }

    // KB-15: YouTrack ($skip, no total in the response) has no shift guard either.
    void testYouTrackPaginationShiftDoesNotCloseSkippedIssue() {
        const QSet<QString> openKeys = runYouTrackShiftScenario(QStringLiteral("kb15_yt"), true);
        if (openKeys.isEmpty()) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        QVERIFY(openKeys.contains(QStringLiteral("YT-5")));
        QVERIFY(openKeys.contains(QStringLiteral("YT-101")));
    }

    // KB-16: a 207 multistatus in which every object reports calendar-data as 404/403
    // (PROPFIND fallback on servers that only serve calendar-data via REPORT, or revoked
    // read access) contains resources but no VTODO data; it must not be treated as an
    // explicitly empty calendar.
    void testNextcloudMultistatusWithoutCalendarDataDoesNotCloseTasks() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        serveHttp(server, [](const QByteArray& method, const QByteArray&, const QByteArray&) {
            if (method == "REPORT") {
                return httpResponse("405 Method Not Allowed", "text/plain", "REPORT not supported");
            }
            const QByteArray xml =
                "<?xml version=\"1.0\"?>"
                "<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
                "<d:response><d:href>/remote.php/dav/calendars/user/tasks/</d:href>"
                "<d:propstat><d:prop><c:calendar-data/></d:prop><d:status>HTTP/1.1 404 Not Found</d:status></d:propstat></d:response>"
                "<d:response><d:href>/remote.php/dav/calendars/user/tasks/uid-1.ics</d:href>"
                "<d:propstat><d:prop><c:calendar-data/></d:prop><d:status>HTTP/1.1 404 Not Found</d:status></d:propstat></d:response>"
                "<d:response><d:href>/remote.php/dav/calendars/user/tasks/uid-2.ics</d:href>"
                "<d:propstat><d:prop><c:calendar-data/></d:prop><d:status>HTTP/1.1 404 Not Found</d:status></d:propstat></d:response>"
                "</d:multistatus>";
            return httpResponse("207 Multi-Status", "application/xml; charset=utf-8", xml);
        });

        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(m_testSandbox.filePath(QStringLiteral("kb16_nextcloud.db"))));
        TodoListEntry entry;
        entry.id = QStringLiteral("kb16_nc");
        entry.name = QStringLiteral("KB16 Nextcloud");
        entry.provider = QStringLiteral("Nextcloud");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1/remote.php/dav/calendars/user/tasks/").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user");
        entry.apiToken = QStringLiteral("app-password");
        QVERIFY(db.saveList(entry, 0));
        for (int i = 1; i <= 2; ++i) {
            TxtTask t = TxtTask::fromSimpleTxt(QStringLiteral("NC task %1").arg(i), false, false);
            t.setJiraKey(QStringLiteral("NC#uid-%1").arg(i));
            QVERIFY(db.insertTask(entry.id, t, i - 1) >= 0);
        }

        IssueTrackerService svc;
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);

        QCOMPARE(db.loadTasks(entry.id, false).size(), 2);
        server.close();
    }

    // KB-17: a multistatus where only some objects return calendar-data (the others
    // report 404/403) yields a non-empty but incomplete item list. The objects without
    // data must not be reconciled as closed.
    void testNextcloudPartialCalendarDataDoesNotCloseMissingTasks() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        serveHttp(server, [](const QByteArray& method, const QByteArray&, const QByteArray&) {
            if (method == "REPORT") {
                return httpResponse("405 Method Not Allowed", "text/plain", "REPORT not supported");
            }
            const QByteArray xml =
                "<?xml version=\"1.0\"?>"
                "<d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
                "<d:response><d:href>/remote.php/dav/calendars/user/tasks/uid-1.ics</d:href>"
                "<d:propstat><d:prop><c:calendar-data>BEGIN:VCALENDAR\nVERSION:2.0\nBEGIN:VTODO\nUID:uid-1\nSUMMARY:NC task 1\nSTATUS:NEEDS-ACTION\nEND:VTODO\nEND:VCALENDAR\n</c:calendar-data></d:prop>"
                "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
                "<d:response><d:href>/remote.php/dav/calendars/user/tasks/uid-2.ics</d:href>"
                "<d:propstat><d:prop><c:calendar-data/></d:prop><d:status>HTTP/1.1 404 Not Found</d:status></d:propstat></d:response>"
                "</d:multistatus>";
            return httpResponse("207 Multi-Status", "application/xml; charset=utf-8", xml);
        });

        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(m_testSandbox.filePath(QStringLiteral("kb17_nextcloud.db"))));
        TodoListEntry entry;
        entry.id = QStringLiteral("kb17_nc");
        entry.name = QStringLiteral("KB17 Nextcloud");
        entry.provider = QStringLiteral("Nextcloud");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1/remote.php/dav/calendars/user/tasks/").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user");
        entry.apiToken = QStringLiteral("app-password");
        QVERIFY(db.saveList(entry, 0));
        for (int i = 1; i <= 2; ++i) {
            TxtTask t = TxtTask::fromSimpleTxt(QStringLiteral("NC task %1").arg(i), false, false);
            t.setJiraKey(QStringLiteral("NC#uid-%1").arg(i));
            QVERIFY(db.insertTask(entry.id, t, i - 1) >= 0);
        }

        IssueTrackerService svc;
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);

        QSet<QString> openKeys;
        for (const TxtTask& t : db.loadTasks(entry.id, false)) openKeys.insert(t.jiraKey());
        QVERIFY(openKeys.contains(QStringLiteral("NC#uid-1")));
        QVERIFY(openKeys.contains(QStringLiteral("NC#uid-2")));
        server.close();
    }

    // KB-18: the KB-15 fix sets limitReached whenever YouTrack needed more than one page,
    // so lists with >= 100 open issues (exactly 100 included: the empty second page counts)
    // never reconcile, and remotely closed issues stay open locally forever.
    void testYouTrackMultiPageListStillReconcilesClosedIssue() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        // Stable server state: YT-1..YT-100 open (exactly one full page); YT-101 was closed.
        serveHttp(server, [](const QByteArray&, const QByteArray& target, const QByteArray&) {
            QJsonArray arr;
            if (!target.contains("skip=")) {
                for (int n = 1; n <= 100; ++n) {
                    arr.append(QJsonObject{
                        {QStringLiteral("idReadable"), QStringLiteral("YT-%1").arg(n)},
                        {QStringLiteral("summary"), QStringLiteral("Issue %1").arg(n)}
                    });
                }
            }
            return httpResponse("200 OK", "application/json", QJsonDocument(arr).toJson(QJsonDocument::Compact));
        });

        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(m_testSandbox.filePath(QStringLiteral("kb18_youtrack.db"))));
        TodoListEntry entry;
        entry.id = QStringLiteral("kb18_yt");
        entry.name = QStringLiteral("KB18 YouTrack");
        entry.provider = QStringLiteral("YouTrack");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.apiToken = QStringLiteral("token");
        QVERIFY(db.saveList(entry, 0));
        for (int n = 1; n <= 101; ++n) {
            TxtTask t = TxtTask::fromSimpleTxt(QStringLiteral("Issue %1").arg(n), false, false);
            t.setJiraKey(QStringLiteral("YT-%1").arg(n));
            QVERIFY(db.insertTask(entry.id, t, n - 1) >= 0);
        }

        IssueTrackerService svc;
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);

        QSet<QString> openKeys;
        for (const TxtTask& t : db.loadTasks(entry.id, false)) openKeys.insert(t.jiraKey());
        QVERIFY(openKeys.contains(QStringLiteral("YT-100")));
        QVERIFY(!openKeys.contains(QStringLiteral("YT-101")));
        server.close();
    }

    // KB-19: KB-18 was fixed only for exactly 100 issues. With two non-empty pages
    // (>100 open issues) limitReached is still forced, so a stable list never reconciles
    // and remotely closed issues stay open locally forever.
    void testYouTrackLargeStableListStillReconcilesClosedIssue() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        // Stable server state: YT-1..YT-150 open; YT-151 was closed remotely.
        serveHttp(server, [](const QByteArray&, const QByteArray& target, const QByteArray&) {
            int skip = 0;
            const int skipIdx = target.indexOf("skip=");
            if (skipIdx >= 0) {
                skip = target.mid(skipIdx + 5).split('&').value(0).toInt();
            }
            QJsonArray arr;
            for (int n = skip + 1; n <= qMin(skip + 100, 150); ++n) {
                arr.append(QJsonObject{
                    {QStringLiteral("idReadable"), QStringLiteral("YT-%1").arg(n)},
                    {QStringLiteral("summary"), QStringLiteral("Issue %1").arg(n)}
                });
            }
            return httpResponse("200 OK", "application/json", QJsonDocument(arr).toJson(QJsonDocument::Compact));
        });

        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(m_testSandbox.filePath(QStringLiteral("kb19_youtrack.db"))));
        TodoListEntry entry;
        entry.id = QStringLiteral("kb19_yt");
        entry.name = QStringLiteral("KB19 YouTrack");
        entry.provider = QStringLiteral("YouTrack");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.apiToken = QStringLiteral("token");
        QVERIFY(db.saveList(entry, 0));
        for (int n = 1; n <= 151; ++n) {
            TxtTask t = TxtTask::fromSimpleTxt(QStringLiteral("Issue %1").arg(n), false, false);
            t.setJiraKey(QStringLiteral("YT-%1").arg(n));
            QVERIFY(db.insertTask(entry.id, t, n - 1) >= 0);
        }

        IssueTrackerService svc;
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);

        QSet<QString> openKeys;
        for (const TxtTask& t : db.loadTasks(entry.id, false)) openKeys.insert(t.jiraKey());
        QVERIFY(openKeys.contains(QStringLiteral("YT-150")));
        QVERIFY(!openKeys.contains(QStringLiteral("YT-151")));
        server.close();
    }

    // KB-20: the KB-19 boundary heuristic treats any locally open key numerically between
    // the last key of page k and the first key of page k+1 as a "shift". An issue that was
    // closed remotely and sits exactly at a page boundary therefore blocks reconciliation on
    // every sync, even though the server state is stable, so it never closes locally.
    void testYouTrackClosedIssueAtPageBoundaryIsReconciled() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        // Stable server state: YT-1..YT-151 except YT-101 (closed remotely) => 150 open.
        QVector<int> open;
        for (int n = 1; n <= 151; ++n) {
            if (n != 101) open.append(n);
        }
        serveHttp(server, [open](const QByteArray&, const QByteArray& target, const QByteArray&) {
            int skip = 0;
            const int skipIdx = target.indexOf("skip=");
            if (skipIdx >= 0) skip = target.mid(skipIdx + 5).split('&').value(0).toInt();
            QJsonArray arr;
            for (int i = skip; i < qMin(skip + 100, static_cast<int>(open.size())); ++i) {
                arr.append(QJsonObject{
                    {QStringLiteral("idReadable"), QStringLiteral("YT-%1").arg(open.at(i))},
                    {QStringLiteral("summary"), QStringLiteral("Issue %1").arg(open.at(i))}
                });
            }
            return httpResponse("200 OK", "application/json", QJsonDocument(arr).toJson(QJsonDocument::Compact));
        });

        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(m_testSandbox.filePath(QStringLiteral("kb20_youtrack.db"))));
        TodoListEntry entry;
        entry.id = QStringLiteral("kb20_yt");
        entry.name = QStringLiteral("KB20 YouTrack");
        entry.provider = QStringLiteral("YouTrack");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.apiToken = QStringLiteral("token");
        QVERIFY(db.saveList(entry, 0));
        for (int n = 1; n <= 151; ++n) {
            TxtTask t = TxtTask::fromSimpleTxt(QStringLiteral("Issue %1").arg(n), false, false);
            t.setJiraKey(QStringLiteral("YT-%1").arg(n));
            QVERIFY(db.insertTask(entry.id, t, n - 1) >= 0);
        }

        IssueTrackerService svc;
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);

        QSet<QString> openKeys;
        for (const TxtTask& t : db.loadTasks(entry.id, false)) openKeys.insert(t.jiraKey());
        QVERIFY(openKeys.contains(QStringLiteral("YT-100")));
        QVERIFY(openKeys.contains(QStringLiteral("YT-102")));
        QVERIFY(!openKeys.contains(QStringLiteral("YT-101")));
        server.close();
    }

    // KB-21: the KB-19 checks (re-fetch of page 1 + same-project numeric boundary gaps)
    // miss a shift that starts on page 2 when the page 2/3 boundary crosses projects.
    // The skipped first issue of page 3 is then reconciled as done.
    void testYouTrackCrossProjectShiftOnLaterPageDoesNotCloseSkippedIssue() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        // Server order (created asc): A-1..A-200, then B-1..B-50. A-150 is closed right
        // after page 2 ($skip=100) has been served, so page 3 starts at B-2 instead of B-1.
        auto page2Served = std::make_shared<bool>(false);
        serveHttp(server, [page2Served](const QByteArray&, const QByteArray& target, const QByteArray&) {
            int skip = 0;
            const int skipIdx = target.indexOf("skip=");
            if (skipIdx >= 0) skip = target.mid(skipIdx + 5).split('&').value(0).toInt();
            QStringList open;
            for (int n = 1; n <= 200; ++n) {
                if (*page2Served && n == 150) continue;
                open.append(QStringLiteral("A-%1").arg(n));
            }
            for (int n = 1; n <= 50; ++n) open.append(QStringLiteral("B-%1").arg(n));
            QJsonArray arr;
            for (int i = skip; i < qMin(skip + 100, static_cast<int>(open.size())); ++i) {
                arr.append(QJsonObject{
                    {QStringLiteral("idReadable"), open.at(i)},
                    {QStringLiteral("summary"), open.at(i)}
                });
            }
            if (skip == 100) *page2Served = true;
            return httpResponse("200 OK", "application/json", QJsonDocument(arr).toJson(QJsonDocument::Compact));
        });

        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(m_testSandbox.filePath(QStringLiteral("kb21_youtrack.db"))));
        TodoListEntry entry;
        entry.id = QStringLiteral("kb21_yt");
        entry.name = QStringLiteral("KB21 YouTrack");
        entry.provider = QStringLiteral("YouTrack");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.apiToken = QStringLiteral("token");
        QVERIFY(db.saveList(entry, 0));
        int order = 0;
        for (int n = 1; n <= 200; ++n) {
            TxtTask t = TxtTask::fromSimpleTxt(QStringLiteral("A-%1").arg(n), false, false);
            t.setJiraKey(QStringLiteral("A-%1").arg(n));
            QVERIFY(db.insertTask(entry.id, t, order++) >= 0);
        }
        for (int n = 1; n <= 50; ++n) {
            TxtTask t = TxtTask::fromSimpleTxt(QStringLiteral("B-%1").arg(n), false, false);
            t.setJiraKey(QStringLiteral("B-%1").arg(n));
            QVERIFY(db.insertTask(entry.id, t, order++) >= 0);
        }

        IssueTrackerService svc;
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);

        QSet<QString> openKeys;
        for (const TxtTask& t : db.loadTasks(entry.id, false)) openKeys.insert(t.jiraKey());
        QVERIFY(openKeys.contains(QStringLiteral("A-150")));   // seen open on page 2
        QVERIFY(openKeys.contains(QStringLiteral("B-1")));
        server.close();
    }

    // Shared scenario for KB-22: 150 open YouTrack issues, local YT-1..YT-150.
    // statefulShift=false reproduces the KB-15 test server (only $skip=100 requests see
    // YT-5 closed). statefulShift=true is a realistic server: YT-5 closes right after the
    // first page-1 request, and every later request sees it closed.
    QSet<QString> runYouTrackShiftScenario(const QString& listId, bool statefulShift) {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            return {};
        }
        auto firstPageServed = std::make_shared<bool>(false);
        serveHttp(server, [statefulShift, firstPageServed](const QByteArray&, const QByteArray& target, const QByteArray&) {
            int skip = 0;
            const int skipIdx = target.indexOf("skip=");
            if (skipIdx >= 0) skip = target.mid(skipIdx + 5).split('&').value(0).toInt();
            const bool closed = statefulShift ? *firstPageServed : (skip == 100);
            QVector<int> open;
            for (int n = 1; n <= 150; ++n) {
                if (closed && n == 5) continue;
                open.append(n);
            }
            QJsonArray arr;
            for (int i = skip; i < qMin(skip + 100, static_cast<int>(open.size())); ++i) {
                arr.append(QJsonObject{
                    {QStringLiteral("idReadable"), QStringLiteral("YT-%1").arg(open.at(i))},
                    {QStringLiteral("summary"), QStringLiteral("Issue %1").arg(open.at(i))}
                });
            }
            if (skip == 0) *firstPageServed = true;
            return httpResponse("200 OK", "application/json", QJsonDocument(arr).toJson(QJsonDocument::Compact));
        });

        DatabaseManager& db = DatabaseManager::instance();
        db.close();
        const QString dbPath = m_testSandbox.filePath(listId + QStringLiteral(".db"));
        QFile::remove(dbPath);
        if (!db.initialize(dbPath)) return {};
        TodoListEntry entry;
        entry.id = listId;
        entry.name = listId;
        entry.provider = QStringLiteral("YouTrack");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.apiToken = QStringLiteral("token");
        if (!db.saveList(entry, 0)) return {};
        for (int n = 1; n <= 150; ++n) {
            TxtTask t = TxtTask::fromSimpleTxt(QStringLiteral("Issue %1").arg(n), false, false);
            t.setJiraKey(QStringLiteral("YT-%1").arg(n));
            if (db.insertTask(entry.id, t, n - 1) < 0) return {};
        }

        IssueTrackerService svc;
        svc.syncListIssues(entry);
        if (!QTest::qWaitFor([&svc]() { return !svc.isSyncing(); }, 5000)) return {};

        QSet<QString> openKeys;
        for (const TxtTask& t : db.loadTasks(entry.id, false)) openKeys.insert(t.jiraKey());
        server.close();
        db.close();
        return openKeys;
    }

    // KB-22: production code special-cases the test list id "kb15_yt" in
    // fetchYouTrackPage. Identical server responses must give the same result whatever
    // the list id is.
    void testYouTrackSyncOutcomeDoesNotDependOnListId() {
        const QSet<QString> withTestId = runYouTrackShiftScenario(QStringLiteral("kb15_yt"), false);
        const QSet<QString> withOtherId = runYouTrackShiftScenario(QStringLiteral("kb22_yt"), false);
        if (withTestId.isEmpty() || withOtherId.isEmpty()) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        QCOMPARE(withOtherId, withTestId);
    }

    // KB-22 (companion): with a realistic, stateful server the generic two-pass check
    // detects the shift on its own, so the "kb15_yt" hack is not needed. This passes now.
    void testYouTrackTwoPassDetectsRealisticShift() {
        const QSet<QString> openKeys = runYouTrackShiftScenario(QStringLiteral("kb22_yt_stateful"), true);
        if (openKeys.isEmpty()) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        QVERIFY(openKeys.contains(QStringLiteral("YT-101")));
        QVERIFY(openKeys.contains(QStringLiteral("YT-5")));
    }

    // KB-23: TaskManager::loadFromDatabase inserts the three onboarding tasks whenever a
    // local list has zero tasks. setActiveList is called with addDefaultTodosIfNew = true
    // on every selection of a local list, so a user who deletes all tasks gets the
    // defaults back the next time the list is opened.
    void testDefaultTasksNotReaddedAfterUserEmptiesList() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb23_defaults.db")));
        const QString listId = ctrl.listManager()->addList(QStringLiteral("KB23 Local"));
        QVERIFY(!listId.isEmpty());
        ctrl.selectListById(listId);
        const int initial = ctrl.todoProxyModel()->rowCount();
        QVERIFY(initial > 0);   // onboarding tasks for a brand-new list are fine

        while (ctrl.todoProxyModel()->rowCount() > 0) {
            QVERIFY(ctrl.removeTask(false, 0));
        }
        QCOMPARE(DatabaseManager::instance().countAllTasksInList(listId), 0);

        ctrl.selectListById(listId);   // user re-opens the now empty list
        QCOMPARE(DatabaseManager::instance().countAllTasksInList(listId), 0);
    }

    // KB-24: links from tracker-controlled text (task details / comments rendered as
    // Markdown in TaskDetailsDialog.qml) reach Qt.openUrlExternally / app.openUrlExternally
    // without the http/https/mailto whitelist that TxtTask::isSafeUriScheme applies to
    // task rows. AppController::openUrlExternally must refuse other schemes.
    void testOpenUrlExternallyRejectsUnsafeSchemes() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb24_urls.db")));
        UrlOpenSpy spy;
        const QStringList schemes = {QStringLiteral("file"), QStringLiteral("smb"), QStringLiteral("https")};
        for (const QString& scheme : schemes) {
            QDesktopServices::setUrlHandler(scheme, &spy, "handle");
        }
        ctrl.openUrlExternally(QUrl(QStringLiteral("https://example.com/issue/1")));
        ctrl.openUrlExternally(QUrl(QStringLiteral("file:///etc/passwd")));
        ctrl.openUrlExternally(QUrl(QStringLiteral("smb://attacker.example/share")));
        for (const QString& scheme : schemes) {
            QDesktopServices::unsetUrlHandler(scheme);
        }

        QVERIFY(spy.opened.contains(QUrl(QStringLiteral("https://example.com/issue/1"))));
        QCOMPARE(spy.opened.size(), 1);
    }

    // KB-25: relocateDatabase writes Storage/databasePath to settings.ini and lets
    // initialize() close the current connection before the new database is known to open.
    // A failed relocation leaves the app without a database and persists the broken path
    // for the next start.
    void testFailedDatabaseRelocationKeepsCurrentDatabase() {
        DatabaseManager& db = DatabaseManager::instance();
        const QString goodPath = m_testSandbox.filePath(QStringLiteral("kb25_current.db"));
        QVERIFY(db.initialize(goodPath));

        QSettings boot(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("go-for-it-qt"), QStringLiteral("settings"));
        const QVariant previousSetting = boot.value(QStringLiteral("Storage/databasePath"));

        // A path below a regular file can never be created or opened.
        const QString blocker = m_testSandbox.filePath(QStringLiteral("kb25_blocker"));
        {
            QFile f(blocker);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
        const QString badTarget = blocker + QStringLiteral("/sub/goforit.db");

        QVERIFY(!db.relocateDatabase(badTarget, false));

        boot.sync();
        const QVariant afterSetting = boot.value(QStringLiteral("Storage/databasePath"));
        const bool stillOpen = db.isOpen() && db.databasePath() == goodPath;

        // Restore global state for the following tests before asserting.
        if (previousSetting.isValid()) boot.setValue(QStringLiteral("Storage/databasePath"), previousSetting);
        else boot.remove(QStringLiteral("Storage/databasePath"));
        boot.sync();

        QVERIFY(stillOpen);
        QCOMPARE(afterSetting, previousSetting);
    }

    // KB-26 guard: 6803fd5 relaxed testJiraBearerAuthRetryWithLocalServer to
    // requestCount >= 2, so it no longer catches a Jira auth-retry loop. When both Basic
    // and Bearer are rejected, exactly one Basic and one Bearer attempt are expected,
    // followed by a single syncFailed and no further requests.
    void testJiraAuthRetryIsBoundedWhenBothSchemesFail() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        int basicCount = 0;
        int bearerCount = 0;
        int otherCount = 0;
        serveHttpFull(server, [&basicCount, &bearerCount, &otherCount](const QByteArray&, const QByteArray&, const QByteArray& head, const QByteArray&) {
            const QByteArray lower = head.toLower();
            if (lower.contains("authorization: basic")) ++basicCount;
            else if (lower.contains("authorization: bearer")) ++bearerCount;
            else ++otherCount;
            return httpResponse("401 Unauthorized", "application/json", "{}",
                                "X-Seraph-Loginreason: AUTHENTICATED_FAILED\r\n");
        });

        IssueTrackerService svc;
        TodoListEntry entry;
        entry.id = QStringLiteral("kb26_jira");
        entry.name = QStringLiteral("KB26 Jira");
        entry.provider = QStringLiteral("Jira");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user@example.com");
        entry.apiToken = QStringLiteral("token");

        QSignalSpy failSpy(&svc, &IssueTrackerService::syncFailed);
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 30000);
        QTest::qWait(300);   // let any stray retry reach the server

        QCOMPARE(basicCount, 1);
        QCOMPARE(bearerCount, 1);
        QCOMPARE(otherCount, 0);
        QCOMPARE(failSpy.count(), 1);
        server.close();
    }

    // KB-27: mutating a row that no longer exists must not be reported as a
    // successful persistence operation. Callers otherwise keep stale in-memory
    // list state after an external deletion or race.
    void testListMutationsRejectMissingRows() {
        DatabaseManager& db = DatabaseManager::instance();
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb27_missing_lists.db"));
        QFile::remove(dbPath);
        QVERIFY(db.initialize(dbPath));

        TodoListEntry entry;
        entry.id = QStringLiteral("kb27-list");
        entry.name = QStringLiteral("KB27 list");
        QVERIFY(db.saveList(entry, 0));
        QVERIFY(db.removeList(entry.id));

        const bool missingDeleteRejected = !db.removeList(entry.id);
        const bool missingMappingUpdateRejected =
            !db.updateListStatusMapping(entry.id, {{QStringLiteral("todo"), QStringLiteral("Open")}});
        const bool missingOnboardingUpdateRejected = !db.setOnboardingCompleted(entry.id);

        QVERIFY(missingDeleteRejected && missingMappingUpdateRejected && missingOnboardingUpdateRejected);
    }

    // KB-28: since c816a12 removeList() returns false when the row is already gone, and
    // ListManagerModel::removeListById then keeps the entry in memory. A list deleted by
    // another process on the same database (GUI and TUI side by side) can no longer be
    // removed from the UI until restart.
    void testRemovingListAlreadyDeletedElsewhereDropsItFromModel() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb28_ghost_list.db")));
        ListManagerModel* lm = ctrl.listManager();
        const QString listId = lm->addList(QStringLiteral("KB28 Ghost"));
        QVERIFY(!listId.isEmpty());
        const int before = lm->rowCount();

        // Another process deletes the list directly in the shared database.
        QVERIFY(DatabaseManager::instance().removeList(listId));

        lm->removeListById(listId);

        QCOMPARE(lm->indexOfId(listId), -1);
        QCOMPARE(lm->rowCount(), before - 1);
    }

    // KB-30: billing exports contain task names, tracked time and list/client names. The
    // TUI writes them to predictable world-writable paths (/tmp/billing-<period>.csv|pdf,
    // /tmp/billing-report.csv|pdf) and AnalyticsService creates the files with the default
    // umask, so on a typical 022 umask any local user can read the report.
    void testBillingExportFilesAreOwnerOnly() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb30_billing.db")));
        const QString listId = ctrl.listManager()->addList(QStringLiteral("KB30 Client"));
        ctrl.selectListById(listId);

        const mode_t oldMask = ::umask(022);
        const QString csvPath = m_testSandbox.filePath(QStringLiteral("kb30-billing.csv"));
        const QString pdfPath = m_testSandbox.filePath(QStringLiteral("kb30-billing.pdf"));
        QFile::remove(csvPath);
        QFile::remove(pdfPath);
        const bool csvOk = ctrl.analytics()->exportBillingCsv(csvPath, QStringLiteral("all"), listId);
        const bool pdfOk = ctrl.analytics()->exportBillingPdf(pdfPath, QStringLiteral("all"), listId);
        ::umask(oldMask);

        QVERIFY(csvOk);
        QVERIFY(pdfOk);
        const QFileDevice::Permissions othersOrGroup =
            QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther;
        QVERIFY((QFile::permissions(csvPath) & othersOrGroup) == 0);
        QVERIFY((QFile::permissions(pdfPath) & othersOrGroup) == 0);
    }

    // KB-31: exportBillingPdf (since 27dc901) ignores a failed QFile::rename/QFile::copy
    // of the temporary PDF, deletes the temporary file and still returns true. The UI
    // and TUI then report "exported" although no report exists. Reproduced with a target
    // path that is an existing directory (also: read-only target, vanished directory).
    void testBillingPdfExportReportsFailureWhenTargetCannotBeWritten() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb31_billing.db")));
        const QString listId = ctrl.listManager()->addList(QStringLiteral("KB31 Client"));
        ctrl.selectListById(listId);

        const QString dirTarget = m_testSandbox.filePath(QStringLiteral("kb31-target-is-a-dir.pdf"));
        QVERIFY(QDir().mkpath(dirTarget));

        const bool csvOk = ctrl.analytics()->exportBillingCsv(dirTarget, QStringLiteral("all"), listId);
        QVERIFY(!csvOk);   // CSV export already reports this failure correctly

        const bool pdfOk = ctrl.analytics()->exportBillingPdf(dirTarget, QStringLiteral("all"), listId);
        QVERIFY(QFileInfo(dirTarget).isDir());   // nothing was written over the directory
        QVERIFY(!pdfOk);
    }

    // KB-32: TxtTask::updateFromSimpleTxt resets priority and duration but keeps the old
    // m_dueDate. Removing "due:YYYY-MM-DD" from a task's text (UI edit, TUI edit,
    // TaskStoreModel::setData(DescriptionRole)) leaves the task overdue / due today until
    // the list is reloaded, so the overdue badge, smart filters and due reminders
    // (AppController::checkDueReminders) still fire for a task that no longer has a due date.
    void testEditingTextRemovesDueDate() {
        TxtTask task = TxtTask::fromSimpleTxt(QStringLiteral("Pay bills due:2020-01-01"), false, false);
        QVERIFY(task.dueDate().isValid());
        QVERIFY(task.isOverdue(QDate(2026, 1, 1)));

        task.updateFromSimpleTxt(QStringLiteral("Pay bills"));
        QCOMPARE(task.description(), QStringLiteral("Pay bills"));
        QVERIFY(!task.dueDate().isValid());
        QVERIFY(!task.isOverdue(QDate(2026, 1, 1)));
    }

    // KB-34: the per-list activity log CSV (TodoListEntry::activityLogPath, written on
    // every timer stop by ActivityLog::logTaskInFile) holds the same data as the billing
    // reports fixed in KB-30 (list name, task description, tracked time, timestamps) but
    // is still created with the process umask, i.e. 0644 / world-readable.
    void testActivityLogCsvIsOwnerOnly() {
        const QString logPath = m_testSandbox.filePath(QStringLiteral("kb34/activity.csv"));
        QFile::remove(logPath);
        const mode_t oldMask = ::umask(022);
        const QDateTime stop = QDateTime::currentDateTimeUtc();
        const bool ok = ActivityLog::logTaskInFile(logPath, QStringLiteral("KB34 Client"),
                                                   QStringLiteral("Confidential task"),
                                                   stop.addSecs(-60), 60, stop);
        ::umask(oldMask);
        QVERIFY(ok);
        const QFileDevice::Permissions othersOrGroup =
            QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther;
        QVERIFY((QFile::permissions(logPath) & othersOrGroup) == 0);
    }

    // KB-33: notification bodies and titles must be escaped before being sent to
    // org.freedesktop.Notifications to prevent malicious markup or injection from
    // remote task descriptions and tracker issue titles.
    void testNotificationPayloadEscapesMarkup() {
        const QString rawTitle = QStringLiteral("Alert <script> & 'check'");
        const QString rawBody = QStringLiteral("Click <a href=\"x\">here</a> & win");

        QCOMPARE(NotificationService::notifyPayloadTitle(rawTitle), rawTitle);
        QCOMPARE(NotificationService::notifyPayloadBody(rawBody),
                 QStringLiteral("Click &lt;a href=&quot;x&quot;&gt;here&lt;/a&gt; &amp; win"));
    }

    // KB-35: the Desktop Notifications spec defines the summary (title) as plain text;
    // only the body may contain markup. 79cc64d escapes the title too, so list and
    // task names with '&', '<', '>' or '"' (e.g. "Updates in 'R&D'") are displayed with
    // literal entities ("R&amp;D") by GNOME Shell, KDE Plasma, dunst, etc.
    void testNotificationTitleIsNotHtmlEscaped() {
        const QString title = QStringLiteral("Updates in 'R&D <beta>'");
        QCOMPARE(NotificationService::notifyPayloadTitle(title), title);
        // The body must stay escaped (KB-33).
        QCOMPARE(NotificationService::notifyPayloadBody(QStringLiteral("<a href=\"x\">R&D</a>")),
                 QStringLiteral("&lt;a href=&quot;x&quot;&gt;R&amp;D&lt;/a&gt;"));
    }

    // KB-36: AnalyticsService::getProductivityStats aggregates the task breakdown by
    // jiraKey, or by description when there is no key, ignoring the list. In an all-lists
    // report (listId empty: "All Lists" view, TUI dashboard without a selected list),
    // identically named tasks of different lists/clients ("Code review" for client A and B)
    // or identical keys from two trackers ("PROJ-1") collapse into one row that carries the
    // list name of whichever record came last. The billing CSV/PDF then bills the summed
    // time to a single client.
    void testBillingBreakdownKeepsTasksOfDifferentListsApart() {
        DatabaseManager& db = DatabaseManager::instance();
        QVERIFY(db.initialize(m_testSandbox.filePath(QStringLiteral("kb36_breakdown.db"))));
        ActivityLog log;
        const QDateTime t0 = QDateTime::currentDateTimeUtc().addSecs(-7200);
        QVERIFY(log.logTask(QStringLiteral("kb36_a"), QStringLiteral("Client A"), -1, QString(),
                            QStringLiteral("Code review"), t0, 1800, t0.addSecs(1800)) >= 0);
        QVERIFY(log.logTask(QStringLiteral("kb36_b"), QStringLiteral("Client B"), -1, QString(),
                            QStringLiteral("Code review"), t0.addSecs(3600), 600, t0.addSecs(4200)) >= 0);

        AnalyticsService analytics;
        const QVariantList rows = analytics.getProductivityStats(QStringLiteral("all"), QString())
                                      .value(QStringLiteral("taskBreakdown")).toList();
        QSet<QString> lists;
        for (const QVariant& r : rows) lists.insert(r.toMap().value(QStringLiteral("listName")).toString());

        QCOMPARE(rows.size(), 2);
        QVERIFY(lists.contains(QStringLiteral("Client A")) && lists.contains(QStringLiteral("Client B")));
    }

    // KB-37: per CLAUDE.md every code path that writes to a remote tracker must be a no-op
    // under QStandardPaths::isTestModeEnabled(). updateIssueStatusRemote has the guard, but
    // postWorklog, updateIssueDescription and postIssueComment do not, so tests (and any
    // test-mode run) send real POST/PUT requests: AppController::onTimerStopped ->
    // postWorklog, updateTaskDetails -> updateIssueDescription, addTaskComment ->
    // postIssueComment. Lists in tests point at hosts such as jira.example.com, which resolve.
    void testTrackerWritesAreNoOpsInTestMode() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        int requests = 0;
        serveHttpFull(server, [&requests](const QByteArray&, const QByteArray&, const QByteArray&, const QByteArray&) {
            ++requests;
            return httpResponse("201 Created", "application/json", "{}");
        });
        QVERIFY(QStandardPaths::isTestModeEnabled());

        TodoListEntry entry;
        entry.id = QStringLiteral("kb37_jira");
        entry.name = QStringLiteral("KB37 Jira");
        entry.provider = QStringLiteral("Jira");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user@example.com");
        entry.apiToken = QStringLiteral("token");
        entry.autoWorklog = true;

        IssueTrackerService svc;
        const QDateTime start = QDateTime::currentDateTimeUtc().addSecs(-1500);
        svc.postWorklog(entry, 1, 1, QStringLiteral("KB-1"), start, 1500);
        svc.updateIssueDescription(entry, QStringLiteral("KB-1"), QStringLiteral("new details"));
        svc.postIssueComment(entry, QStringLiteral("KB-1"), QStringLiteral("a comment"));
        QTest::qWait(500);

        QCOMPARE(requests, 0);
        server.close();
    }

    // KB-38: the TUI edit commands (cw/cc/i/e in TuiApp::handleNormalKey) pre-fill the
    // input line with DescriptionRole, which excludes the "(A)" priority and the
    // "duration:" token, and save it via AppController::updateTaskText ->
    // TxtTask::updateFromSimpleTxt, which resets priority and duration from the text.
    // Opening the editor and pressing Enter without any change therefore silently drops
    // the task's priority and estimate. The GUI pre-fills SimpleTextRole and keeps both.
    void testTuiStyleTitleEditKeepsPriorityAndDuration() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb38_tui_edit.db")));
        const QString listId = ctrl.listManager()->addList(QStringLiteral("KB38"));
        ctrl.selectListById(listId);
        while (ctrl.todoProxyModel()->rowCount() > 0) QVERIFY(ctrl.removeTask(false, 0));
        ctrl.addNewTask(QStringLiteral("(A) Write report duration:30m"));
        QCOMPARE(ctrl.todoProxyModel()->rowCount(), 1);

        auto* proxy = ctrl.todoProxyModel();
        const QModelIndex idx = proxy->index(0, 0);
        QCOMPARE(proxy->data(idx, TaskStoreModel::PriorityRole).toInt(), 0);
        QCOMPARE(proxy->data(idx, TaskStoreModel::DurationRole).toUInt(), 1800u);

        // Exactly what TuiApp does for "cw" followed by Enter without edits:
        const QString prefilled = proxy->data(idx, TaskStoreModel::SimpleTextRole).toString();
        QVERIFY(ctrl.updateTaskText(false, 0, prefilled));

        const QModelIndex after = proxy->index(0, 0);
        QCOMPARE(proxy->data(after, TaskStoreModel::PriorityRole).toInt(), 0);
        QCOMPARE(proxy->data(after, TaskStoreModel::DurationRole).toUInt(), 1800u);
    }

    // KB-39: TuiApp::onStdinActivated reads stdin in 128-byte chunks and decodes each
    // chunk on its own, then feeds the text to the key handlers one QChar at a time:
    //  - a multi-byte UTF-8 character split across two reads (pasting a longer Cyrillic
    //    title) turns into U+FFFD replacement characters;
    //  - characters outside the BMP (emoji such as U+1F4DD) are split into lone UTF-16
    //    surrogates, which QString::toUtf8() turns into '?'.
    // The test drives the real TuiApp through a pipe on stdin: "a" (insert mode),
    // the title, Enter.
    void testTuiInputKeepsMultibyteAndNonBmpCharacters() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb39_tui_input.db")));
        const QString listId = ctrl.listManager()->addList(QStringLiteral("KB39"));
        ctrl.selectListById(listId);
        while (ctrl.todoProxyModel()->rowCount() > 0) QVERIFY(ctrl.removeTask(false, 0));

        int fds[2];
        QCOMPARE(::pipe(fds), 0);
        const int savedStdin = ::dup(STDIN_FILENO);
        const int savedStdout = ::dup(STDOUT_FILENO);
        const int devNull = ::open("/dev/null", O_WRONLY);
        QVERIFY(savedStdin >= 0 && savedStdout >= 0 && devNull >= 0);
        ::dup2(fds[0], STDIN_FILENO);
        ::dup2(devNull, STDOUT_FILENO);   // TuiApp::render() writes ANSI frames to stdout

        // 127 ASCII bytes push the 2-byte Cyrillic "Ж" across the 128-byte read boundary.
        const QString title = QString(127, QLatin1Char('x')) + QStringLiteral("Ж звіт ")
                              + QString::fromUcs4(U"\U0001F4DD");
        QStringList added;
        {
            TuiApp tui(&ctrl);
            auto feed = [&](const QByteArray& bytes) {
                QCOMPARE(::write(fds[1], bytes.constData(), static_cast<size_t>(bytes.size())),
                         static_cast<ssize_t>(bytes.size()));
                // Drain everything currently in the pipe, one read per activation.
                for (int i = 0; i < 8; ++i) {
                    int pending = 0;
                    ::ioctl(STDIN_FILENO, FIONREAD, &pending);
                    if (pending <= 0) break;
                    QMetaObject::invokeMethod(&tui, "onStdinActivated", Qt::DirectConnection);
                }
            };
            feed("a");
            feed(title.toUtf8());
            feed("\r");
            for (int r = 0; r < ctrl.todoProxyModel()->rowCount(); ++r) {
                added << ctrl.todoProxyModel()->data(ctrl.todoProxyModel()->index(r, 0),
                                                     TaskStoreModel::DescriptionRole).toString();
            }
        }
        std::fflush(stdout);
        ::dup2(savedStdin, STDIN_FILENO);
        ::dup2(savedStdout, STDOUT_FILENO);
        ::close(savedStdin);
        ::close(savedStdout);
        ::close(devNull);
        ::close(fds[0]);
        ::close(fds[1]);

        QCOMPARE(added.size(), 1);
        QCOMPARE(added.first(), title);
    }

    // KB-40: TuiApp::onStdinActivated treats a read that starts with ESC as one single key
    // sequence and otherwise decodes it character by character, so an escape sequence that
    // shares a read with ordinary characters (fast typing, key repeat, SSH/tmux coalescing)
    // is mangled:
    //  - "\033[DX" (Left, then 'X') is passed as one unknown sequence: the cursor does not
    //    move and 'X' is lost;
    //  - "Y\033[D" sends 'Y', then a lone "\033" (cancels the input line), then '[' and 'D'.
    void testTuiSplitsEscapeSequencesFromTypedText() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb40_tui_esc.db")));
        const QString listId = ctrl.listManager()->addList(QStringLiteral("KB40"));
        ctrl.selectListById(listId);
        while (ctrl.todoProxyModel()->rowCount() > 0) QVERIFY(ctrl.removeTask(false, 0));

        int fds[2];
        QCOMPARE(::pipe(fds), 0);
        const int savedStdin = ::dup(STDIN_FILENO);
        const int savedStdout = ::dup(STDOUT_FILENO);
        const int devNull = ::open("/dev/null", O_WRONLY);
        QVERIFY(savedStdin >= 0 && savedStdout >= 0 && devNull >= 0);
        ::dup2(fds[0], STDIN_FILENO);
        ::dup2(devNull, STDOUT_FILENO);

        // With the bug, the stray keys reach normal mode and 'C' opens the comment editor
        // ($VISUAL/$EDITOR) on our stdin; make that editor a no-op so the test cannot hang.
        const QByteArray oldVisual = qgetenv("VISUAL");
        const QByteArray oldEditor = qgetenv("EDITOR");
        qputenv("VISUAL", "true");
        qputenv("EDITOR", "true");

        QStringList added;
        {
            TuiApp tui(&ctrl);
            auto feed = [&](const QByteArray& bytes) {
                QCOMPARE(::write(fds[1], bytes.constData(), static_cast<size_t>(bytes.size())),
                         static_cast<ssize_t>(bytes.size()));
                for (int i = 0; i < 8; ++i) {
                    int pending = 0;
                    ::ioctl(STDIN_FILENO, FIONREAD, &pending);
                    if (pending <= 0) break;
                    QMetaObject::invokeMethod(&tui, "onStdinActivated", Qt::DirectConnection);
                }
            };
            feed("a");             // insert mode
            feed("ab");
            feed("\033[DX");       // Left + 'X' in one read  -> expected "aXb"
            feed("Y\033[C");       // 'Y' + Right in one read -> expected "aXYb"
            feed("\r");
            for (int r = 0; r < ctrl.todoProxyModel()->rowCount(); ++r) {
                added << ctrl.todoProxyModel()->data(ctrl.todoProxyModel()->index(r, 0),
                                                     TaskStoreModel::DescriptionRole).toString();
            }
        }
        if (oldVisual.isNull()) qunsetenv("VISUAL"); else qputenv("VISUAL", oldVisual);
        if (oldEditor.isNull()) qunsetenv("EDITOR"); else qputenv("EDITOR", oldEditor);
        std::fflush(stdout);
        ::dup2(savedStdin, STDIN_FILENO);
        ::dup2(savedStdout, STDOUT_FILENO);
        ::close(savedStdin);
        ::close(savedStdout);
        ::close(devNull);
        ::close(fds[0]);
        ::close(fds[1]);

        QCOMPARE(added, QStringList{QStringLiteral("aXYb")});
    }

    // KB-41: IssueTrackerService::mapPriorityName checks "low" before "lowest", so the
    // standard Jira/YouTrack priority "Lowest" maps to (D), the same as "Low", and the
    // (E) branch for "lowest" is unreachable. Remote "Lowest" issues are sorted and
    // filtered as if they were "Low".
    void testLowestRemotePriorityMapsToE() {
        QCOMPARE(IssueTrackerService::mapPriorityName(QStringLiteral("Highest")), quint8(0));
        QCOMPARE(IssueTrackerService::mapPriorityName(QStringLiteral("High")), quint8(1));
        QCOMPARE(IssueTrackerService::mapPriorityName(QStringLiteral("Medium")), quint8(2));
        QCOMPARE(IssueTrackerService::mapPriorityName(QStringLiteral("Low")), quint8(3));
        QCOMPARE(IssueTrackerService::mapPriorityName(QStringLiteral("Trivial")), quint8(4));
        QCOMPARE(IssueTrackerService::mapPriorityName(QStringLiteral("Lowest")), quint8(4));
    }

    // KB-42: IssueTrackerService::postWorklog only handles QNetworkReply::NoError. When the
    // tracker rejects or cannot be reached (expired token -> 401, server error -> 5xx,
    // offline), nothing is reported (no syncFailed, no status message) and nothing retries
    // later: activity_log.worklog_synced stays 0 and is never read again. Tracked time is
    // silently missing from Jira/YouTrack/GitHub/GitLab.
    void testFailedWorklogIsReported() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        int requests = 0;
        serveHttpFull(server, [&requests](const QByteArray&, const QByteArray&, const QByteArray&, const QByteArray&) {
            ++requests;
            return httpResponse("500 Internal Server Error", "application/json", "{\"errorMessages\":[\"boom\"]}");
        });

        TodoListEntry entry;
        entry.id = QStringLiteral("kb42_jira");
        entry.name = QStringLiteral("KB42 Jira");
        entry.provider = QStringLiteral("Jira");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user@example.com");
        entry.apiToken = QStringLiteral("token");
        entry.autoWorklog = true;

        IssueTrackerService svc;
        QSignalSpy failSpy(&svc, &IssueTrackerService::syncFailed);
        // postWorklog is a no-op in test mode (KB-37); lift it only for this local request.
        QStandardPaths::setTestModeEnabled(false);
        svc.postWorklog(entry, 1, 1, QStringLiteral("KB-1"), QDateTime::currentDateTimeUtc().addSecs(-1500), 1500);
        QStandardPaths::setTestModeEnabled(true);

        QTRY_VERIFY_WITH_TIMEOUT(requests >= 1, 5000);
        QTest::qWait(300);
        QCOMPARE(failSpy.count(), 1);
        server.close();
    }

    // KB-43: AmbientSoundService::setAmbientMode generates and plays
    // "/tmp/gofi_ambient_<mode>.wav", a predictable name in the shared world-writable /tmp,
    // and reuses an existing file at that path without checking its owner or content.
    // Another local user can pre-create /tmp/gofi_ambient_rain.wav (any audio, or a
    // malformed file fed to the media decoder) and the victim's app plays it; the generated
    // files are also shared between all users. The file is created even in test mode.
    // Expected: generate into a per-user location (CacheLocation/AppDataLocation).
    void testAmbientSoundIsNotGeneratedInSharedTmp() {
        const QString sharedPath = QStringLiteral("/tmp/gofi_ambient_kb43probe.wav");
        QFile::remove(sharedPath);
        {
            AmbientSoundService ambient(nullptr);
            ambient.setAmbientMode(QStringLiteral("kb43probe"));
        }
        const bool createdInSharedTmp = QFile::exists(sharedPath);
        QFile::remove(sharedPath);
        QVERIFY(!createdInSharedTmp);
    }

    // KB-44: Jira sync retries with Bearer auth when Basic is rejected
    // (fetchJiraPage -> fetchJiraIssues(..., forceBearerAuth=true)), but postWorklog always
    // sends Basic (applyAuthHeader(req, listEntry)) and has no fallback. For Jira Server/DC
    // personal access tokens with a user/email filled in, the list syncs fine while every
    // worklog fails with 401, so tracked time never reaches Jira.
    void testJiraWorklogFallsBackToBearerAuth() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        int basic = 0;
        int bearer = 0;
        serveHttpFull(server, [&basic, &bearer](const QByteArray&, const QByteArray&, const QByteArray& head, const QByteArray&) {
            const QByteArray lower = head.toLower();
            if (lower.contains("authorization: bearer")) {
                ++bearer;
                return httpResponse("201 Created", "application/json", "{\"id\":\"1\"}");
            }
            ++basic;
            return httpResponse("401 Unauthorized", "application/json", "{}",
                                "X-Seraph-Loginreason: AUTHENTICATED_FAILED\r\n");
        });

        TodoListEntry entry;
        entry.id = QStringLiteral("kb44_jira");
        entry.name = QStringLiteral("KB44 Jira");
        entry.provider = QStringLiteral("Jira");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user@example.com");
        entry.apiToken = QStringLiteral("pat-token");
        entry.autoWorklog = true;

        IssueTrackerService svc;
        QSignalSpy postedSpy(&svc, &IssueTrackerService::worklogPosted);
        QStandardPaths::setTestModeEnabled(false);
        // Keep test mode off for the whole exchange: the Bearer retry is issued from the
        // reply handler, after the first request returns. (Re-enabling test mode right after
        // the first call made the retry hit the test-mode guard; see KB-47.)
        svc.postWorklog(entry, 1, 1, QStringLiteral("KB-1"), QDateTime::currentDateTimeUtc().addSecs(-1500), 1500);
        QTRY_VERIFY_WITH_TIMEOUT(postedSpy.count() >= 1 || bearer >= 1, 5000);
        QTest::qWait(300);
        QStandardPaths::setTestModeEnabled(true);

        QCOMPARE(basic, 1);
        QCOMPARE(bearer, 1);
        QCOMPARE(postedSpy.count(), 1);
        server.close();
    }

    // KB-45: GitHub and GitLab labels are run through IssueTrackerService::mapPriorityName,
    // which matches by substring (contains()). Very common non-priority labels therefore
    // assign a priority: GitLab scoped labels "workflow::…" and "follow-up" / "slow-query"
    // contain "low" -> (D); "high-availability" / "highlight" contain "high" -> (B);
    // "normalization" contains "normal" -> (C). The first such label wins, so e.g. every
    // MR with a workflow label gets priority (D) and sorts/filters wrongly.
    void testNonPriorityLabelsDoNotMapToPriority() {
        // Real priority names must keep working.
        QCOMPARE(IssueTrackerService::mapPriorityName(QStringLiteral("priority::high")), quint8(1));
        QCOMPARE(IssueTrackerService::mapPriorityName(QStringLiteral("Low")), quint8(3));

        const QStringList unrelated = {
            QStringLiteral("workflow::in review"),
            QStringLiteral("follow-up"),
            QStringLiteral("slow-query"),
            QStringLiteral("high-availability"),
            QStringLiteral("normalization")
        };
        QStringList wronglyMapped;
        for (const QString& label : unrelated) {
            if (IssueTrackerService::mapPriorityName(label) != TxtTask::NoPriority) wronglyMapped << label;
        }
        QCOMPARE(wronglyMapped, QStringList());
    }

    // KB-46: IssueTrackerService::mapRemoteStatusToLocal (used when the list has no explicit
    // status mapping) also matches by substring, so negated or unrelated status names land
    // in the opposite bucket: "Incomplete" contains "complete" -> done (the task is moved to
    // the done list on every sync), "Inactive" contains "active" -> in_progress, "Unblocked"
    // contains "block" -> paused, "Not Done" contains "done" -> done.
    void testNegatedRemoteStatusesAreNotMisclassified() {
        const QVariantMap noMapping;
        QCOMPARE(IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("Done"), QString(), noMapping), QStringLiteral("done"));
        QCOMPARE(IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("In Progress"), QString(), noMapping), QStringLiteral("in_progress"));

        QStringList wrong;
        if (IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("Incomplete"), QString(), noMapping) == QStringLiteral("done")) wrong << QStringLiteral("Incomplete->done");
        if (IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("Not Done"), QString(), noMapping) == QStringLiteral("done")) wrong << QStringLiteral("Not Done->done");
        if (IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("Inactive"), QString(), noMapping) == QStringLiteral("in_progress")) wrong << QStringLiteral("Inactive->in_progress");
        if (IssueTrackerService::mapRemoteStatusToLocal(QStringLiteral("Unblocked"), QString(), noMapping) == QStringLiteral("paused")) wrong << QStringLiteral("Unblocked->paused");
        QCOMPARE(wrong, QStringList());
    }

    // KB-47: 82b5093 changed postWorklog's test-mode guard to
    // `if (!forceBearerAuth && QStandardPaths::isTestModeEnabled()) return;`, so any call
    // with forceBearerAuth = true (the internal Bearer retry, or a direct caller) sends a
    // real worklog in test mode again, undoing KB-37. The guard was relaxed only because
    // the KB-44 test re-enabled test mode before the retry ran; that test is now fixed to
    // keep test mode off for the whole exchange, so the guard can be unconditional again.
    void testForcedBearerWorklogIsNoOpInTestMode() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        int requests = 0;
        serveHttpFull(server, [&requests](const QByteArray&, const QByteArray&, const QByteArray&, const QByteArray&) {
            ++requests;
            return httpResponse("201 Created", "application/json", "{}");
        });
        QVERIFY(QStandardPaths::isTestModeEnabled());

        TodoListEntry entry;
        entry.id = QStringLiteral("kb47_jira");
        entry.name = QStringLiteral("KB47 Jira");
        entry.provider = QStringLiteral("Jira");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user@example.com");
        entry.apiToken = QStringLiteral("token");
        entry.autoWorklog = true;

        IssueTrackerService svc;
        svc.postWorklog(entry, 1, 1, QStringLiteral("KB-1"), QDateTime::currentDateTimeUtc().addSecs(-1500), 1500, true);
        QTest::qWait(400);
        QCOMPARE(requests, 0);
        server.close();
    }

    // KB-48: 82b5093 remembers Jira lists that needed Bearer auth in m_jiraBearerLists and
    // never forgets them for the life of the process. fetchJiraIssues inserts the list as
    // soon as a Bearer retry *starts* (before knowing whether Bearer works), so a single
    // rejected sync (wrong or expired token) pins the list to Bearer. On Jira Cloud, where
    // API tokens only work with Basic email:token, correcting the token in the list dialog
    // does not help: every later sync, worklog, comment and description push is sent with
    // Bearer and fails until the app is restarted.
    void testJiraBearerModeIsNotStickyAfterCredentialFix() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        // Phase 1: wrong token, everything rejected. Phase 2: "Cloud" accepts Basic only.
        auto phase = std::make_shared<int>(1);
        serveHttpFull(server, [phase](const QByteArray&, const QByteArray&, const QByteArray& head, const QByteArray&) {
            const QByteArray lower = head.toLower();
            if (*phase == 2 && lower.contains("authorization: basic")) {
                return httpResponse("200 OK", "application/json", "{\"issues\":[],\"total\":0}");
            }
            return httpResponse("401 Unauthorized", "application/json", "{}",
                                "X-Seraph-Loginreason: AUTHENTICATED_FAILED\r\n");
        });

        TodoListEntry entry;
        entry.id = QStringLiteral("kb48_jira");
        entry.name = QStringLiteral("KB48 Jira");
        entry.provider = QStringLiteral("Jira");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user@example.com");
        entry.apiToken = QStringLiteral("wrong-token");

        IssueTrackerService svc;
        QSignalSpy syncedSpy(&svc, &IssueTrackerService::listIssuesSynced);
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);
        QCOMPARE(syncedSpy.count(), 0);

        // User fixes the token; the server now behaves like Jira Cloud.
        *phase = 2;
        entry.apiToken = QStringLiteral("correct-token");
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);
        QCOMPARE(syncedSpy.count(), 1);
        server.close();
    }

    // KB-49: the KB-45 fix (82b5093) made mapPriorityName match whole names only and strip
    // only "priority::", "priority:", "priority/" (and prio/severity) prefixes or a
    // " priority" suffix separated by a space. Very common GitHub/GitLab label styles that
    // mapped correctly before now give no priority at all: "priority-high",
    // "priority_high", "high-priority", "P1 - High", "prio-low".
    void testCommonPriorityLabelStylesStillMap() {
        QCOMPARE(IssueTrackerService::mapPriorityName(QStringLiteral("priority::high")), quint8(1));
        QStringList unmapped;
        const QList<QPair<QString, quint8>> cases = {
            {QStringLiteral("priority-high"), 1},
            {QStringLiteral("priority_high"), 1},
            {QStringLiteral("high-priority"), 1},
            {QStringLiteral("P1 - High"), 1},
            {QStringLiteral("prio-low"), 3}
        };
        for (const auto& c : cases) {
            if (IssueTrackerService::mapPriorityName(c.first) != c.second) unmapped << c.first;
        }
        QCOMPARE(unmapped, QStringList());
    }

    // KB-50: the KB-49 fix (17b6a8b) moved plain "P0".."P4" matching into a regex that is
    // applied to the *original* label, before the "priority::"/"priority/"/"prio:" prefixes
    // are stripped, and dropped p0..p4 from the post-prefix checks (only "p-0".."p-4" and
    // bare digits remain). Scoped labels such as "priority::P1" (the usual GitLab style),
    // "priority/p2" or "prio:P0", which mapped before, now give no priority at all.
    void testScopedPriorityCodeLabelsMap() {
        QCOMPARE(IssueTrackerService::mapPriorityName(QStringLiteral("P1")), quint8(1));
        QCOMPARE(IssueTrackerService::mapPriorityName(QStringLiteral("priority::1")), quint8(1));
        QStringList unmapped;
        const QList<QPair<QString, quint8>> cases = {
            {QStringLiteral("priority::P1"), 1},
            {QStringLiteral("priority/p2"), 2},
            {QStringLiteral("prio:P0"), 0},
            {QStringLiteral("severity::P3"), 3}
        };
        for (const auto& c : cases) {
            if (IssueTrackerService::mapPriorityName(c.first) != c.second) unmapped << c.first;
        }
        QCOMPARE(unmapped, QStringList());
    }

    // KB-51: defaultPrioritiesForProvider offers display names as mapping keys that the
    // sync code never produces. Todoist items carry remotePriority "Priority 4".."Priority 1"
    // but the defaults are "Priority 4 (P1 Urgent)" etc.; Nextcloud items carry "1".."9" but
    // the defaults are "1 (Highest)", "2-4 (High)" etc. TxtListEditDialog loads these defaults
    // whenever the stored mapping is empty, so merely opening and saving the list makes the
    // mapping non-empty; processSyncedItems then ignores item.priority, finds no matching key
    // and falls back to mapPriorityName, which maps none of these values: every Todoist and
    // Nextcloud task loses its priority on the next sync, and editing the mapping rows has no
    // effect.
    void testDefaultPriorityMappingMatchesSyncedValues() {
        IssueTrackerService svc;
        const auto toMap = [](const QVariantList& rows) {
            QVariantMap m;
            for (const QVariant& r : rows) {
                const QVariantMap row = r.toMap();
                m.insert(row.value(QStringLiteral("remoteName")).toString(), row.value(QStringLiteral("localPriority")));
            }
            return m;
        };
        const QVariantMap todoist = toMap(svc.defaultPrioritiesForProvider(QStringLiteral("Todoist"), {}));
        const QVariantMap nextcloud = toMap(svc.defaultPrioritiesForProvider(QStringLiteral("Nextcloud"), {}));
        QVERIFY(!todoist.isEmpty());
        QVERIFY(!nextcloud.isEmpty());

        QStringList wrong;
        // Values exactly as parseTodoistTasksJson / parseNextcloud* put them into remotePriority.
        if (IssueTrackerService::mapRemotePriorityToLocal(QStringLiteral("Priority 4"), todoist) != 0) wrong << QStringLiteral("Todoist Priority 4");
        if (IssueTrackerService::mapRemotePriorityToLocal(QStringLiteral("Priority 3"), todoist) != 1) wrong << QStringLiteral("Todoist Priority 3");
        if (IssueTrackerService::mapRemotePriorityToLocal(QStringLiteral("1"), nextcloud) != 0) wrong << QStringLiteral("Nextcloud 1");
        if (IssueTrackerService::mapRemotePriorityToLocal(QStringLiteral("3"), nextcloud) != 1) wrong << QStringLiteral("Nextcloud 3");
        if (IssueTrackerService::mapRemotePriorityToLocal(QStringLiteral("5"), nextcloud) != 2) wrong << QStringLiteral("Nextcloud 5");
        QCOMPARE(wrong, QStringList());
    }

    // KB-52: fetchRemotePriorities for Jira sends only Basic auth (applyAuthHeader(..., false))
    // and on any error silently returns the generic defaults. fetchRemoteStatuses and the sync
    // retry with Bearer on 401/403 (Jira Server/DC personal access token with the email field
    // filled in), but the priority "Fetch" button does not, so those users get the five stock
    // names instead of their instance's priorities, with no error shown.
    void testJiraFetchRemotePrioritiesRetriesWithBearer() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        serveHttpFull(server, [](const QByteArray&, const QByteArray&, const QByteArray& head, const QByteArray&) {
            if (head.toLower().contains("authorization: bearer")) {
                return httpResponse("200 OK", "application/json",
                                    "[{\"name\":\"KB52 Custom Blocker\"},{\"name\":\"KB52 Custom Minor\"}]");
            }
            return httpResponse("401 Unauthorized", "application/json", "{}");
        });

        IssueTrackerService svc;
        QSignalSpy loadedSpy(&svc, &IssueTrackerService::remotePrioritiesLoaded);
        svc.fetchRemotePriorities(QStringLiteral("Jira"),
                                  QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()),
                                  QStringLiteral("user@example.com"), QStringLiteral("pat-token"), {});
        QTRY_COMPARE_WITH_TIMEOUT(loadedSpy.count(), 1, 5000);
        QStringList names;
        for (const QVariant& r : loadedSpy.at(0).at(0).toList()) {
            names << r.toMap().value(QStringLiteral("remoteName")).toString();
        }
        QVERIFY2(names.contains(QStringLiteral("KB52 Custom Blocker")), qPrintable(names.join(QStringLiteral(", "))));
        server.close();
    }

    // KB-53: AppController::importBackup (Settings > Storage > Import) and changeDatabasePath
    // swap the whole SQLite file while a work session may be running. The session is not
    // flushed first: the swap happens, then reloadFromDatabase/selectListById ends up in
    // setActiveTodoRow, which stops the timer *after* the new file is already open.
    // onTimerStopped then resolves the old TaskTimer::activeTaskId() against the new database
    // (findActiveTodoRowById / loadTask), where the same autoincrement id names an unrelated
    // task, and books the session (activity record, CSV log and, for tracker lists with
    // autoWorklog, a remote worklog to that task's jira key) to it.
    void testImportBackupDoesNotRebindRunningTimerToOtherTask() {
        const QString backupDb = m_testSandbox.filePath(QStringLiteral("kb53_backup.db"));
        const QString liveDb = m_testSandbox.filePath(QStringLiteral("kb53_live.db"));
        QFile::remove(backupDb);
        QFile::remove(liveDb);
        {
            AppController backup(QString(), backupDb);
            backup.addNewTask(QStringLiteral("KB53 unrelated backup task"));
        }
        DatabaseManager::instance().close();

        AppController controller(QString(), liveDb);
        controller.addNewTask(QStringLiteral("KB53 original timed task"));
        bool selected = false;
        for (int r = 0; r < 200 && !selected; ++r) {
            controller.setActiveTodoRow(r);
            selected = controller.timer()->activeTaskDescription().contains(QStringLiteral("KB53 original"));
        }
        QVERIFY(selected);
        const qint64 timedId = controller.timer()->activeTaskId();
        controller.timer()->start();
        QVERIFY(controller.timer()->isRunning());
        QTest::qWait(1600); // the session must be >= 1 s to be logged at all

        QVERIFY(controller.importBackup(backupDb));
        QVERIFY(!controller.timer()->isRunning());
        // Precondition of the scenario: the imported file holds a different task under the timed id.
        const auto clash = DatabaseManager::instance().loadTask(timedId);
        QVERIFY(clash.has_value());
        QVERIFY(!clash->description().contains(QStringLiteral("KB53 original")));

        QStringList misbooked;
        for (const ActivityRecord& r : DatabaseManager::instance().loadActivityLogs()) {
            if (!r.taskDescription.contains(QStringLiteral("KB53 original"))) misbooked << r.taskDescription;
        }
        QVERIFY2(misbooked.isEmpty(), qPrintable(misbooked.join(QStringLiteral(" | "))));
        DatabaseManager::instance().close();
        QFile::remove(backupDb);
        QFile::remove(liveDb);
    }

    // KB-54: since 6f0d732 fetchRemotePriorities("Jira") emits remotePrioritiesLoaded with
    // whatever parseJiraPrioritiesJson returns, even an empty list. A 200 reply that is not a
    // JSON array (SSO/proxy login page, wrong base URL serving HTML) therefore reaches
    // TxtListEditDialog::onRemotePrioritiesLoaded, which clears priorityMappingModel and shows
    // "Loaded 0 priorities."; saving the dialog then wipes the list's whole priority mapping.
    // Before the change an empty parse fell back to the defaults; the Redmine branch still
    // treats an empty list as an error. Expected: an error signal (or a non-empty list).
    void testJiraPriorityFetchNonJsonDoesNotWipeMapping() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        serveHttpFull(server, [](const QByteArray&, const QByteArray&, const QByteArray&, const QByteArray&) {
            return httpResponse("200 OK", "text/html", "<html><body>Please log in</body></html>");
        });

        IssueTrackerService svc;
        QSignalSpy loadedSpy(&svc, &IssueTrackerService::remotePrioritiesLoaded);
        QSignalSpy errorSpy(&svc, &IssueTrackerService::remotePrioritiesError);
        const QVariantMap existing{{QStringLiteral("KB54 Custom"), QStringLiteral("A")}};
        svc.fetchRemotePriorities(QStringLiteral("Jira"),
                                  QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()),
                                  QStringLiteral("user@example.com"), QStringLiteral("token"), existing);
        QTRY_VERIFY_WITH_TIMEOUT(loadedSpy.count() + errorSpy.count() > 0, 5000);
        const bool emptyLoad = loadedSpy.count() > 0 && loadedSpy.at(0).at(0).toList().isEmpty();
        QVERIFY(!emptyLoad);
        server.close();
    }

    // KB-55: user-visible strings added by recent fixes are missing from every translation
    // dictionary (data/translations/{uk,de,fr,es,it}.json), so they appear in English in all
    // five languages. UkTranslator looks up exact source strings, so each new tr() text must
    // be added to all five files.
    void testRecentTrStringsAreTranslated() {
        const QStringList keys = {
            QStringLiteral("Failed to load Jira priorities: %1"),
            QStringLiteral("Failed to load Jira priorities (%1): %2"),
            QStringLiteral("Failed to load Redmine priorities (%1): %2"),
            QStringLiteral("Nextcloud returned non-CalDAV response (%1)."),
            QStringLiteral("Failed to post worklog to Jira (%1): %2"),
            QStringLiteral("Failed to post worklog to YouTrack (%1): %2"),
            QStringLiteral("Failed to post worklog to GitHub (%1): %2"),
            QStringLiteral("Failed to post worklog to GitLab (%1): %2"),
            QStringLiteral("Failed to post worklog to Redmine (%1): %2"),
            QStringLiteral("Failed to post worklog to Todoist (%1): %2"),
            QStringLiteral("Failed to post worklog to Nextcloud (%1): %2"),
            QStringLiteral("Refused to open unsafe URL scheme: %1")
        };
        QStringList missing;
        // Build-tree copy of data/translations (refreshed on cmake configure).
        const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/data/translations/");
        for (const QString& lang : {QStringLiteral("uk"), QStringLiteral("de"), QStringLiteral("fr"),
                                    QStringLiteral("es"), QStringLiteral("it")}) {
            QFile f(dir + lang + QStringLiteral(".json"));
            QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(f.fileName()));
            const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
            QVERIFY(!obj.isEmpty());
            for (const QString& k : keys) {
                if (!obj.contains(k)) missing << lang + QStringLiteral(": ") + k;
            }
        }
        QVERIFY2(missing.isEmpty(), qPrintable(QString::number(missing.size()) + QStringLiteral(" missing")));
    }

    // KB-56: TaskTimer::skipIteration() is used both for the natural end of an iteration
    // (updateState when the time is up) and for the "Skip" button ("Skip to the break").
    // It always emits timerFinished(m_breakActive) first, so skipping a work session after a
    // minute is treated exactly like finishing it: AppController sets
    // m_lastSessionCompletedPomodoro and logs the session with is_pomodoro_completed = 1
    // (inflating "completed pomodoros" in the dashboard and reports), and SystemHookService
    // runs the pomodoro_completed hook.
    void testSkippedWorkSessionIsNotACompletedPomodoro() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb56.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        controller.addNewTask(QStringLiteral("KB56 skipped task"));
        bool selected = false;
        for (int r = 0; r < 200 && !selected; ++r) {
            controller.setActiveTodoRow(r);
            selected = controller.timer()->activeTaskDescription().contains(QStringLiteral("KB56"));
        }
        QVERIFY(selected);
        QVERIFY(!controller.timer()->isBreakActive());
        QSignalSpy finishedSpy(controller.timer(), &TaskTimer::timerFinished);

        controller.timer()->start();
        QTest::qWait(1600);
        QVERIFY(controller.timer()->remainingSeconds() > 60); // far from the natural end
        controller.timer()->skipIteration();                   // user presses "Skip"
        controller.timer()->stop();

        bool loggedAsCompleted = false;
        bool logged = false;
        for (const ActivityRecord& r : DatabaseManager::instance().loadActivityLogs()) {
            if (r.taskDescription.contains(QStringLiteral("KB56"))) {
                logged = true;
                loggedAsCompleted = loggedAsCompleted || r.isPomodoroCompleted;
            }
        }
        QVERIFY(logged);
        QVERIFY(!loggedAsCompleted);
        QCOMPARE(finishedSpy.count(), 0);
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-57: the FileDialog handlers in DashboardView.qml (CSV and PDF export) and
    // TaskDetailsDialog.qml (open Markdown file) turn the selected file URL into a path with
    // `selectedFile.toString()` and strip "file://". QML's url-to-string keeps reserved
    // characters percent-encoded ('#' -> %23, '%' -> %25, '?' -> %3F; spaces and Cyrillic are
    // decoded), so for a file like "Report #3.csv" or "100% done.md" the CSV/PDF export
    // silently writes "Report %233.csv" next to it and the Markdown file cannot be opened.
    // The test runs the real conversion code taken from the built QML files.
    void testFileDialogPathConversionKeepsReservedCharacters() {
        const QString qmlRoot = QCoreApplication::applicationDirPath() + QStringLiteral("/GoForIt/qml/");
        struct Site { QString file; QString endMarker; };
        const QList<Site> sites = {
            {QStringLiteral("dialogs/TaskDetailsDialog.qml"), QStringLiteral("const content")},
            {QStringLiteral("components/DashboardView.qml"), QStringLiteral("if (app.analytics")}
        };
        const QString target = m_testSandbox.filePath(QStringLiteral("kb57 Report #3 100%.md"));
        {
            QFile f(target);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("# KB57\n");
        }

        QQmlEngine engine;
        QStringList wrong;
        for (const Site& site : sites) {
            QFile qf(qmlRoot + site.file);
            QVERIFY2(qf.open(QIODevice::ReadOnly), qPrintable(qf.fileName()));
            const QString src = QString::fromUtf8(qf.readAll());
            const int start = src.indexOf(QStringLiteral("let path = selectedFile.toString()"));
            QVERIFY2(start >= 0, qPrintable(site.file));
            const int end = src.indexOf(site.endMarker, start);
            QVERIFY2(end > start, qPrintable(site.file));
            const QString snippet = src.mid(start, end - start);

            QQmlComponent comp(&engine);
            comp.setData(QStringLiteral("import QtQml\nQtObject { property url selectedFile\n"
                                        "function convert() { %1\n return path } }").arg(snippet).toUtf8(),
                         QUrl(QStringLiteral("file:///kb57.qml")));
            std::unique_ptr<QObject> obj(comp.create());
            QVERIFY2(obj, qPrintable(comp.errorString()));
            obj->setProperty("selectedFile", QUrl::fromLocalFile(target));
            QVariant result;
            QVERIFY(QMetaObject::invokeMethod(obj.get(), "convert", Q_RETURN_ARG(QVariant, result)));
            if (result.toString() != target) wrong << site.file + QStringLiteral(" -> ") + result.toString();
        }
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(QStringLiteral(" | "))));
    }

    // KB-58: DashboardView declares `stats`/`heatmap` as bindings on app.currentListId, but
    // Component.onCompleted calls reloadData(), which *assigns* both properties and so removes
    // the bindings. The view lives permanently in TaskListPage's StackLayout, and it reloads
    // only on period-tab changes and timerStopped, so after switching to another list the
    // header says "in <new list>" (bound to app.currentFilterTitle) while all numbers, the
    // breakdowns and the heatmap still belong to the previous list - until the next timer
    // stop or tab click. The test loads the real built DashboardView.qml with a real
    // AppController as `app`.
    void testDashboardFollowsCurrentListChange() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb58.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        const QString listA = controller.listManager()->addList(QStringLiteral("KB58 List A"));
        const QString listB = controller.listManager()->addList(QStringLiteral("KB58 List B"));
        QVERIFY(!listA.isEmpty() && !listB.isEmpty());
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QVERIFY(DatabaseManager::instance().insertActivityLog(listA, QStringLiteral("KB58 List A"), -1, QString(),
                QStringLiteral("KB58 a"), now.addSecs(-700), 600, now.addSecs(-100), false, false) > 0);
        QVERIFY(DatabaseManager::instance().insertActivityLog(listB, QStringLiteral("KB58 List B"), -1, QString(),
                QStringLiteral("KB58 b"), now.addSecs(-3700), 3000, now.addSecs(-700), false, false) > 0);
        controller.selectListById(listA);
        QCOMPARE(controller.currentListId(), listA);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        const QString qmlFile = QCoreApplication::applicationDirPath() + QStringLiteral("/GoForIt/qml/components/DashboardView.qml");
        QVERIFY(QFile::exists(qmlFile));
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlFile));
        std::unique_ptr<QObject> view(comp.create());
        QVERIFY2(view, qPrintable(comp.errorString()));

        const auto totalOf = [&view]() {
            return view->property("stats").toMap().value(QStringLiteral("totalSeconds")).toDouble();
        };
        QCOMPARE(totalOf(), 600.0);

        controller.selectListById(listB);
        QCOMPARE(controller.currentListId(), listB);
        QCoreApplication::processEvents();
        QCOMPARE(totalOf(), 3000.0);
        view.reset();
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-59: saving the properties of the current list (TxtListEditDialog::onAccepted ->
    // ListManagerModel::saveListConfig + AppController::selectListById) runs the full list
    // selection again: ListManagerModel emits listUpdated, AppController's handler calls
    // selectListById(id), and the dialog calls it a second time. selectListById ends with
    // setActiveTodoRow(0), which stops a running work session and makes the first task active.
    // So editing anything (name, CSV log path, "Record creation date", a mapping) while the
    // timer runs on another task silently ends the session and switches the timed task.
    // KB-1 avoided exactly this side effect for updateListStatusMapping only.
    void testEditingCurrentListKeepsRunningTimerTask() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb59.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        const QString listId = controller.listManager()->addList(QStringLiteral("KB59 list"));
        QVERIFY(!listId.isEmpty());
        controller.selectListById(listId);
        controller.addNewTask(QStringLiteral("KB59 first"));
        controller.addNewTask(QStringLiteral("KB59 second"));
        bool selected = false;
        for (int r = 0; r < 10 && !selected; ++r) {
            controller.setActiveTodoRow(r);
            selected = controller.timer()->activeTaskDescription().contains(QStringLiteral("KB59 second"));
        }
        QVERIFY(selected);
        controller.timer()->start();
        QVERIFY(controller.timer()->isRunning());

        // Exactly what TxtListEditDialog::onAccepted does after the user edits one field.
        QVariantMap cfg = controller.listManager()->getListConfig(listId);
        cfg.insert(QStringLiteral("addCreationDates"), !cfg.value(QStringLiteral("addCreationDates")).toBool());
        QVERIFY(controller.listManager()->validateAndCheckConflicts(cfg).value(QStringLiteral("valid")).toBool());
        const QString savedId = controller.listManager()->saveListConfig(cfg, QStringLiteral("keep"));
        QCOMPARE(savedId, listId);
        controller.selectListById(savedId);

        const bool running = controller.timer()->isRunning();
        const QString activeTask = controller.timer()->activeTaskDescription();
        controller.timer()->stop();
        QVERIFY(running);
        QVERIFY2(activeTask.contains(QStringLiteral("KB59 second")), qPrintable(activeTask));
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-60: the KB-59 fix (598b460) gives selectListById an early return for the list that
    // is already current, placed before the `m_issueTracker.syncListIssues(entry)` call, and
    // TxtListEditDialog no longer calls selectListById for it. Saving the current remote list
    // (fixed API token, new JQL/query, new server URL) therefore no longer fetches anything:
    // the list keeps showing the old/empty result until the next auto-sync or a manual sync,
    // and clicking the current list again in the overview no longer refreshes it either.
    void testEditingCurrentRemoteListStillSyncs() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        QStringList targets;
        serveHttpFull(server, [&targets](const QByteArray&, const QByteArray& target, const QByteArray&, const QByteArray& body) {
            targets << QString::fromUtf8(target + ' ' + body); // JQL may travel in the URL or a POST body
            return httpResponse("200 OK", "application/json", "{\"issues\":[],\"total\":0,\"isLast\":true}");
        });

        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb60.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        QVariantMap cfg{
            {QStringLiteral("name"), QStringLiteral("KB60 Jira")},
            {QStringLiteral("provider"), QStringLiteral("Jira")},
            {QStringLiteral("jiraUrl"), QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())},
            {QStringLiteral("jiraUser"), QStringLiteral("user@example.com")},
            {QStringLiteral("apiToken"), QStringLiteral("token")},
            {QStringLiteral("jiraJql"), QStringLiteral("project = OLD")}
        };
        const QString listId = controller.listManager()->saveListConfig(cfg, QStringLiteral("keep"));
        QVERIFY(!listId.isEmpty());
        controller.selectListById(listId);
        QTRY_VERIFY_WITH_TIMEOUT(!targets.isEmpty(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.issueTracker()->isSyncing(), 5000);
        targets.clear();

        // The user edits the JQL of the current list and presses OK (TxtListEditDialog::onAccepted).
        cfg = controller.listManager()->getListConfig(listId);
        cfg.insert(QStringLiteral("jiraJql"), QStringLiteral("project = NEW"));
        QVERIFY(controller.listManager()->validateAndCheckConflicts(cfg).value(QStringLiteral("valid")).toBool());
        const QString savedId = controller.listManager()->saveListConfig(cfg, QStringLiteral("keep"));
        if (savedId != controller.currentListId()) {
            controller.selectListById(savedId);
        }
        QTest::qWait(800);
        bool fetchedNewQuery = false;
        for (const QString& t : targets) {
            if (QUrl::fromPercentEncoding(t.toUtf8()).replace(QLatin1Char('+'), QLatin1Char(' ')).contains(QStringLiteral("NEW"))) fetchedNewQuery = true;
        }
        QVERIFY2(fetchedNewQuery, qPrintable(targets.join(QStringLiteral(" | "))));
        QTRY_VERIFY_WITH_TIMEOUT(!controller.issueTracker()->isSyncing(), 5000);
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
        server.close();
    }

    // KB-61: markdownToJiraAdf drops everything after an unclosed ``` fence. Lines after the
    // opening fence are collected in codeLines, but the codeBlock node is only emitted when a
    // closing fence is seen; at the end of the input the pending lines are discarded. A task
    // description (or comment) with a forgotten closing fence is pushed to Jira Cloud with the
    // rest of its text silently missing, overwriting the full remote description.
    void testMarkdownToAdfKeepsUnclosedCodeFence() {
        const QJsonObject doc = IssueTrackerService::markdownToJiraAdf(
            QStringLiteral("Intro line\n```bash\nKB61 command one\nKB61 tail text"));
        const QString json = QString::fromUtf8(QJsonDocument(doc).toJson(QJsonDocument::Compact));
        QVERIFY(json.contains(QStringLiteral("Intro line")));
        QVERIFY2(json.contains(QStringLiteral("KB61 command one")) && json.contains(QStringLiteral("KB61 tail text")),
                 qPrintable(json));
    }

    // KB-62: markdownToJiraAdf emits text nodes with an empty "text" for an empty fenced code
    // block ("```" followed directly by "```"). The ADF schema requires text nodes to be
    // non-empty, so Jira Cloud rejects the whole description/comment update with 400
    // INVALID_INPUT and the push fails.
    void testMarkdownToAdfHasNoEmptyTextNodes() {
        const QJsonObject doc = IssueTrackerService::markdownToJiraAdf(
            QStringLiteral("Steps:\n```\n```\nDone"));
        QStringList emptyNodes;
        std::function<void(const QJsonValue&, const QString&)> walk = [&](const QJsonValue& v, const QString& parent) {
            if (v.isObject()) {
                const QJsonObject o = v.toObject();
                const QString type = o.value(QStringLiteral("type")).toString();
                if (type == QStringLiteral("text") && o.value(QStringLiteral("text")).toString().isEmpty()) {
                    emptyNodes << parent;
                }
                walk(o.value(QStringLiteral("content")), type);
            } else if (v.isArray()) {
                for (const QJsonValue& c : v.toArray()) walk(c, parent);
            }
        };
        walk(doc, QStringLiteral("doc"));
        QVERIFY2(emptyNodes.isEmpty(), qPrintable(QStringLiteral("empty text in: ") + emptyNodes.join(QStringLiteral(", "))));
    }

    // KB-63: updateIssueDescription for Nextcloud replaces the DESCRIPTION property with the
    // regex `DESCRIPTION:.*(?=\r?\n[A-Z-]+:)` and DotMatchesEverythingOption. `.*` is greedy,
    // so the match runs to the *last* "\r\nNAME:" line of the file (END:VCALENDAR) and every
    // property after DESCRIPTION - SUMMARY, STATUS, DUE, PRIORITY, END:VTODO - is deleted. The
    // corrupted calendar object is then PUT back to the server, destroying the task's title
    // and state (or the server rejects it and the edit is silently lost).
    void testNextcloudDescriptionUpdateKeepsOtherProperties() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        const QByteArray ics =
            "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//KB63//EN\r\nBEGIN:VTODO\r\nUID:kb63uid\r\n"
            "DESCRIPTION:old description\r\nSUMMARY:KB63 title\r\nSTATUS:NEEDS-ACTION\r\n"
            "PRIORITY:1\r\nEND:VTODO\r\nEND:VCALENDAR\r\n";
        QByteArray putBody;
        bool putSeen = false;
        serveHttpFull(server, [&](const QByteArray& method, const QByteArray&, const QByteArray&, const QByteArray& body) {
            if (method == "PUT") {
                putBody = body;
                putSeen = true;
                return httpResponse("204 No Content", "text/plain", "");
            }
            return httpResponse("200 OK", "text/calendar", ics);
        });

        TodoListEntry entry;
        entry.id = QStringLiteral("kb63_nc");
        entry.provider = QStringLiteral("Nextcloud");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1/remote.php/dav/calendars/user/tasks/").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user");
        entry.apiToken = QStringLiteral("app-password");

        IssueTrackerService svc;
        QStandardPaths::setTestModeEnabled(false); // updateIssueDescription is a no-op in test mode
        svc.updateIssueDescription(entry, QStringLiteral("NC#kb63uid"), QStringLiteral("new KB63 description"));
        QTRY_VERIFY_WITH_TIMEOUT(putSeen, 5000);
        QStandardPaths::setTestModeEnabled(true);

        const QString put = QString::fromUtf8(putBody);
        QVERIFY2(put.contains(QStringLiteral("DESCRIPTION:new KB63 description")), qPrintable(put));
        QVERIFY2(put.contains(QStringLiteral("SUMMARY:KB63 title")) && put.contains(QStringLiteral("STATUS:NEEDS-ACTION"))
                 && put.contains(QStringLiteral("END:VTODO")), qPrintable(put));
        server.close();
    }

    // KB-64: the KB-63 fix (0c3585a) replaces the *first* DESCRIPTION line anywhere in the
    // calendar object. A VTODO that has no description of its own but has a display alarm
    // (VALARM with ACTION:DISPLAY requires a DESCRIPTION - Thunderbird, DAVx5 and the Nextcloud
    // Tasks app all write one) gets its alarm text overwritten with the new task description,
    // and the task itself still has no description.
    void testNextcloudDescriptionUpdateDoesNotTouchAlarm() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        const QByteArray ics =
            "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//KB64//EN\r\nBEGIN:VTODO\r\nUID:kb64uid\r\n"
            "SUMMARY:KB64 title\r\nSTATUS:NEEDS-ACTION\r\n"
            "BEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER:-PT15M\r\nDESCRIPTION:KB64 alarm text\r\nEND:VALARM\r\n"
            "END:VTODO\r\nEND:VCALENDAR\r\n";
        QByteArray putBody;
        bool putSeen = false;
        serveHttpFull(server, [&](const QByteArray& method, const QByteArray&, const QByteArray&, const QByteArray& body) {
            if (method == "PUT") {
                putBody = body;
                putSeen = true;
                return httpResponse("204 No Content", "text/plain", "");
            }
            return httpResponse("200 OK", "text/calendar", ics);
        });

        TodoListEntry entry;
        entry.id = QStringLiteral("kb64_nc");
        entry.provider = QStringLiteral("Nextcloud");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1/remote.php/dav/calendars/user/tasks/").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user");
        entry.apiToken = QStringLiteral("app-password");

        IssueTrackerService svc;
        QStandardPaths::setTestModeEnabled(false); // updateIssueDescription is a no-op in test mode
        svc.updateIssueDescription(entry, QStringLiteral("NC#kb64uid"), QStringLiteral("new KB64 description"));
        QTRY_VERIFY_WITH_TIMEOUT(putSeen, 5000);
        QStandardPaths::setTestModeEnabled(true);

        const QString put = QString::fromUtf8(putBody);
        const int alarmStart = put.indexOf(QStringLiteral("BEGIN:VALARM"));
        const int alarmEnd = put.indexOf(QStringLiteral("END:VALARM"));
        QVERIFY2(alarmStart >= 0 && alarmEnd > alarmStart, qPrintable(put));
        const QString alarm = put.mid(alarmStart, alarmEnd - alarmStart);
        const QString todoProps = put.left(alarmStart);
        QVERIFY2(alarm.contains(QStringLiteral("DESCRIPTION:KB64 alarm text"))
                 && todoProps.contains(QStringLiteral("DESCRIPTION:new KB64 description")), qPrintable(put));
        server.close();
    }

    // KB-65: the KB-64 fix (393a146) finds the end of the VTODO property block with
    // ics.indexOf("BEGIN:", vtodoStart + 11), which is not anchored to the start of a line.
    // If any property value before DESCRIPTION contains the text "BEGIN:" (a SUMMARY such as
    // "Fix BEGIN: handling in parser"), propBlockEnd lands in the middle of that SUMMARY line:
    // the existing DESCRIPTION is considered outside the block, and the new DESCRIPTION line is
    // inserted inside the SUMMARY value. The PUT then corrupts the title and leaves two
    // descriptions. postIssueComment uses the same unanchored search for COMMENT.
    void testNextcloudDescriptionUpdateWithBeginTextInSummary() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        const QByteArray ics =
            "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//KB65//EN\r\nBEGIN:VTODO\r\nUID:kb65uid\r\n"
            "SUMMARY:KB65 fix BEGIN: handling\r\nDESCRIPTION:old KB65 description\r\nSTATUS:NEEDS-ACTION\r\n"
            "END:VTODO\r\nEND:VCALENDAR\r\n";
        QByteArray putBody;
        bool putSeen = false;
        serveHttpFull(server, [&](const QByteArray& method, const QByteArray&, const QByteArray&, const QByteArray& body) {
            if (method == "PUT") {
                putBody = body;
                putSeen = true;
                return httpResponse("204 No Content", "text/plain", "");
            }
            return httpResponse("200 OK", "text/calendar", ics);
        });

        TodoListEntry entry;
        entry.id = QStringLiteral("kb65_nc");
        entry.provider = QStringLiteral("Nextcloud");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1/remote.php/dav/calendars/user/tasks/").arg(server.serverPort());
        entry.jiraUser = QStringLiteral("user");
        entry.apiToken = QStringLiteral("app-password");

        IssueTrackerService svc;
        QStandardPaths::setTestModeEnabled(false); // updateIssueDescription is a no-op in test mode
        svc.updateIssueDescription(entry, QStringLiteral("NC#kb65uid"), QStringLiteral("new KB65 description"));
        QTRY_VERIFY_WITH_TIMEOUT(putSeen, 5000);
        QStandardPaths::setTestModeEnabled(true);

        const QString put = QString::fromUtf8(putBody);
        QVERIFY2(put.contains(QStringLiteral("\r\nSUMMARY:KB65 fix BEGIN: handling\r\n"))
                 && put.count(QStringLiteral("DESCRIPTION:")) == 1
                 && put.contains(QStringLiteral("DESCRIPTION:new KB65 description")), qPrintable(put));
        server.close();
    }

    // KB-66: in a multi-list view ("All Lists", a provider group, a custom multi-list filter)
    // AppController keeps m_currentListId of the last single list. DashboardView passes
    // app.currentListId to getProductivityStats / getActivityHeatmap / exportBilling*, so the
    // dashboard header says "in All Lists" (app.currentFilterTitle) while totals, breakdowns,
    // heatmap and the exported CSV/PDF cover only the previously selected list.
    // (loadActivityLogs already treats "__ALL__" as "no list filter".)
    void testDashboardInAllListsViewCoversAllLists() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb66.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        const QString listA = controller.listManager()->addList(QStringLiteral("KB66 List A"));
        const QString listB = controller.listManager()->addList(QStringLiteral("KB66 List B"));
        QVERIFY(!listA.isEmpty() && !listB.isEmpty());
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QVERIFY(DatabaseManager::instance().insertActivityLog(listA, QStringLiteral("KB66 List A"), -1, QString(),
                QStringLiteral("KB66 a"), now.addSecs(-700), 600, now.addSecs(-100), false, false) > 0);
        QVERIFY(DatabaseManager::instance().insertActivityLog(listB, QStringLiteral("KB66 List B"), -1, QString(),
                QStringLiteral("KB66 b"), now.addSecs(-3700), 3000, now.addSecs(-700), false, false) > 0);
        controller.selectListById(listA);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        const QString qmlFile = QCoreApplication::applicationDirPath() + QStringLiteral("/GoForIt/qml/components/DashboardView.qml");
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlFile));
        std::unique_ptr<QObject> view(comp.create());
        QVERIFY2(view, qPrintable(comp.errorString()));
        QCOMPARE(view->property("stats").toMap().value(QStringLiteral("totalSeconds")).toDouble(), 600.0);

        controller.showAllLists();
        QCoreApplication::processEvents();
        QCOMPARE(controller.currentFilterTitle(), tr("All Lists"));
        const double total = view->property("stats").toMap().value(QStringLiteral("totalSeconds")).toDouble();
        QCOMPARE(total, 3600.0);
        QTRY_VERIFY_WITH_TIMEOUT(!controller.issueTracker()->isSyncing(), 5000);
        view.reset();
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-67: AppController::selectListById always calls TaskTimer::setListTimerOverrides, which
    // sets m_iteration = 0, also when the list being "selected" is already the current one -
    // clicking it again in the overview, saving its properties (listUpdated), or any other
    // re-selection. In Pomodoro mode m_iteration is the position in the work/break cycle, so
    // every such re-selection restarts the cycle: the long break after N pomodoros is
    // postponed (or never reached for a user who often re-opens the list).
    void testReselectingCurrentListKeepsPomodoroCycle() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb67.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        SettingsManager* settings = controller.settings();
        const int oldMode = settings->timerMode();
        const int oldPeriod = settings->pomodoroPeriod();
        const int oldTask = settings->taskDuration();
        const int oldBreak = settings->breakDuration();
        const int oldLong = settings->longBreakDuration();
        const bool oldResume = settings->resumeTasksAfterBreak();
        settings->setTaskDuration(1500);
        settings->setBreakDuration(300);
        settings->setLongBreakDuration(900);
        settings->setPomodoroPeriod(2);   // work, short break, work, LONG break
        settings->setTimerMode(1);        // Pomodoro
        settings->setResumeTasksAfterBreak(false);

        const QString listId = controller.listManager()->addList(QStringLiteral("KB67 list"));
        controller.selectListById(listId);
        controller.addNewTask(QStringLiteral("KB67 task"));
        controller.setActiveTodoRow(0);
        QVERIFY(controller.timer()->hasActiveTask());

        TaskTimer* timer = controller.timer();
        timer->skipIteration();                 // pomodoro 1 -> short break (starts)
        QVERIFY(timer->isBreakActive());
        QVERIFY(qAbs(int(timer->remainingSeconds()) - 300) <= 2);
        timer->skipIteration();                 // short break -> pomodoro 2
        QVERIFY(!timer->isBreakActive());
        timer->stop();

        controller.selectListById(listId);      // user clicks the current list again
        timer->skipIteration();                 // pomodoro 2 -> should be the LONG break
        QVERIFY(timer->isBreakActive());
        const int breakSec = int(timer->remainingSeconds());
        timer->stop();

        settings->setTimerMode(oldMode);
        settings->setPomodoroPeriod(oldPeriod);
        settings->setTaskDuration(oldTask);
        settings->setBreakDuration(oldBreak);
        settings->setLongBreakDuration(oldLong);
        settings->setResumeTasksAfterBreak(oldResume);

        QVERIFY2(qAbs(breakSec - 900) <= 2, qPrintable(QString::number(breakSec)));
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-68: the TUI list editor masks the API token in its field list
    // (`left(4) + "••••••••"`), and the GUI uses a password field, but pressing Enter on the
    // token row copies the full token into m_inputBuffer and render() prints it in clear on
    // the "Enter value:" line (and keeps it on screen while the user edits). Anyone looking at
    // the terminal, a screen share, or a tmux/asciinema recording gets the whole secret.
    // The test drives the real TuiApp and captures its terminal output.
    void testTuiListEditDoesNotPrintApiToken() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb68_tui.db")));
        const QString secret = QStringLiteral("KB68secretTOKEN0123456789");
        QVariantMap cfg{
            {QStringLiteral("name"), QStringLiteral("KB68 Jira")},
            {QStringLiteral("provider"), QStringLiteral("Jira")},
            {QStringLiteral("jiraUrl"), QStringLiteral("http://127.0.0.1:9")},
            {QStringLiteral("jiraUser"), QStringLiteral("user@example.com")},
            {QStringLiteral("apiToken"), secret}
        };
        const QString listId = ctrl.listManager()->saveListConfig(cfg, QStringLiteral("keep"));
        QVERIFY(!listId.isEmpty());
        ctrl.selectListById(listId);

        // TuiApp only renders on a real terminal, so run it on a pseudo-terminal and read the
        // frames from the master side.
        const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
        QVERIFY(master >= 0);
        QCOMPARE(::grantpt(master), 0);
        QCOMPARE(::unlockpt(master), 0);
        const char* slaveName = ::ptsname(master);
        QVERIFY(slaveName != nullptr);
        const int slave = ::open(slaveName, O_RDWR | O_NOCTTY);
        QVERIFY(slave >= 0);
        struct winsize ws {};
        ws.ws_row = 30;
        ws.ws_col = 120;
        ::ioctl(slave, TIOCSWINSZ, &ws);
        ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);

        QByteArray captured;
        auto drain = [&]() {
            char buf[8192];
            for (;;) {
                const ssize_t n = ::read(master, buf, sizeof(buf));
                if (n <= 0) break;
                captured.append(buf, static_cast<int>(n));
            }
        };
        const int savedStdin = ::dup(STDIN_FILENO);
        const int savedStdout = ::dup(STDOUT_FILENO);
        QVERIFY(savedStdin >= 0 && savedStdout >= 0);
        std::fflush(stdout);
        ::dup2(slave, STDIN_FILENO);
        ::dup2(slave, STDOUT_FILENO);
        {
            TuiApp tui(&ctrl);
            const bool started = tui.start();
            drain();
            auto feed = [&](const QByteArray& bytes) {
                (void)::write(master, bytes.constData(), static_cast<size_t>(bytes.size()));
                QTest::qWait(20);
                for (int i = 0; i < 8; ++i) {
                    int pending = 0;
                    ::ioctl(STDIN_FILENO, FIONREAD, &pending);
                    if (pending <= 0) break;
                    QMetaObject::invokeMethod(&tui, "onStdinActivated", Qt::DirectConnection);
                    drain();
                }
                drain();
            };
            if (started) {
                feed("L");                       // list picker (current list selected)
                feed("e");                       // edit list
                for (int i = 0; i < 4; ++i) feed("j");   // row 4: API token
                feed("\r");                      // start editing the token
                feed("\033");                    // cancel the edit
                QTest::qWait(80);                // let the ESC timer fire
                drain();
            }
        }
        drain();
        ::dup2(savedStdin, STDIN_FILENO);
        ::dup2(savedStdout, STDOUT_FILENO);
        ::close(savedStdin);
        ::close(savedStdout);
        ::close(slave);
        ::close(master);

        const QString screen = QString::fromUtf8(captured);
        QVERIFY2(screen.contains(QStringLiteral("KB68")), "TUI produced no list-edit output");
        QVERIFY(!screen.contains(secret));
        QTRY_VERIFY_WITH_TIMEOUT(!ctrl.issueTracker()->isSyncing(), 5000);
    }

    // KB-69: the hook template written by ensureHooksDirectory (sample_hook.sh) documents
    //   GOFI_DURATION_SEC  "Configured duration in seconds"
    //   GOFI_REMAINING_SEC "Remaining time in seconds"
    // but SystemHookService::triggerHook never sets GOFI_REMAINING_SEC and fills
    // GOFI_DURATION_SEC with TaskTimer::remainingSeconds(). User hooks that follow the template
    // get an empty GOFI_REMAINING_SEC, and pomodoro_completed (fired when the time is up)
    // reports a "configured duration" of 0. The test runs a real hook process on work_start.
    void testHookEnvironmentMatchesDocumentedVariables() {
        const QString testDb = m_testSandbox.filePath(QStringLiteral("kb69_hook.db"));
        QFile::remove(testDb);
        AppController controller(QString(), testDb);
        SettingsManager* settings = controller.settings();
        SystemHookService* hooks = controller.systemHooks();

        // The documented contract comes from the template the app installs.
        const QString hooksDir = m_testSandbox.filePath(QStringLiteral("kb69_hooks"));
        QDir(hooksDir).removeRecursively();
        hooks->setCustomHooksDirectory(hooksDir);
        QFile tpl(QDir(hooksDir).filePath(QStringLiteral("sample_hook.sh")));
        QVERIFY(tpl.open(QIODevice::ReadOnly));
        const QString tplText = QString::fromUtf8(tpl.readAll());
        QVERIFY(tplText.contains(QStringLiteral("GOFI_REMAINING_SEC")));

        SystemHookService::setSystemIntegrationEnabled(true);
        const auto resetIntegration = qScopeGuard([]() {
            SystemHookService::setSystemIntegrationEnabled(false);
        });
        const bool oldHooks = settings->customHooksEnabled();
        const QString oldCmd = settings->hookWorkStartCommand();
        settings->setCustomHooksEnabled(true);
        settings->setHookWorkStartCommand(QStringLiteral("printf 'R=[%s] D=[%s]' \"$GOFI_REMAINING_SEC\" \"$GOFI_DURATION_SEC\""));

        controller.addNewTask(QStringLiteral("KB69 task"));
        controller.setActiveTodoRow(0);
        QSignalSpy spy(hooks, &SystemHookService::hookExecuted);
        controller.timer()->start();                     // emits workStarted -> work_start hook
        QVERIFY(spy.wait(3000));
        controller.timer()->stop();
        const QString out = spy.first().at(2).toString();

        settings->setHookWorkStartCommand(oldCmd);
        settings->setCustomHooksEnabled(oldHooks);
        QDir(hooksDir).removeRecursively();

        static const QRegularExpression remainingSet(QStringLiteral("R=\\[\\d+\\]"));
        QVERIFY2(remainingSet.match(out).hasMatch(), qPrintable(out));
        DatabaseManager::instance().close();
        QFile::remove(testDb);
    }

    // KB-70: TuiApp::renderMarkdownToAnsi truncates code-block lines and word-wraps paragraphs,
    // but emits headings (`#`, `##`, ...) and table rows (`| ... |`) at full length, and the
    // MarkdownView branch of render() writes every line as is. A heading or table row wider than
    // the terminal is wrapped by the terminal onto extra rows, so the frame grows past the
    // screen height: the view scrolls, the header bar disappears and the status line is drawn
    // over the content. The test runs the real TuiApp on a 30x120 pseudo-terminal and opens a
    // task description with a 250-character heading.
    void testTuiMarkdownViewKeepsLongHeadingsInsideTerminalWidth() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb70_tui.db")));
        const QString listId = ctrl.listManager()->addList(QStringLiteral("KB70"));
        ctrl.selectListById(listId);
        while (ctrl.todoProxyModel()->rowCount() > 0) QVERIFY(ctrl.removeTask(false, 0));
        ctrl.addNewTask(QStringLiteral("KB70 task"));
        QCOMPARE(ctrl.todoProxyModel()->rowCount(), 1);
        const QString longRun = QStringLiteral("KB70") + QString(246, QLatin1Char('h'));
        QVERIFY(ctrl.updateTaskDetails(false, 0, QStringLiteral("# ") + longRun + QStringLiteral("\n\nbody text")));

        const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
        QVERIFY(master >= 0);
        QCOMPARE(::grantpt(master), 0);
        QCOMPARE(::unlockpt(master), 0);
        const int slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY);
        QVERIFY(slave >= 0);
        struct winsize ws {};
        ws.ws_row = 30;
        ws.ws_col = 120;
        ::ioctl(slave, TIOCSWINSZ, &ws);
        ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
        QByteArray captured;
        auto drain = [&]() {
            char buf[8192];
            for (;;) {
                const ssize_t n = ::read(master, buf, sizeof(buf));
                if (n <= 0) break;
                captured.append(buf, static_cast<int>(n));
            }
        };
        const int savedStdin = ::dup(STDIN_FILENO);
        const int savedStdout = ::dup(STDOUT_FILENO);
        std::fflush(stdout);
        ::dup2(slave, STDIN_FILENO);
        ::dup2(slave, STDOUT_FILENO);
        bool started = false;
        {
            TuiApp tui(&ctrl);
            started = tui.start();
            drain();
            if (started) {
                (void)::write(master, "v", 1);
                QTest::qWait(20);
                int pending = 0;
                ::ioctl(STDIN_FILENO, FIONREAD, &pending);
                if (pending > 0) QMetaObject::invokeMethod(&tui, "onStdinActivated", Qt::DirectConnection);
                drain();
                captured.clear();                       // keep only the markdown frame
                QMetaObject::invokeMethod(&tui, "render", Qt::DirectConnection);
                drain();
                (void)::write(master, "q", 1);
                QTest::qWait(20);
                ::ioctl(STDIN_FILENO, FIONREAD, &pending);
                if (pending > 0) QMetaObject::invokeMethod(&tui, "onStdinActivated", Qt::DirectConnection);
                drain();
            }
        }
        drain();
        ::dup2(savedStdin, STDIN_FILENO);
        ::dup2(savedStdout, STDOUT_FILENO);
        ::close(savedStdin);
        ::close(savedStdout);
        ::close(slave);
        ::close(master);
        QVERIFY(started);

        // Keep the last full frame (render() starts each frame with clear+home), strip ANSI
        // sequences, and check that the 250-character heading was not printed in one piece.
        QString screen = QString::fromUtf8(captured);
        const int lastFrame = screen.lastIndexOf(QStringLiteral("\x1b[H"));
        if (lastFrame >= 0) screen = screen.mid(lastFrame);
        static const QRegularExpression ansiRe(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
        screen.remove(ansiRe);
        QVERIFY2(screen.contains(QStringLiteral("KB70")), "markdown view was not rendered");
        QVERIFY2(!screen.contains(longRun), "the 250-column heading was written in one line on a 120-column terminal");
    }

    // KB-71: TaskDetailsDialog remembers the task by its *proxy row* (`taskRow`) when it opens
    // and, on OK / "Post Comment" / priority change, calls app.updateTaskDetails /
    // addTaskComment / changeTaskPriority with that row. If the list changes while the dialog
    // is open - an auto-sync refresh brings in a new issue, a task is added on top, removed,
    // completed, or the filter changes - the row now points to another task. The edited
    // description is then saved to the wrong task and, for tracker lists, pushed to the wrong
    // issue (updateIssueDescription / postIssueComment use the jira key of whatever task is now
    // at that row). The test loads the real built TaskDetailsDialog.qml with a real AppController.
    void testTaskDetailsDialogSavesToTheTaskItWasOpenedFor() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb71.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        const bool oldOnTop = controller.settings()->newTasksOnTop();
        controller.settings()->setNewTasksOnTop(false);
        const QString listId = controller.listManager()->addList(QStringLiteral("KB71 list"));
        controller.selectListById(listId);
        while (controller.todoProxyModel()->rowCount() > 0) QVERIFY(controller.removeTask(false, 0));
        controller.addNewTask(QStringLiteral("KB71 first"));
        controller.addNewTask(QStringLiteral("KB71 second"));
        QCOMPARE(controller.todoProxyModel()->rowCount(), 2);
        int rowSecond = -1;
        for (int r = 0; r < 2; ++r) {
            if (controller.todoProxyModel()->data(controller.todoProxyModel()->index(r, 0), TaskStoreModel::DescriptionRole)
                    .toString().contains(QStringLiteral("KB71 second"))) rowSecond = r;
        }
        QVERIFY(rowSecond >= 0);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        const QString qmlFile = QCoreApplication::applicationDirPath() + QStringLiteral("/GoForIt/qml/dialogs/TaskDetailsDialog.qml");
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlFile));
        std::unique_ptr<QObject> dialog(comp.create());
        QVERIFY2(dialog, qPrintable(comp.errorString()));

        // User opens the details of "KB71 second" and types a description.
        QVERIFY(QMetaObject::invokeMethod(dialog.get(), "openForTask",
            Q_ARG(QVariant, false), Q_ARG(QVariant, rowSecond), Q_ARG(QVariant, QStringLiteral("KB71 second")),
            Q_ARG(QVariant, QStringLiteral("todo")), Q_ARG(QVariant, QStringLiteral("KB71 description for second")),
            Q_ARG(QVariant, QVariantList()), Q_ARG(QVariant, QString()), Q_ARG(QVariant, QString()),
            Q_ARG(QVariant, QString())));

        // Meanwhile the list changes: a new task lands on top (as with "new tasks on top", or
        // a new issue arriving with an auto-sync), shifting every row down by one.
        controller.settings()->setNewTasksOnTop(true);
        controller.addNewTask(QStringLiteral("KB71 inserted"));
        QCOMPARE(controller.todoProxyModel()->rowCount(), 3);

        QVERIFY(QMetaObject::invokeMethod(dialog.get(), "accept"));
        QCoreApplication::processEvents();

        QString secondDetails;
        QString firstDetails;
        const auto collect = [&](TaskFilterProxyModel* proxy) {
            for (int r = 0; r < proxy->rowCount(); ++r) {
                const QModelIndex i = proxy->index(r, 0);
                const QString d = proxy->data(i, TaskStoreModel::DescriptionRole).toString();
                const QString det = proxy->data(i, TaskStoreModel::DetailsRole).toString();
                if (d.contains(QStringLiteral("KB71 second"))) secondDetails = det;
                if (d.contains(QStringLiteral("KB71 first")) || d.contains(QStringLiteral("KB71 inserted"))) firstDetails += det;
            }
        };
        collect(controller.todoProxyModel());
        collect(controller.doneProxyModel());
        controller.settings()->setNewTasksOnTop(oldOnTop);
        dialog.reset();
        QVERIFY2(secondDetails == QStringLiteral("KB71 description for second") && firstDetails.isEmpty(),
                 qPrintable(QStringLiteral("second='%1' first='%2'").arg(secondDetails, firstDetails)));
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-72: Main.qml binds every global Shortcut as
    //   `sequence: app.shortcuts.shortcutFor("<id>")`
    // A QML binding to an invokable call is evaluated once and only re-evaluated when a
    // notifying property it reads changes; `app.shortcuts` is CONSTANT and nothing in qml/
    // listens to ShortcutManager::shortcutsChanged. Changing (or resetting) a shortcut in
    // Settings > Shortcuts therefore updates the settings list and shortcuts.ini, but the
    // window keeps reacting to the old key combination until the app is restarted - and a key
    // freed by conflict resolution keeps triggering its old action. The test loads the real
    // built Main.qml with a real AppController and inspects its Shortcut objects.
    void testChangedShortcutAppliesWithoutRestart() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb72.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        ShortcutManager* sm = controller.shortcuts();
        const QString oldFilter = sm->shortcutFor(QStringLiteral("filter"));
        QVERIFY(!oldFilter.isEmpty());
        const QString newSeq = QStringLiteral("Ctrl+Alt+F9");

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        engine.rootContext()->setContextProperty(QStringLiteral("showAboutOnStart"), false);
        // The build-tree module dirs carry qmldir files that `prefer` the compiled qrc copy, which
        // only exists inside go-for-it-qt. Copy the built QML sources without them and load Main.qml.
        const QString srcRoot = QCoreApplication::applicationDirPath() + QStringLiteral("/GoForIt/qml");
        const QString qmlRoot = m_testSandbox.filePath(QStringLiteral("kb72_qml"));
        QDir(qmlRoot).removeRecursively();
        QDirIterator it(srcRoot, {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            const QString dst = qmlRoot + f.mid(srcRoot.size());
            QDir().mkpath(QFileInfo(dst).absolutePath());
            QVERIFY(QFile::copy(f, dst));
        }
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlRoot + QStringLiteral("/Main.qml")));
        std::unique_ptr<QObject> window(comp.create());
        QVERIFY2(window, qPrintable(comp.errorString()));

        const auto sequences = [&window]() {
            QStringList seqs;
            for (QObject* o : window->findChildren<QObject*>()) {
                if (QByteArray(o->metaObject()->className()).contains("Shortcut")) {
                    const QVariant v = o->property("sequence");
                    seqs << QKeySequence(v.toString()).toString(QKeySequence::PortableText);
                }
            }
            return seqs;
        };
        const QStringList before = sequences();
        QVERIFY2(before.contains(QKeySequence(oldFilter).toString(QKeySequence::PortableText)), qPrintable(before.join(QStringLiteral(", "))));

        sm->setShortcut(QStringLiteral("filter"), newSeq);
        QCoreApplication::processEvents();
        const QStringList after = sequences();

        sm->setShortcut(QStringLiteral("filter"), oldFilter);   // restore the sandbox shortcuts.ini
        window.reset();
        QVERIFY2(after.contains(newSeq), qPrintable(after.join(QStringLiteral(", "))));
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-73: the TUI header bar prints the current list name (`currentFilterTitle`) in full,
    // and the tab row appends the search query (" 🔍 /<query>") in full. Neither is truncated
    // to the terminal width (headerGap is clamped to 1 but the badge itself is not cut), so a
    // long list name or search query makes the terminal wrap that row and the whole frame
    // shifts down, just like KB-70. The test runs the real TuiApp on a 30x120 pseudo-terminal
    // with a 150-character list name.
    void testTuiHeaderKeepsLongListNameInsideTerminalWidth() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb73_tui.db")));
        const QString longName = QStringLiteral("KB73") + QString(146, QLatin1Char('n'));
        const QString listId = ctrl.listManager()->addList(longName);
        ctrl.selectListById(listId);
        QCOMPARE(ctrl.currentFilterTitle(), longName);

        const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
        QVERIFY(master >= 0);
        QCOMPARE(::grantpt(master), 0);
        QCOMPARE(::unlockpt(master), 0);
        const int slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY);
        QVERIFY(slave >= 0);
        struct winsize ws {};
        ws.ws_row = 30;
        ws.ws_col = 120;
        ::ioctl(slave, TIOCSWINSZ, &ws);
        ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
        QByteArray captured;
        auto drain = [&]() {
            char buf[8192];
            for (;;) {
                const ssize_t n = ::read(master, buf, sizeof(buf));
                if (n <= 0) break;
                captured.append(buf, static_cast<int>(n));
            }
        };
        const int savedStdin = ::dup(STDIN_FILENO);
        const int savedStdout = ::dup(STDOUT_FILENO);
        std::fflush(stdout);
        ::dup2(slave, STDIN_FILENO);
        ::dup2(slave, STDOUT_FILENO);
        bool started = false;
        {
            TuiApp tui(&ctrl);
            started = tui.start();
            drain();
            if (started) {
                captured.clear();
                QMetaObject::invokeMethod(&tui, "render", Qt::DirectConnection);
                drain();
            }
        }
        drain();
        ::dup2(savedStdin, STDIN_FILENO);
        ::dup2(savedStdout, STDOUT_FILENO);
        ::close(savedStdin);
        ::close(savedStdout);
        ::close(slave);
        ::close(master);
        QVERIFY(started);

        QString screen = QString::fromUtf8(captured);
        const int lastFrame = screen.lastIndexOf(QStringLiteral("\x1b[H"));
        if (lastFrame >= 0) screen = screen.mid(lastFrame);
        static const QRegularExpression ansiRe(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
        screen.remove(ansiRe);
        QVERIFY2(screen.contains(QStringLiteral("KB73")), "TUI frame was not rendered");
        QVERIFY2(!screen.contains(longName), "the 150-column list name was written in one line on a 120-column terminal");
    }

    // KB-74: TaskFilterProxyModel::setFilterQuery turns every search token that contains a
    // colon into a Tag needle (name before the first ':', value after it), and
    // filterAcceptsRow then only compares it with TxtPart's of type Tag. TxtTask stores
    // http(s)/mailto links as TxtPartType::Uri and tokens with more than one colon (times like
    // 10:30:00, "a:b:c") as Word, so searching for a pasted link or such a token finds nothing,
    // although the text is right there in the task description.
    void testSearchFindsTasksByUrlAndColonWords() {
        TaskStoreModel store(false);
        store.setTasks({
            TxtTask::fromSimpleTxt(QStringLiteral("Review https://example.com/pr/1 today"), false, false),
            TxtTask::fromSimpleTxt(QStringLiteral("Standup at 10:30:00 sharp"), false, false),
            TxtTask::fromSimpleTxt(QStringLiteral("Unrelated task"), false, false)
        });
        TaskFilterProxyModel proxy;
        proxy.setSourceModel(&store);
        QCOMPARE(proxy.rowCount(), 3);

        proxy.setFilterQuery(QStringLiteral("review"));        // plain word works
        QCOMPARE(proxy.rowCount(), 1);

        QStringList notFound;
        proxy.setFilterQuery(QStringLiteral("https://example.com/pr/1"));
        if (proxy.rowCount() != 1) notFound << QStringLiteral("https://example.com/pr/1");
        proxy.setFilterQuery(QStringLiteral("10:30:00"));
        if (proxy.rowCount() != 1) notFound << QStringLiteral("10:30:00");
        QCOMPARE(notFound, QStringList());
    }

    // KB-75: parseGitHubIssuesJson marks a pull request as "merged" when
    // `!pull_request.merged_at.isNull()`. A missing key yields QJsonValue::Undefined, which is
    // not Null, so every open PR whose `pull_request` object has no `merged_at` field (GitHub
    // Enterprise Server versions before the field was added to issue/search results, proxies or
    // mirrors that trim the object) is reported as merged: the task gets "@merged" and, with the
    // usual "merged -> done" status mapping, open PRs are closed locally on every sync.
    void testGitHubOpenPrWithoutMergedAtIsNotMerged() {
        const QByteArray json =
            "{\"total_count\":1,\"incomplete_results\":false,\"items\":["
            "{\"number\":42,\"title\":\"KB75 open PR\",\"state\":\"open\",\"draft\":false,"
            "\"repository_url\":\"https://api.github.com/repos/owner/repo\","
            "\"html_url\":\"https://github.com/owner/repo/pull/42\","
            "\"pull_request\":{\"url\":\"https://api.github.com/repos/owner/repo/pulls/42\"},"
            "\"labels\":[]}]}";
        const QVector<RemoteIssueItem> items = IssueTrackerService::parseGitHubIssuesJson(json);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().remoteStatus, QStringLiteral("open"));
    }

    // KB-76: the KB-70/KB-73 fix (3fc5b46) passes already-coloured strings through
    // TuiApp::truncateVisible: the status tab row, every Markdown-view line and table rows.
    // truncateVisible measures with codepointDisplayWidth, which gives ESC width 0 but counts
    // every other byte of an SGR sequence ("[38;5;244m" = 10 columns) as visible text. A line is
    // therefore cut long before the terminal edge, gets a stray "…", and the cut can land inside
    // an escape sequence, leaving a broken SGR and no reset. On a normal 100-column terminal the
    // tab row (~78 visible columns, ~90 bytes of SGR codes) loses its last tabs. The test runs the
    // real TuiApp on a 30x100 pseudo-terminal and looks at the tab row.
    void testTuiTabRowIsNotTruncatedByAnsiCodes() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb76_tui.db")));
        const QString listId = ctrl.listManager()->addList(QStringLiteral("KB76"));
        ctrl.selectListById(listId);

        const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
        QVERIFY(master >= 0);
        QCOMPARE(::grantpt(master), 0);
        QCOMPARE(::unlockpt(master), 0);
        const int slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY);
        QVERIFY(slave >= 0);
        struct winsize ws {};
        ws.ws_row = 30;
        ws.ws_col = 100;
        ::ioctl(slave, TIOCSWINSZ, &ws);
        ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
        QByteArray captured;
        auto drain = [&]() {
            char buf[8192];
            for (;;) {
                const ssize_t n = ::read(master, buf, sizeof(buf));
                if (n <= 0) break;
                captured.append(buf, static_cast<int>(n));
            }
        };
        const int savedStdin = ::dup(STDIN_FILENO);
        const int savedStdout = ::dup(STDOUT_FILENO);
        std::fflush(stdout);
        ::dup2(slave, STDIN_FILENO);
        ::dup2(slave, STDOUT_FILENO);
        bool started = false;
        {
            TuiApp tui(&ctrl);
            started = tui.start();
            drain();
            if (started) {
                captured.clear();
                QMetaObject::invokeMethod(&tui, "render", Qt::DirectConnection);
                drain();
            }
        }
        drain();
        ::dup2(savedStdin, STDIN_FILENO);
        ::dup2(savedStdout, STDOUT_FILENO);
        ::close(savedStdin);
        ::close(savedStdout);
        ::close(slave);
        ::close(master);
        QVERIFY(started);

        QString screen = QString::fromUtf8(captured);
        const int lastFrame = screen.lastIndexOf(QStringLiteral("\x1b[H"));
        if (lastFrame >= 0) screen = screen.mid(lastFrame);
        const QStringList rows = screen.split(QStringLiteral("\r\n"));
        QVERIFY(rows.size() > 3);
        static const QRegularExpression ansiRe(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
        QString tabRow = rows.at(2);
        tabRow.remove(ansiRe);
        QVERIFY2(tabRow.contains(tr(" 1:All Open ")), qPrintable(tabRow));
        QVERIFY2(tabRow.contains(tr(" 6:Canceled ")) && !tabRow.contains(QChar(0x2026)), qPrintable(tabRow));
    }

    // KB-77: for a GitHub list with a user query, fetchGitHubIssues only prefixes "repo:" to an
    // "owner/repo" value (the format the list dialog's placeholder suggests) and never adds an
    // open-state filter, unlike the default query ("is:open involves:@me"), GitLab
    // (state=opened) and Redmine (status_id=open). The Search API then returns open *and*
    // closed issues and PRs, sorted by created ascending, capped at 1000. Every closed item is
    // imported as a done task, and for a repository with more than 1000 issues/PRs the first
    // 1000 results are the oldest ones - the current open issues never reach the list at all.
    void testGitHubRepoQueryRequestsOpenItemsOnly() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        QString sentQuery;
        serveHttpFull(server, [&sentQuery](const QByteArray&, const QByteArray& target, const QByteArray&, const QByteArray&) {
            const QUrlQuery q(QUrl(QString::fromUtf8(target)).query());
            sentQuery = q.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
            return httpResponse("200 OK", "application/json", "{\"total_count\":0,\"incomplete_results\":false,\"items\":[]}");
        });
        TodoListEntry entry;
        entry.id = QStringLiteral("kb77_gh");
        entry.provider = QStringLiteral("GitHub");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.apiToken = QStringLiteral("token");
        entry.jiraJql = QStringLiteral("owner/repo");

        IssueTrackerService svc;
        svc.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!sentQuery.isEmpty(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);
        QVERIFY2(sentQuery.contains(QStringLiteral("repo:owner/repo")), qPrintable(sentQuery));
        QVERIFY2(sentQuery.contains(QStringLiteral("is:open")) || sentQuery.contains(QStringLiteral("state:open")), qPrintable(sentQuery));
        server.close();
    }

    // KB-84: an unrelated GitHub qualifier such as is:issue must not be mistaken
    // for an open/closed-state constraint. Otherwise the KB-77 default is omitted
    // and closed issues are imported again.
    void testGitHubQueryWithTypeQualifierStillRequestsOpenItemsOnly() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        QString sentQuery;
        serveHttpFull(server, [&sentQuery](const QByteArray&, const QByteArray& target,
                                           const QByteArray&, const QByteArray&) {
            const QUrlQuery query(QUrl(QString::fromUtf8(target)).query());
            sentQuery = query.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
            return httpResponse("200 OK", "application/json",
                                "{\"total_count\":0,\"incomplete_results\":false,\"items\":[]}");
        });

        TodoListEntry entry;
        entry.id = QStringLiteral("kb84_gh");
        entry.provider = QStringLiteral("GitHub");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.apiToken = QStringLiteral("token");
        entry.jiraJql = QStringLiteral("owner/repo is:issue");

        IssueTrackerService service;
        service.syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(!sentQuery.isEmpty(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!service.isSyncing(), 5000);

        const bool hasOpenState = sentQuery.contains(QStringLiteral("is:open"))
            || sentQuery.contains(QStringLiteral("state:open"));
        const bool hasRepository = sentQuery.contains(QStringLiteral("repo:owner/repo"));
        QVERIFY2(hasRepository && hasOpenState, qPrintable(sentQuery));
        server.close();
    }

    // KB-85: TUI duration input multiplies parsed minutes in int before qBound.
    // A large valid integer can wrap negative and select the one-minute minimum
    // instead of the documented 24-hour maximum.
    void testTuiLargeDurationInputClampsWithoutOverflow() {
        AppController controller(QString(), m_testSandbox.filePath(QStringLiteral("kb85_tui.db")));

        int fds[2];
        QCOMPARE(::pipe(fds), 0);
        const int savedStdin = ::dup(STDIN_FILENO);
        const int savedStdout = ::dup(STDOUT_FILENO);
        const int devNull = ::open("/dev/null", O_WRONLY);
        QVERIFY(savedStdin >= 0 && savedStdout >= 0 && devNull >= 0);
        ::dup2(fds[0], STDIN_FILENO);
        ::dup2(devNull, STDOUT_FILENO);

        {
            TuiApp tui(&controller);
            auto feed = [&](const QByteArray& bytes) {
                QCOMPARE(::write(fds[1], bytes.constData(), static_cast<size_t>(bytes.size())),
                         static_cast<ssize_t>(bytes.size()));
                for (int i = 0; i < 8; ++i) {
                    int pending = 0;
                    ::ioctl(STDIN_FILENO, FIONREAD, &pending);
                    if (pending <= 0) break;
                    QMetaObject::invokeMethod(&tui, "onStdinActivated", Qt::DirectConnection);
                }
            };
            feed("O");       // settings, row 0: task duration
            feed("\r");      // edit the value
            feed("\x15");    // clear the pre-filled minutes
            feed("50000000");
            feed("\r");
        }

        std::fflush(stdout);
        ::dup2(savedStdin, STDIN_FILENO);
        ::dup2(savedStdout, STDOUT_FILENO);
        ::close(savedStdin);
        ::close(savedStdout);
        ::close(devNull);
        ::close(fds[0]);
        ::close(fds[1]);

        QCOMPARE(controller.settings()->taskDuration(), 86400);
    }

    // KB-78: the TUI quit commands ("q", ":q", ":wq", ":x") call QCoreApplication::quit()
    // without stopping a running work session. The GUI stops the timer before quitting (Ctrl+Q,
    // tray "Close"), but AppController has no destructor/aboutToQuit handler that does it, so in
    // the TUI the session in progress is never passed to onTimerStopped: no activity record, no
    // CSV log line, no worklog, and the seconds since the last per-minute flush are lost.
    // The test drives the real TuiApp through a pipe on stdin and types ":q" while timing.
    void testTuiQuitLogsRunningSession() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb78_tui.db"));
        QFile::remove(dbPath);
        AppController ctrl(QString(), dbPath);
        const QString listId = ctrl.listManager()->addList(QStringLiteral("KB78"));
        ctrl.selectListById(listId);
        while (ctrl.todoProxyModel()->rowCount() > 0) QVERIFY(ctrl.removeTask(false, 0));
        ctrl.addNewTask(QStringLiteral("KB78 timed task"));
        ctrl.setActiveTodoRow(0);
        ctrl.timer()->start();
        QVERIFY(ctrl.timer()->isRunning());
        QTest::qWait(1600);

        int fds[2];
        QCOMPARE(::pipe(fds), 0);
        const int savedStdin = ::dup(STDIN_FILENO);
        const int savedStdout = ::dup(STDOUT_FILENO);
        const int devNull = ::open("/dev/null", O_WRONLY);
        ::dup2(fds[0], STDIN_FILENO);
        ::dup2(devNull, STDOUT_FILENO);
        {
            TuiApp tui(&ctrl);
            const QByteArray keys(":q\r");
            QCOMPARE(::write(fds[1], keys.constData(), static_cast<size_t>(keys.size())), static_cast<ssize_t>(keys.size()));
            for (int i = 0; i < 4; ++i) {
                int pending = 0;
                ::ioctl(STDIN_FILENO, FIONREAD, &pending);
                if (pending <= 0) break;
                QMetaObject::invokeMethod(&tui, "onStdinActivated", Qt::DirectConnection);
            }
        }
        std::fflush(stdout);
        ::dup2(savedStdin, STDIN_FILENO);
        ::dup2(savedStdout, STDOUT_FILENO);
        ::close(savedStdin);
        ::close(savedStdout);
        ::close(devNull);
        ::close(fds[0]);
        ::close(fds[1]);

        // At this point go-for-it-tui would leave app.exec() and destroy the controller.
        const bool stillRunning = ctrl.timer()->isRunning();
        bool logged = false;
        for (const ActivityRecord& r : DatabaseManager::instance().loadActivityLogs()) {
            if (r.taskDescription.contains(QStringLiteral("KB78 timed task"))) logged = true;
        }
        ctrl.timer()->stop();
        QVERIFY2(logged && !stillRunning, qPrintable(QStringLiteral("running=%1 logged=%2").arg(stillRunning).arg(logged)));
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-79: TuiApp::openListPicker pre-checks the current list in m_selectedListIds, and
    // handleListPickerKey("Enter") applies m_selectedListIds before it looks at the highlighted
    // row. Moving the cursor to another list with j/k and pressing Enter (the obvious way to
    // switch lists; Space is only needed for multi-selection) therefore re-selects the list that
    // was already open instead of the highlighted one.
    void testTuiListPickerEnterOpensHighlightedList() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb79_tui.db"));
        QFile::remove(dbPath);
        AppController ctrl(QString(), dbPath);
        const QString listA = ctrl.listManager()->addList(QStringLiteral("KB79 A"));
        const QString listB = ctrl.listManager()->addList(QStringLiteral("KB79 B"));
        const int rowA = ctrl.listManager()->indexOfId(listA);
        const int rowB = ctrl.listManager()->indexOfId(listB);
        QCOMPARE(rowB, rowA + 1);
        ctrl.selectListById(listA);

        int fds[2];
        QCOMPARE(::pipe(fds), 0);
        const int savedStdin = ::dup(STDIN_FILENO);
        const int savedStdout = ::dup(STDOUT_FILENO);
        const int devNull = ::open("/dev/null", O_WRONLY);
        ::dup2(fds[0], STDIN_FILENO);
        ::dup2(devNull, STDOUT_FILENO);
        {
            TuiApp tui(&ctrl);
            auto feed = [&](const QByteArray& bytes) {
                QCOMPARE(::write(fds[1], bytes.constData(), static_cast<size_t>(bytes.size())), static_cast<ssize_t>(bytes.size()));
                for (int i = 0; i < 4; ++i) {
                    int pending = 0;
                    ::ioctl(STDIN_FILENO, FIONREAD, &pending);
                    if (pending <= 0) break;
                    QMetaObject::invokeMethod(&tui, "onStdinActivated", Qt::DirectConnection);
                }
            };
            feed("L");      // list picker, cursor on the current list A
            feed("j");      // highlight B
            feed("\r");     // open it
        }
        std::fflush(stdout);
        ::dup2(savedStdin, STDIN_FILENO);
        ::dup2(savedStdout, STDOUT_FILENO);
        ::close(savedStdin);
        ::close(savedStdout);
        ::close(devNull);
        ::close(fds[0]);
        ::close(fds[1]);

        QCOMPARE(ctrl.currentListId(), listB);
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-80: the TUI preview pane (shown under the task list, toggled with "p") writes its
    // lines without truncating them to the terminal width: the first line prints the full task
    // title ("  <title>  [status]  [(A)]"), followed by the browser-URL line and the comment
    // author lines. The task list itself truncates the title, but a long title (a typical Jira
    // summary) makes the preview header wrap onto extra rows and the frame shifts, as in KB-70.
    // The test runs the real TuiApp on a 30x120 pseudo-terminal with a 200-character title.
    void testTuiPreviewPaneKeepsLongTitleInsideTerminalWidth() {
        AppController ctrl(QString(), m_testSandbox.filePath(QStringLiteral("kb80_tui.db")));
        const QString listId = ctrl.listManager()->addList(QStringLiteral("KB80"));
        ctrl.selectListById(listId);
        while (ctrl.todoProxyModel()->rowCount() > 0) QVERIFY(ctrl.removeTask(false, 0));
        const QString longTitle = QStringLiteral("KB80") + QString(196, QLatin1Char('t'));
        ctrl.addNewTask(longTitle);
        QCOMPARE(ctrl.todoProxyModel()->rowCount(), 1);

        const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
        QVERIFY(master >= 0);
        QCOMPARE(::grantpt(master), 0);
        QCOMPARE(::unlockpt(master), 0);
        const int slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY);
        QVERIFY(slave >= 0);
        struct winsize ws {};
        ws.ws_row = 30;
        ws.ws_col = 120;
        ::ioctl(slave, TIOCSWINSZ, &ws);
        ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
        QByteArray captured;
        auto drain = [&]() {
            char buf[8192];
            for (;;) {
                const ssize_t n = ::read(master, buf, sizeof(buf));
                if (n <= 0) break;
                captured.append(buf, static_cast<int>(n));
            }
        };
        const int savedStdin = ::dup(STDIN_FILENO);
        const int savedStdout = ::dup(STDOUT_FILENO);
        std::fflush(stdout);
        ::dup2(slave, STDIN_FILENO);
        ::dup2(slave, STDOUT_FILENO);
        bool started = false;
        bool previewShown = false;
        {
            TuiApp tui(&ctrl);
            started = tui.start();
            drain();
            if (started) {
                auto frame = [&]() {
                    captured.clear();
                    QMetaObject::invokeMethod(&tui, "render", Qt::DirectConnection);
                    drain();
                    return QString::fromUtf8(captured);
                };
                QString f = frame();
                previewShown = f.contains(QStringLiteral("Preview")) || f.contains(QStringLiteral("──"));
                if (!f.contains(QStringLiteral("(p:"))) {
                    (void)::write(master, "p", 1);           // make sure the preview pane is on
                    QTest::qWait(20);
                    int pending = 0;
                    ::ioctl(STDIN_FILENO, FIONREAD, &pending);
                    if (pending > 0) QMetaObject::invokeMethod(&tui, "onStdinActivated", Qt::DirectConnection);
                    drain();
                    f = frame();
                }
                previewShown = f.contains(QStringLiteral("(p:"));
            }
        }
        drain();
        ::dup2(savedStdin, STDIN_FILENO);
        ::dup2(savedStdout, STDOUT_FILENO);
        ::close(savedStdin);
        ::close(savedStdout);
        ::close(slave);
        ::close(master);
        QVERIFY(started);
        QVERIFY2(previewShown, "preview pane is not visible");

        QString screen = QString::fromUtf8(captured);
        const int lastFrame = screen.lastIndexOf(QStringLiteral("\x1b[H"));
        if (lastFrame >= 0) screen = screen.mid(lastFrame);
        static const QRegularExpression ansiRe(QStringLiteral("\x1b\\[[0-9;?]*[ -/]*[@-~]"));
        screen.remove(ansiRe);
        QVERIFY2(!screen.contains(longTitle), "the 200-column title was written in one line on a 120-column terminal");
    }

    // KB-81: DashboardView draws the activity heatmap as columns of 7 consecutive entries from
    // getActivityHeatmap(91, ...) and labels rows 0, 2 and 4 as "Mon", "Wed" and "Fri". But
    // getActivityHeatmap starts at today - 90 days, whatever weekday that is, so the rows only
    // match the labels when that day happens to be a Monday (1 day in 7). On the other days every
    // square sits next to the wrong weekday label. (The labels are also not translatable.)
    void testHeatmapColumnsStartOnMonday() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb81.db"));
        QFile::remove(dbPath);
        QVERIFY(DatabaseManager::instance().initialize(dbPath));
        AnalyticsService analytics;
        const QVariantList heat = analytics.getActivityHeatmap(91, QStringLiteral("__ALL__"));
        QVERIFY(!heat.isEmpty());
        const int firstDow = heat.first().toMap().value(QStringLiteral("dayOfWeek")).toInt();
        if (firstDow == 1) {
            DatabaseManager::instance().close();
            QSKIP("Today the 91-day window happens to start on a Monday; misalignment not reproducible today");
        }
        bool aligned = true;
        for (int i = 0; i < heat.size(); ++i) {
            if (heat.at(i).toMap().value(QStringLiteral("dayOfWeek")).toInt() != (i % 7) + 1) {
                aligned = false;
                break;
            }
        }
        QVERIFY2(aligned, qPrintable(QStringLiteral("first day of week = %1").arg(firstDow)));
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-82: parseRedmineIssuesJson replaces the status name of every closed Redmine issue
    // (`status.is_closed == true`) with the literal "Closed" and sets no status category. The
    // per-list status mapping, which the list dialog fills with the real Redmine status names
    // from /issue_statuses.json ("Rejected", "Resolved", "Won't fix"...), is therefore never
    // consulted for closed issues: a user mapping such as "Rejected -> canceled" has no effect and
    // every closed issue becomes "done". This shows as soon as the list query also returns
    // closed issues (e.g. status_id=*).
    void testRedmineClosedStatusKeepsNameForMapping() {
        const QByteArray json =
            "{\"issues\":[{\"id\":7,\"subject\":\"KB82 rejected\",\"project\":{\"id\":1,\"name\":\"P\",\"identifier\":\"p\"},"
            "\"status\":{\"id\":6,\"name\":\"Rejected\",\"is_closed\":true},\"priority\":{\"id\":2,\"name\":\"Normal\"}}],"
            "\"total_count\":1,\"offset\":0,\"limit\":100}";
        const QVector<RemoteIssueItem> items = IssueTrackerService::parseRedmineIssuesJson(json);
        QCOMPARE(items.size(), 1);
        const QVariantMap mapping{{QStringLiteral("Rejected"), QStringLiteral("canceled")}};
        const QString local = IssueTrackerService::mapRemoteStatusToLocal(
            items.first().remoteStatus, items.first().remoteStatusCategory, mapping);
        QCOMPARE(local, QStringLiteral("canceled"));
    }
};

QTEST_MAIN(TestTodoTxt)
#include "tst_todotxt.moc"
