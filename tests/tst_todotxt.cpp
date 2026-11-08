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
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace gofi;

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

        connect(&server, &QTcpServer::newConnection, [&]() {
            while (server.hasPendingConnections()) {
                QTcpSocket* socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, [socket, &requestCount, &hadBasicAuth, &hadBearerAuth]() {
                    const QByteArray req = socket->readAll();
                    if (!req.isEmpty()) {
                        requestCount++;
                        if (req.contains("Authorization: Basic")) {
                            hadBasicAuth = true;
                            const QByteArray body = "{}";
                            const QByteArray resp =
                                "HTTP/1.1 401 Unauthorized\r\n"
                                "Content-Type: application/json\r\n"
                                "X-Seraph-Loginreason: AUTHENTICATED_FAILED\r\n"
                                "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
                            socket->write(resp);
                        } else if (req.contains("Authorization: Bearer")) {
                            hadBearerAuth = true;
                            const QByteArray body = "{\"issues\":[]}";
                            const QByteArray resp =
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: application/json\r\n"
                                "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
                            socket->write(resp);
                        }
                    }
                });
            }
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
        QTRY_VERIFY_WITH_TIMEOUT(!svc.isSyncing(), 5000);
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

            // Case A: Next sync returns first 100-item page (pageLimitReached = false, but items.size() == 100)
            // MUST NOT close the remaining 50 items!
            svc.processSyncedItems(e, items150.mid(0, 100), QStringLiteral("GitHub"), false, false);
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

        const QString testDb = QStringLiteral("/tmp/gofi-timer-switch-test.db");
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

            // 2. DatabaseManager batch ordering transaction atomicity
            DatabaseManager& db = DatabaseManager::instance();
            QVector<TodoListEntry> entries = db.loadLists();
            QVERIFY(!entries.isEmpty());
            QVERIFY(db.updateListsOrder(entries));

            QVector<TxtTask> tasks = db.loadTasks(listId, false);
            QCOMPARE(tasks.size(), 2);
            QVERIFY(db.replaceListTasksOrders(listId, false, tasks));
        }

        DatabaseManager::instance().close();
        QFile::remove(testDb);
    }
};

QTEST_MAIN(TestTodoTxt)
#include "tst_todotxt.moc"
