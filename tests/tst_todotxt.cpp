#include "TestSupport.h"
#include "core/ActivityLog.h"
#include "core/AmbientSoundService.h"
#include "core/AnalyticsService.h"
#include "core/AppController.h"
#include "core/CredentialStore.h"
#include <QSet>
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
#include "tui/TuiApp.h"
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusVirtualObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDesktopServices>
#include <QNetworkProxy>
#include <QTimeZone>
#include <QWindow>
#include <QtTest>
#include "ShortcutManager.h"
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlContext>
#include <QJSEngine>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <poll.h>

using namespace gofi;

class TestTodoTxt : public QObject {
    Q_OBJECT

private:
    TestSandbox m_testSandbox;

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
        const QDate today = QDate::currentDate();
        const QDate nominalStart = today.addDays(-13);
        const QDate startDate = nominalStart.addDays(-(nominalStart.dayOfWeek() - 1));
        const int expectedDays = static_cast<int>(startDate.daysTo(today)) + 1;
        QCOMPARE(heatmap.size(), expectedDays);
        QCOMPARE(heatmap.first().toMap().value(QStringLiteral("dayOfWeek")).toInt(), 1);
        QCOMPARE(heatmap.last().toMap().value(QStringLiteral("date")).toString(), today.toString(Qt::ISODate));

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

        const QString oceanPath = m_testSandbox.filePath(QStringLiteral("test_ocean.wav"));
        QFile::remove(oceanPath);
        QVERIFY(AmbientSoundService::generateAmbientWav(QStringLiteral("ocean"), oceanPath));
        QVERIFY(QFile::exists(oceanPath));
        QVERIFY(QFileInfo(oceanPath).size() > 44);
        QFile::remove(oceanPath);

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
        QCOMPARE(firstDow, 1);
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

    // KB-99: getActivityHeatmap shifted start date to Monday but kept a rigid loop of numDays,
    // which truncated recent days when daysBack was not a multiple of 7 (e.g. 365 days ended
    // several days before today) or produced future days up to Sunday when it was a multiple of 7.
    void testActivityHeatmapRangeAndBoundaryAlignment() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb99.db"));
        QFile::remove(dbPath);
        QVERIFY(DatabaseManager::instance().initialize(dbPath));
        AnalyticsService analytics;
        const QDate today = QDate::currentDate();

        for (int daysBack : {7, 10, 14, 28, 91, 365}) {
            const QVariantList heat = analytics.getActivityHeatmap(daysBack, QStringLiteral("__ALL__"));
            QVERIFY(!heat.isEmpty());
            // First entry must be Monday
            const int firstDow = heat.first().toMap().value(QStringLiteral("dayOfWeek")).toInt();
            QCOMPARE(firstDow, 1);
            // Last entry must be today
            const QString lastDateStr = heat.last().toMap().value(QStringLiteral("date")).toString();
            QCOMPARE(lastDateStr, today.toString(Qt::ISODate));
            // All entries must be <= today (no future days)
            // And days must be strictly consecutive
            QDate prevDate = QDate::fromString(heat.first().toMap().value(QStringLiteral("date")).toString(), Qt::ISODate);
            for (int i = 1; i < heat.size(); ++i) {
                const QDate curDate = QDate::fromString(heat.at(i).toMap().value(QStringLiteral("date")).toString(), Qt::ISODate);
                QCOMPARE(curDate, prevDate.addDays(1));
                QVERIFY(curDate <= today);
                QCOMPARE(heat.at(i).toMap().value(QStringLiteral("dayOfWeek")).toInt(), (i % 7) + 1);
                prevDate = curDate;
            }
        }
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }
















    // KB-94: user-visible strings added since 2.0.1 have no key in any translation dictionary
    // (data/translations/{uk,de,fr,es,it}.json), so they are shown in English in every language.
    // The TUI ambient-mode error was renamed in 846dbfe (", ocean" added), which orphaned its
    // existing translations. The heatmap row labels Mon/Wed/Fri were never translated.
    void testStringsAddedSince201AreTranslated() {
        const QStringList keys = {
            QStringLiteral("Unknown ambient mode. Available: none, white, rain, ocean, clock"),
            QStringLiteral("Double-click or press Enter to view description & comments"),
            QStringLiteral("No matching Jira transition to update status of %1; status was not changed on %2."),
            QStringLiteral("Invalid CalDAV VTODO response"),
            QStringLiteral("Invalid number: \"%1\" (value unchanged)"),
            QStringLiteral("Mon"),
            QStringLiteral("Wed"),
            QStringLiteral("Fri")
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
        QVERIFY2(missing.isEmpty(), qPrintable(QString::number(missing.size()) + QStringLiteral(" missing: ")
                                               + missing.join(QStringLiteral(" | "))));
    }




    // KB-116: AnalyticsService::exportBillingPdf did not HTML-escape project names, causing
    // projects with angle brackets like "+Security<Core>" to be treated as HTML markup and
    // stripped from the document.
    void testPdfReportEscapesProjectNames() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb116_pdf.db"));
        QFile::remove(dbPath);
        AppController ctrl(QString(), dbPath);
        const QString list = ctrl.listManager()->addList(QStringLiteral("KB116"));
        ctrl.selectListById(list);

        const QDateTime now = QDateTime::currentDateTimeUtc();
        QVERIFY(DatabaseManager::instance().insertActivityLog(list, QStringLiteral("KB116"), -1, QString(),
                QStringLiteral("Refactor kernel +Security<Core>"), now.addSecs(-3600), 3600, now, false, false) > 0);

        const QString pdfPath = m_testSandbox.filePath(QStringLiteral("kb116_billing.pdf"));
        QFile::remove(pdfPath);
        QVERIFY(ctrl.analytics()->exportBillingPdf(pdfPath, QStringLiteral("all"), list));
        QVERIFY(QFile::exists(pdfPath));

        // In QPdfWriter output, text strings from QTextDocument appear in the rendered document.
        // If "+Security<Core>" was not HTML-escaped, "<Core>" is parsed as an HTML tag and omitted entirely.
        const QString pdftotext = QStandardPaths::findExecutable(QStringLiteral("pdftotext"));
        QVERIFY2(!pdftotext.isEmpty(), "pdftotext (poppler-utils) is required to inspect the generated PDF");
        QProcess pdfProc;
        pdfProc.start(pdftotext, {pdfPath, QStringLiteral("-")});
        QVERIFY(pdfProc.waitForFinished(10000));
        QCOMPARE(pdfProc.exitCode(), 0);
        const QString pdfText = QString::fromUtf8(pdfProc.readAllStandardOutput());
        QFile::remove(pdfPath);
        // KB-119: the task description row ("Refactor kernel +Security<Core>") is escaped independently,
        // so the project-distribution row must contribute the second occurrence.
        QVERIFY2(pdfText.count(QStringLiteral("+Security<Core>")) == 2,
                 qPrintable(QStringLiteral("Project name with angle brackets was stripped from the generated PDF: %1").arg(pdfText)));

        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

};

QTEST_MAIN(TestTodoTxt)
#include "tst_todotxt.moc"
