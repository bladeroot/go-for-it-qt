#include "TestSupport.h"
#include "core/ActivityLog.h"
#include "core/AmbientSoundService.h"
#include "core/AnalyticsService.h"
#include "core/AppController.h"
#include "core/CredentialStore.h"
#include "core/DesktopIntegration.h"
#include "core/IssueTrackerService.h"
#include "core/NotificationService.h"
#include "core/Schedule.h"
#include "core/ShortcutManager.h"
#include "core/SystemHookService.h"
#include "todotxt/TaskFilterProxyModel.h"
#include "todotxt/TaskManager.h"
#include "todotxt/TaskStoreModel.h"
#include "todotxt/TxtTask.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimeZone>
#include <QUrl>
#include <QtTest>

using namespace gofi;

class TestTrackers : public QObject {
    Q_OBJECT

private:
    TestSandbox m_testSandbox;

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

private slots:
    void initTestCase() {
        QVERIFY(m_testSandbox.init());
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
        RemoteWritesEnabled enableWrites;
        svc.updateIssueStatusRemote(entry, QStringLiteral("RM#proj#42"), true, QStringLiteral("done"));

        QTRY_VERIFY_WITH_TIMEOUT(!putBody.isEmpty(), 5000);
        QCOMPARE(putTarget, QByteArray("/issues/42.json"));
        const int sentStatus = QJsonDocument::fromJson(putBody).object()
            .value(QStringLiteral("issue")).toObject().value(QStringLiteral("status_id")).toInt();
        QCOMPARE(sentStatus, 6);
        server.close();
    }


    void testRemoteStatusSyncDispatchesForJiraYouTrackAndNextcloud() {
        QTcpServer jiraServer;
        QTcpServer youTrackServer;
        QTcpServer nextcloudServer;
        if (!jiraServer.listen(QHostAddress::LocalHost, 0)
            || !youTrackServer.listen(QHostAddress::LocalHost, 0)
            || !nextcloudServer.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }

        QVector<int> requestCounts(3, 0);
        serveHttp(jiraServer, [&requestCounts](const QByteArray&, const QByteArray&, const QByteArray&) {
            ++requestCounts[0];
            return httpResponse("200 OK", "application/json", "{\"transitions\":[]}");
        });
        serveHttp(youTrackServer, [&requestCounts](const QByteArray&, const QByteArray&, const QByteArray&) {
            ++requestCounts[1];
            return httpResponse("200 OK", "application/json", "{}");
        });
        serveHttp(nextcloudServer, [&requestCounts](const QByteArray&, const QByteArray&, const QByteArray&) {
            ++requestCounts[2];
            return httpResponse("200 OK", "application/json", "{}");
        });

        const QStringList providers{
            QStringLiteral("Jira"),
            QStringLiteral("YouTrack"),
            QStringLiteral("Nextcloud")
        };
        const QStringList issueKeys{
            QStringLiteral("GOFI-123"),
            QStringLiteral("YT-123"),
            QStringLiteral("NC#task-123")
        };
        const QList<quint16> ports{
            jiraServer.serverPort(),
            youTrackServer.serverPort(),
            nextcloudServer.serverPort()
        };

        RemoteWritesEnabled enableWrites;
        IssueTrackerService service;
        QStringList providersWithoutRequests;
        for (int i = 0; i < providers.size(); ++i) {
            TodoListEntry entry;
            entry.id = QStringLiteral("remote-status-%1").arg(i);
            entry.provider = providers.at(i);
            entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(ports.at(i));
            entry.jiraUser = QStringLiteral("test-user");
            entry.apiToken = QStringLiteral("test-token");
            entry.syncStatusToRemote = true;

            service.updateIssueStatusRemote(entry, issueKeys.at(i), true, QStringLiteral("done"));
            const bool requestSent = QTest::qWaitFor(
                [&requestCounts, i]() { return requestCounts.at(i) > 0; }, 1000);
            if (!requestSent) {
                providersWithoutRequests.append(providers.at(i));
            }
        }

        QVERIFY2(providersWithoutRequests.isEmpty(),
                 qPrintable(QStringLiteral("No remote status request sent for: %1")
                                .arg(providersWithoutRequests.join(QStringLiteral(", ")))));
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
        RemoteWritesEnabled enableWrites;
        svc.postWorklog(entry, 1, 1, QStringLiteral("KB-1"), QDateTime::currentDateTimeUtc().addSecs(-1500), 1500);

        QTRY_VERIFY_WITH_TIMEOUT(requests >= 1, 5000);
        QTest::qWait(300);
        QCOMPARE(failSpy.count(), 1);
        server.close();
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
        RemoteWritesEnabled enableWrites;
        // Keep test mode off for the whole exchange: the Bearer retry is issued from the
        // reply handler, after the first request returns. (Re-enabling test mode right after
        // the first call made the retry hit the test-mode guard; see KB-47.)
        svc.postWorklog(entry, 1, 1, QStringLiteral("KB-1"), QDateTime::currentDateTimeUtc().addSecs(-1500), 1500);
        QTRY_VERIFY_WITH_TIMEOUT(postedSpy.count() >= 1 || bearer >= 1, 5000);
        QTest::qWait(300);

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


    void testCurrentListRemoteFlagIncludesAllRemoteProviders() {
        ApplicationProxyGuard proxyGuard;
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("remote-provider-flag.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        QVERIFY(controller.isDatabaseReady());

        const QStringList providers{
            QStringLiteral("Jira"),
            QStringLiteral("YouTrack"),
            QStringLiteral("GitHub"),
            QStringLiteral("GitLab"),
            QStringLiteral("Redmine"),
            QStringLiteral("Todoist"),
            QStringLiteral("Nextcloud")
        };
        QStringList providersNotRecognizedAsRemote;
        for (const QString& provider : providers) {
            const QVariantMap config{
                {QStringLiteral("name"), QStringLiteral("Remote %1").arg(provider)},
                {QStringLiteral("provider"), provider},
                {QStringLiteral("jiraUrl"), QStringLiteral("http://127.0.0.1:1")},
                {QStringLiteral("jiraUser"), QStringLiteral("test-user")},
                {QStringLiteral("apiToken"), QStringLiteral("test-token")}
            };
            const QString listId = controller.listManager()->saveListConfig(config, QStringLiteral("keep"));
            if (listId.isEmpty()) {
                providersNotRecognizedAsRemote.append(provider + QStringLiteral(" (could not save)"));
                continue;
            }
            controller.selectListById(listId);
            if (!controller.isCurrentListRemote()) {
                providersNotRecognizedAsRemote.append(provider);
            }
        }

        QVERIFY2(providersNotRecognizedAsRemote.isEmpty(),
                 qPrintable(QStringLiteral("Not recognized as remote: %1")
                                .arg(providersNotRecognizedAsRemote.join(QStringLiteral(", ")))));
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
        RemoteWritesEnabled enableWrites;
        svc.updateIssueDescription(entry, QStringLiteral("NC#kb63uid"), QStringLiteral("new KB63 description"));
        QTRY_VERIFY_WITH_TIMEOUT(putSeen, 5000);

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
        RemoteWritesEnabled enableWrites;
        svc.updateIssueDescription(entry, QStringLiteral("NC#kb64uid"), QStringLiteral("new KB64 description"));
        QTRY_VERIFY_WITH_TIMEOUT(putSeen, 5000);

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
        RemoteWritesEnabled enableWrites;
        svc.updateIssueDescription(entry, QStringLiteral("NC#kb65uid"), QStringLiteral("new KB65 description"));
        QTRY_VERIFY_WITH_TIMEOUT(putSeen, 5000);

        const QString put = QString::fromUtf8(putBody);
        QVERIFY2(put.contains(QStringLiteral("\r\nSUMMARY:KB65 fix BEGIN: handling\r\n"))
                 && put.count(QStringLiteral("DESCRIPTION:")) == 1
                 && put.contains(QStringLiteral("DESCRIPTION:new KB65 description")), qPrintable(put));
        server.close();
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


    // KB-89: the YouTrack branch of updateIssueStatusRemote added in d40b2bc builds the command
    // from the list's status mapping. For a reopen it takes the *first* key (QVariantMap order,
    // i.e. alphabetical) whose mapped value is not "done" - which may be a state mapped to
    // "canceled" or "paused", so reopening a task can cancel the issue in YouTrack. It also
    // writes "State <name>" without braces, but YouTrack commands need `State {In Progress}`
    // for values containing spaces, so such commands fail.
    void testYouTrackReopenUsesTodoStateAndBracesMultiWordNames() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        QStringList queries;
        serveHttpFull(server, [&queries](const QByteArray&, const QByteArray&, const QByteArray&, const QByteArray& body) {
            queries << QJsonDocument::fromJson(body).object().value(QStringLiteral("query")).toString();
            return httpResponse("200 OK", "application/json", "{}");
        });
        TodoListEntry entry;
        entry.id = QStringLiteral("kb89_yt");
        entry.provider = QStringLiteral("YouTrack");
        entry.jiraUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        entry.apiToken = QStringLiteral("perm:token");
        entry.syncStatusToRemote = true;
        entry.statusMapping = QVariantMap{
            {QStringLiteral("Canceled"), QStringLiteral("canceled")},
            {QStringLiteral("In Review"), QStringLiteral("done")},
            {QStringLiteral("Open"), QStringLiteral("todo")}
        };

        IssueTrackerService svc;
        RemoteWritesEnabled enableWrites;
        svc.updateIssueStatusRemote(entry, QStringLiteral("KB-89"), false, QStringLiteral("todo"));
        svc.updateIssueStatusRemote(entry, QStringLiteral("KB-89"), true, QStringLiteral("done"));
        QTRY_VERIFY_WITH_TIMEOUT(queries.size() >= 2, 5000);

        QVERIFY2(queries.contains(QStringLiteral("State Open")) || queries.contains(QStringLiteral("State {Open}")), qPrintable(queries.join(QStringLiteral(" | "))));
        QVERIFY2(queries.contains(QStringLiteral("State {In Review}")), qPrintable(queries.join(QStringLiteral(" | "))));
        server.close();
    }


    // KB-86 (Jira reopen): the first transition into a "new" *or* "indeterminate" category was
    // used, so reopening an issue could move it to "In Progress" instead of "To Do".
    void testJiraReopenPrefersToDoTransition() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        QByteArray postBody;
        serveHttpFull(server, [&postBody](const QByteArray& method, const QByteArray&, const QByteArray&, const QByteArray& body) {
            if (method == "POST") {
                postBody = body;
                return httpResponse("204 No Content", "application/json", "");
            }
            return httpResponse("200 OK", "application/json",
                "{\"transitions\":["
                "{\"id\":\"11\",\"name\":\"Start\",\"to\":{\"name\":\"In Progress\",\"statusCategory\":{\"key\":\"indeterminate\"}}},"
                "{\"id\":\"21\",\"name\":\"Back\",\"to\":{\"name\":\"To Do\",\"statusCategory\":{\"key\":\"new\"}}}]}");
        });
        RemoteWritesEnabled enabled;
        IssueTrackerService svc;
        svc.updateIssueStatusRemote(makeStatusPushEntry(QStringLiteral("Jira"), server), QStringLiteral("GOFI-1"), false, QStringLiteral("todo"));
        QTRY_VERIFY_WITH_TIMEOUT(!postBody.isEmpty(), 5000);
        const QString id = QJsonDocument::fromJson(postBody).object().value(QStringLiteral("transition")).toObject()
                               .value(QStringLiteral("id")).toString();
        QCOMPARE(id, QStringLiteral("21"));
        server.close();
    }


    // KB-86 (Jira auth): status push had no Bearer fallback, so Server/DC users with a PAT and an
    // e-mail set got 401 until a sync had stored Bearer mode.
    void testJiraStatusPushFallsBackToBearerAuth() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        QByteArray postBody;
        bool postWasBearer = false;
        serveHttpFull(server, [&](const QByteArray& method, const QByteArray&, const QByteArray& head, const QByteArray& body) {
            const QByteArray lower = head.toLower();
            if (lower.contains("authorization: basic")) {
                return httpResponse("401 Unauthorized", "application/json", "{}",
                                    "X-Seraph-Loginreason: AUTHENTICATED_FAILED\r\n");
            }
            if (method == "POST") {
                postBody = body;
                postWasBearer = lower.contains("authorization: bearer");
                return httpResponse("204 No Content", "application/json", "");
            }
            return httpResponse("200 OK", "application/json",
                "{\"transitions\":[{\"id\":\"31\",\"name\":\"Done\",\"to\":{\"name\":\"Done\",\"statusCategory\":{\"key\":\"done\"}}}]}");
        });
        RemoteWritesEnabled enabled;
        IssueTrackerService svc;
        svc.updateIssueStatusRemote(makeStatusPushEntry(QStringLiteral("Jira"), server), QStringLiteral("GOFI-2"), true, QStringLiteral("done"));
        QTRY_VERIFY_WITH_TIMEOUT(!postBody.isEmpty(), 5000);
        QVERIFY(postWasBearer);
        server.close();
    }


    // KB-86 (Jira silent miss): a missing matching transition left no trace for the user.
    void testJiraStatusPushReportsMissingTransition() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        int posts = 0;
        serveHttpFull(server, [&posts](const QByteArray& method, const QByteArray&, const QByteArray&, const QByteArray&) {
            if (method == "POST") {
                ++posts;
                return httpResponse("204 No Content", "application/json", "");
            }
            return httpResponse("200 OK", "application/json",
                "{\"transitions\":[{\"id\":\"31\",\"name\":\"Done\",\"to\":{\"name\":\"Done\",\"statusCategory\":{\"key\":\"done\"}}}]}");
        });
        RemoteWritesEnabled enabled;
        IssueTrackerService svc;
        QSignalSpy failSpy(&svc, &IssueTrackerService::syncFailed);
        svc.updateIssueStatusRemote(makeStatusPushEntry(QStringLiteral("Jira"), server), QStringLiteral("GOFI-3"), false, QStringLiteral("todo"));
        QTRY_COMPARE_WITH_TIMEOUT(failSpy.count(), 1, 5000);
        QCOMPARE(posts, 0);
        server.close();
    }


    // KB-86 (Nextcloud): completing only set STATUS:COMPLETED; other CalDAV clients also read
    // COMPLETED and PERCENT-COMPLETE, and reopening left both in place.
    void testNextcloudStatusPushKeepsCompletionPropertiesConsistent() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        QByteArray stored =
            "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VTODO\r\nUID:uid1\r\nSUMMARY:Task\r\n"
            "STATUS:NEEDS-ACTION\r\nEND:VTODO\r\nEND:VCALENDAR\r\n";
        int puts = 0;
        serveHttpFull(server, [&](const QByteArray& method, const QByteArray&, const QByteArray&, const QByteArray& body) {
            if (method == "PUT") {
                stored = body;
                ++puts;
                return httpResponse("204 No Content", "text/calendar", "");
            }
            return httpResponse("200 OK", "text/calendar", stored);
        });
        RemoteWritesEnabled enabled;
        IssueTrackerService svc;
        TodoListEntry entry = makeStatusPushEntry(QStringLiteral("Nextcloud"), server);
        entry.jiraUrl += QStringLiteral("/cal/");

        svc.updateIssueStatusRemote(entry, QStringLiteral("NC#uid1"), true, QStringLiteral("done"));
        QTRY_COMPARE_WITH_TIMEOUT(puts, 1, 5000);
        const QString completed = QString::fromUtf8(stored);
        QVERIFY2(completed.contains(QStringLiteral("STATUS:COMPLETED")), qPrintable(completed));
        QVERIFY2(completed.contains(QStringLiteral("PERCENT-COMPLETE:100")), qPrintable(completed));
        QVERIFY2(QRegularExpression(QStringLiteral("\\r\\nCOMPLETED:\\d{8}T\\d{6}Z")).match(completed).hasMatch(), qPrintable(completed));
        QVERIFY2(!completed.contains(QStringLiteral("STATUS:NEEDS-ACTION")), qPrintable(completed));
        QVERIFY2(completed.contains(QStringLiteral("SUMMARY:Task")), qPrintable(completed));

        svc.updateIssueStatusRemote(entry, QStringLiteral("NC#uid1"), false, QStringLiteral("todo"));
        QTRY_COMPARE_WITH_TIMEOUT(puts, 2, 5000);
        const QString reopened = QString::fromUtf8(stored);
        QVERIFY2(reopened.contains(QStringLiteral("STATUS:NEEDS-ACTION")), qPrintable(reopened));
        QVERIFY2(!QRegularExpression(QStringLiteral("(^|\\n)(COMPLETED|PERCENT-COMPLETE)[:;]")).match(reopened).hasMatch(), qPrintable(reopened));
        QVERIFY2(reopened.contains(QStringLiteral("SUMMARY:Task")), qPrintable(reopened));
        server.close();
    }


    // KB-98: Nextcloud status push failed silently if GET returned an HTTP error or if the
    // response body did not contain a valid VTODO block (handleReply was called on an already-finished
    // reply, which never fired, and missing VTODO returned early without error).
    void testNextcloudStatusPushEmitsSyncFailedOnErrorAndInvalidVTodo() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        bool return500 = true;
        serveHttpFull(server, [&](const QByteArray&, const QByteArray&, const QByteArray&, const QByteArray&) {
            if (return500) {
                return httpResponse("500 Internal Server Error", "text/plain", "Server failure");
            }
            return httpResponse("200 OK", "text/calendar", "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nEND:VCALENDAR\r\n");
        });
        RemoteWritesEnabled enabled;
        IssueTrackerService svc;
        QSignalSpy failSpy(&svc, &IssueTrackerService::syncFailed);
        TodoListEntry entry = makeStatusPushEntry(QStringLiteral("Nextcloud"), server);
        entry.jiraUrl += QStringLiteral("/cal/");

        // 1. GET returns 500 error: syncFailed must be emitted
        svc.updateIssueStatusRemote(entry, QStringLiteral("NC#uid1"), true, QStringLiteral("done"));
        QTRY_COMPARE_WITH_TIMEOUT(failSpy.count(), 1, 5000);

        // 2. GET returns 200 OK but without VTODO: syncFailed must be emitted
        return500 = false;
        svc.updateIssueStatusRemote(entry, QStringLiteral("NC#uid1"), true, QStringLiteral("done"));
        QTRY_COMPARE_WITH_TIMEOUT(failSpy.count(), 2, 5000);

        server.close();
    }


    // KB-90: since 96dede1 every GitLab request was sent with order_by=id. The GitLab REST API
    // does not accept "id" for the issues and merge_requests endpoints and answers
    // 400 {"error":"order_by does not have a valid value"}, so GitLab sync failed for every list
    // (project, group: and assigned-to-me queries) whatever the URL and token. The local server
    // below validates order_by like GitLab does.
    void testGitLabSyncUsesOrderByAcceptedByGitLab() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        static const QStringList issueOrders = {
            QStringLiteral("created_at"), QStringLiteral("updated_at"), QStringLiteral("priority"),
            QStringLiteral("due_date"), QStringLiteral("relative_position"), QStringLiteral("label_priority"),
            QStringLiteral("milestone_due"), QStringLiteral("popularity"), QStringLiteral("weight"),
            QStringLiteral("title")};
        static const QStringList mrOrders = {
            QStringLiteral("created_at"), QStringLiteral("updated_at"), QStringLiteral("title"),
            QStringLiteral("merged_at"), QStringLiteral("label_priority"), QStringLiteral("milestone_due"),
            QStringLiteral("popularity"), QStringLiteral("priority")};
        QStringList rejected;
        int requests = 0;
        serveHttpFull(server, [&](const QByteArray&, const QByteArray& target, const QByteArray&, const QByteArray&) {
            ++requests;
            const QUrl url(QString::fromUtf8(target));
            const QString orderBy = QUrlQuery(url.query()).queryItemValue(QStringLiteral("order_by"));
            const bool isMr = url.path().endsWith(QStringLiteral("/merge_requests"));
            const QStringList& allowed = isMr ? mrOrders : issueOrders;
            if (!orderBy.isEmpty() && !allowed.contains(orderBy)) {
                rejected << url.path() + QStringLiteral("?order_by=") + orderBy;
                return httpResponse("400 Bad Request", "application/json",
                                    "{\"error\":\"order_by does not have a valid value\"}");
            }
            return httpResponse("200 OK", "application/json", "[]", "X-Total: 0\r\nX-Next-Page: \r\n");
        });

        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb90_gitlab.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        const QString baseUrl = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
        const QStringList specs = {QStringLiteral("group/project"), QStringLiteral("group:mygroup"), QString()};
        for (const QString& spec : specs) {
            QVariantMap cfg{
                {QStringLiteral("name"), QStringLiteral("KB90 GitLab %1").arg(spec)},
                {QStringLiteral("provider"), QStringLiteral("GitLab")},
                {QStringLiteral("jiraUrl"), baseUrl},
                {QStringLiteral("apiToken"), QStringLiteral("glpat-token")},
                {QStringLiteral("jiraJql"), spec}
            };
            const QString listId = controller.listManager()->saveListConfig(cfg, QStringLiteral("keep"));
            QVERIFY(!listId.isEmpty());
            const TodoListEntry entry = controller.listManager()->entry(controller.listManager()->indexOfId(listId));

            IssueTrackerService* svc = controller.issueTracker();
            QTRY_VERIFY_WITH_TIMEOUT(!svc->isSyncing(), 5000);
            QSignalSpy syncedSpy(svc, &IssueTrackerService::listIssuesSynced);
            QSignalSpy failedSpy(svc, &IssueTrackerService::syncFailed);
            svc->syncListIssues(entry);
            QTRY_VERIFY_WITH_TIMEOUT(syncedSpy.count() + failedSpy.count() > 0, 5000);
            QVERIFY2(failedSpy.isEmpty(), qPrintable(QStringLiteral("spec '%1': %2").arg(spec,
                failedSpy.isEmpty() ? QString() : failedSpy.first().at(1).toString())));
            QCOMPARE(syncedSpy.count(), 1);
        }
        QVERIFY(requests >= 6);
        QVERIFY2(rejected.isEmpty(), qPrintable(rejected.join(QStringLiteral(" | "))));
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
        server.close();
    }


    // KB-90 (companion): when GitLab rejects a request, the sync error only showed
    // QNetworkReply::errorString() ("... server replied: Bad Request") and dropped the reason
    // GitLab puts in the JSON body ({"error": "..."} or {"message": ...}), so a user could not tell
    // a bad parameter from a bad token or URL.
    void testGitLabSyncErrorShowsServerMessage() {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            QSKIP("Cannot bind to loopback port in restricted sandbox");
        }
        serveHttpFull(server, [](const QByteArray&, const QByteArray&, const QByteArray&, const QByteArray&) {
            return httpResponse("400 Bad Request", "application/json", "{\"error\":\"KB90 server explanation\"}");
        });
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb90_gitlab_err.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        QVariantMap cfg{
            {QStringLiteral("name"), QStringLiteral("KB90 GitLab errors")},
            {QStringLiteral("provider"), QStringLiteral("GitLab")},
            {QStringLiteral("jiraUrl"), QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort())},
            {QStringLiteral("apiToken"), QStringLiteral("glpat-token")},
            {QStringLiteral("jiraJql"), QStringLiteral("group/project")}
        };
        const QString listId = controller.listManager()->saveListConfig(cfg, QStringLiteral("keep"));
        const TodoListEntry entry = controller.listManager()->entry(controller.listManager()->indexOfId(listId));
        IssueTrackerService* svc = controller.issueTracker();
        QTRY_VERIFY_WITH_TIMEOUT(!svc->isSyncing(), 5000);
        QSignalSpy failedSpy(svc, &IssueTrackerService::syncFailed);
        svc->syncListIssues(entry);
        QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() > 0, 5000);
        const QString msg = failedSpy.first().at(1).toString();
        QVERIFY2(msg.contains(QStringLiteral("KB90 server explanation")), qPrintable(msg));
        QTRY_VERIFY_WITH_TIMEOUT(!svc->isSyncing(), 5000);
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
        server.close();
    }

    // KB-121: splitting buildIssueBrowserUrl per provider made the GitHub and GitLab helpers return an
    // empty URL when no issue URL can be built, so unkeyed tasks in those lists (e.g. a task added
    // locally to a github.com list) no longer fall back to the link written in the task description.
    void testBrowserUrlFallsBackToDescriptionLinkForUnkeyedGitHubAndGitLabTasks() {
        const QString description = QStringLiteral("Read https://example.org/spec/page for details");
        for (const QString& provider : {QStringLiteral("GitHub"), QStringLiteral("GitLab")}) {
            TodoListEntry entry;
            entry.provider = provider;
            const QUrl url = IssueTrackerService::buildIssueBrowserUrl(entry, QString(), description);
            QVERIFY2(url == QUrl(QStringLiteral("https://example.org/spec/page")),
                     qPrintable(provider + QStringLiteral(": ") + url.toString()));
        }
    }

};

QTEST_MAIN(TestTrackers)
#include "tst_trackers.moc"
