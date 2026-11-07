#include "core/AnalyticsService.h"
#include "core/AppController.h"
#include "core/IssueTrackerService.h"
#include "core/NotificationService.h"
#include "core/Schedule.h"
#include "core/ShortcutManager.h"
#include "todotxt/TaskFilterProxyModel.h"
#include "todotxt/TaskStoreModel.h"
#include "todotxt/TxtTask.h"
#include <QtTest>

using namespace gofi;

class TestTodoTxt : public QObject {
    Q_OBJECT

private slots:
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
        {
            AppController ctrl(csvPath, dbPath);
            listId = ctrl.listManager()->addList(QStringLiteral("SQLiteTestList"));
            ctrl.selectListById(listId);
            QCOMPARE(ctrl.currentListId(), listId);

            const int initialCount = ctrl.todoProxyModel()->rowCount();
            ctrl.addNewTask(QStringLiteral("(A) Port Go-For-It to SQLite +QtPort @dev duration:30m"));
            QCOMPARE(ctrl.todoProxyModel()->rowCount(), initialCount + 1);

            // Move task row and persist sort_order in SQLite
            ctrl.setActiveTodoRow(0);
            ctrl.moveActiveTaskByOffset(1);
            QCOMPARE(ctrl.activeTodoRow(), 1);

            QVERIFY(ctrl.timer()->hasActiveTask());
            ctrl.markActiveTaskDone();
            QCOMPARE(ctrl.doneProxyModel()->rowCount(), 1);
        }

        // Verify persistence by reloading from the same SQLite database
        const QVector<TxtTask> reloadedTodo = DatabaseManager::instance().loadTasks(listId, false);
        const QVector<TxtTask> reloadedDone = DatabaseManager::instance().loadTasks(listId, true);
        QCOMPARE(reloadedTodo.size(), 3);
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
        const QString testDbPath = QStringLiteral("/tmp/gofi_analytics_test.db");
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
        const QString csvPath = QStringLiteral("/tmp/test_billing.csv");
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
        const QString pdfPath = QStringLiteral("/tmp/test_billing.pdf");
        QFile::remove(pdfPath);
        QVERIFY(analytics.exportBillingPdf(pdfPath, QStringLiteral("all")));
        QVERIFY(QFile::exists(pdfPath));
        QVERIFY(QFileInfo(pdfPath).size() > 500);
        QFile::remove(pdfPath);
        QFile::remove(testDbPath);
    }
};

QTEST_MAIN(TestTodoTxt)
#include "tst_todotxt.moc"
