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

#include <QAction>
#include <QDBusConnection>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
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
#include <QWindow>
#include <QtTest>

using namespace gofi;

class TestQml : public QObject {
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
        const QDateTime now = QDateTime(QDate::currentDate(), QTime(12, 0, 0), QTimeZone::systemTimeZone()).toUTC();
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
        const QDateTime now = QDateTime(QDate::currentDate(), QTime(12, 0, 0), QTimeZone::systemTimeZone()).toUTC();
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


    // Mini player: MiniPlayerWindow is declared as a child of the main ApplicationWindow, so Qt
    // makes the main window its transient parent and hides it whenever the main window is hidden
    // (closing to the tray). The test loads the real built Main.qml, shows the mini player, hides
    // the main window and expects the mini player to stay visible.
    void testMiniPlayerSurvivesHidingMainWindow() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("mini_player.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        engine.rootContext()->setContextProperty(QStringLiteral("showAboutOnStart"), false);
        const QString srcRoot = QCoreApplication::applicationDirPath() + QStringLiteral("/GoForIt/qml");
        const QString qmlRoot = m_testSandbox.filePath(QStringLiteral("mini_qml"));
        QDir(qmlRoot).removeRecursively();
        QDirIterator it(srcRoot, {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            const QString dst = qmlRoot + f.mid(srcRoot.size());
            QDir().mkpath(QFileInfo(dst).absolutePath());
            QVERIFY(QFile::copy(f, dst));
        }
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlRoot + QStringLiteral("/Main.qml")));
        std::unique_ptr<QObject> root(comp.create());
        QVERIFY2(root, qPrintable(comp.errorString()));
        QWindow* mainWindow = qobject_cast<QWindow*>(root.get());
        QVERIFY(mainWindow);

        QWindow* mini = nullptr;
        for (QWindow* w : root->findChildren<QWindow*>()) {
            if (w->flags().testFlag(Qt::WindowStaysOnTopHint)) mini = w;
        }
        QVERIFY2(mini, "mini player window not found");
        QVERIFY2(mini->transientParent() == nullptr, "mini player must not be transient for the main window");

        mainWindow->show();
        mini->show();
        QCoreApplication::processEvents();
        QVERIFY(mini->isVisible());
        mainWindow->hide();
        QCoreApplication::processEvents();
        QVERIFY2(mini->isVisible(), "hiding the main window (tray) must keep the mini player open");

        mini->hide();
        mainWindow->hide();
        root.reset();
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }


    // Mini player: the drag area moved the window by adding the pointer delta to window.x/y while
    // the window itself moved under the pointer. That jitters on X11 and does nothing on Wayland,
    // where clients cannot position their own windows. Dragging has to start a compositor-driven
    // move (Window.startSystemMove) and only fall back to manual positioning when unsupported.
    void testMiniPlayerDragUsesSystemMove() {
        QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/GoForIt/qml/components/MiniPlayerWindow.qml"));
        QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(f.fileName()));
        const QString src = QString::fromUtf8(f.readAll());
        QVERIFY2(src.contains(QStringLiteral("startSystemMove()")), "drag must use Window.startSystemMove()");
    }


    // Mini player: it has to offer previous/next task buttons so the active task can be switched
    // without restoring the main window. The test loads the real built Main.qml, finds the two
    // buttons by object name and clicks them.
    void testMiniPlayerSwitchesTasks() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("mini_switch.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        controller.addNewTask(QStringLiteral("mini task A"));
        controller.addNewTask(QStringLiteral("mini task B"));
        controller.addNewTask(QStringLiteral("mini task C"));
        controller.setActiveTodoRow(0);
        QCOMPARE(controller.activeTodoRow(), 0);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        engine.rootContext()->setContextProperty(QStringLiteral("showAboutOnStart"), false);
        const QString srcRoot = QCoreApplication::applicationDirPath() + QStringLiteral("/GoForIt/qml");
        const QString qmlRoot = m_testSandbox.filePath(QStringLiteral("mini_switch_qml"));
        QDir(qmlRoot).removeRecursively();
        QDirIterator it(srcRoot, {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            const QString dst = qmlRoot + f.mid(srcRoot.size());
            QDir().mkpath(QFileInfo(dst).absolutePath());
            QVERIFY(QFile::copy(f, dst));
        }
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlRoot + QStringLiteral("/Main.qml")));
        std::unique_ptr<QObject> root(comp.create());
        QVERIFY2(root, qPrintable(comp.errorString()));

        QObject* next = root->findChild<QObject*>(QStringLiteral("miniNextTask"));
        QObject* prev = root->findChild<QObject*>(QStringLiteral("miniPrevTask"));
        QVERIFY2(next && prev, "mini player previous/next task buttons not found");

        QVERIFY(QMetaObject::invokeMethod(next, "clicked"));
        QCOMPARE(controller.activeTodoRow(), 1);
        QVERIFY(QMetaObject::invokeMethod(next, "clicked"));
        QCOMPARE(controller.activeTodoRow(), 2);
        QVERIFY(QMetaObject::invokeMethod(prev, "clicked"));
        QCOMPARE(controller.activeTodoRow(), 1);

        root.reset();
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }


    // Main window: double-clicking a task row or pressing Enter/Return must open the task details
    // dialog (description & comments) both in the task list view and in the timer view, rather than
    // entering inline title editing or requiring the small row icon button.
    void testMainWindowOpensTaskDetailsOnDoubleClickAndEnter() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("open_details.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        const QString listId = controller.listManager()->addList(QStringLiteral("Details Test List"));
        controller.selectListById(listId);
        while (controller.todoProxyModel()->rowCount() > 0) {
            QVERIFY(controller.removeTask(false, 0));
        }
        controller.addNewTask(QStringLiteral("First task for details"));
        controller.addNewTask(QStringLiteral("Second task for details"));
        QVERIFY(controller.updateTaskDetails(false, 0, QStringLiteral("Detailed markdown body 1")));
        QVERIFY(controller.addTaskComment(false, 0, QStringLiteral("First task comment")));
        QVERIFY(controller.updateTaskDetails(false, 1, QStringLiteral("Detailed markdown body 2")));
        controller.setActiveTodoRow(0);
        controller.setShowingOverview(false);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        engine.rootContext()->setContextProperty(QStringLiteral("showAboutOnStart"), false);
        const QString srcRoot = QCoreApplication::applicationDirPath() + QStringLiteral("/GoForIt/qml");
        const QString qmlRoot = m_testSandbox.filePath(QStringLiteral("open_details_qml"));
        QDir(qmlRoot).removeRecursively();
        QDirIterator it(srcRoot, {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            const QString dst = qmlRoot + f.mid(srcRoot.size());
            QDir().mkpath(QFileInfo(dst).absolutePath());
            QVERIFY(QFile::copy(f, dst));
        }
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlRoot + QStringLiteral("/Main.qml")));
        std::unique_ptr<QObject> root(comp.create());
        QVERIFY2(root, qPrintable(comp.errorString()));
        QWindow* mainWindow = qobject_cast<QWindow*>(root.get());
        QVERIFY(mainWindow);
        mainWindow->show();
        QCoreApplication::processEvents();

        QObject* todoDialog = root->findChild<QObject*>(QStringLiteral("todoTaskDetailsDialog"));
        QObject* timerDialog = root->findChild<QObject*>(QStringLiteral("timerTaskDetailsDialog"));
        QObject* todoList = root->findChild<QObject*>(QStringLiteral("todoTasksView"));
        QVERIFY2(todoDialog && timerDialog && todoList, "task details dialogs or todo list view not found");

        // 1. Pressing Return on the focused task list opens details for the active task (row 0).
        QVERIFY(QMetaObject::invokeMethod(todoList, "forceActiveFocus"));
        QCoreApplication::processEvents();
        QTest::keyClick(mainWindow, Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(todoDialog->property("visible").toBool(), true);
        QCOMPARE(todoDialog->property("taskDetails").toString(), QStringLiteral("Detailed markdown body 1"));
        QCOMPARE(todoDialog->property("taskComments").toList().size(), 1);
        QVERIFY(QMetaObject::invokeMethod(todoDialog, "close"));
        QCoreApplication::processEvents();
        QCOMPARE(todoDialog->property("visible").toBool(), false);

        // 2. Double-clicking a task delegate opens its details (and does NOT enable inline title editing).
        QObject* todoListViewObj = root->findChild<QObject*>(QStringLiteral("todoListView"));
        QVERIFY(todoListViewObj);
        QVariant delegateVar;
        QVERIFY(QMetaObject::invokeMethod(todoListViewObj, "delegateAt",
                                          Q_RETURN_ARG(QVariant, delegateVar),
                                          Q_ARG(QVariant, 1)));
        QObject* secondDelegate = delegateVar.value<QObject*>();
        QVERIFY2(secondDelegate, "second task delegate not found");
        QVERIFY(QMetaObject::invokeMethod(secondDelegate, "doubleClicked"));
        QCoreApplication::processEvents();
        QCOMPARE(secondDelegate->property("editing").toBool(), false);
        QCOMPARE(todoDialog->property("visible").toBool(), true);
        QCOMPARE(todoDialog->property("taskDetails").toString(), QStringLiteral("Detailed markdown body 2"));
        QVERIFY(QMetaObject::invokeMethod(todoDialog, "close"));
        QCoreApplication::processEvents();

        // 3. In Timer view, pressing Return (or double-clicking the active task) opens its details dialog.
        root->setProperty("currentActivityTab", 1);
        QObject* timerViewObj = root->findChild<QObject*>(QStringLiteral("timerViewRoot"));
        QVERIFY2(timerViewObj, "timerViewRoot not found");
        QVERIFY(QMetaObject::invokeMethod(timerViewObj, "forceActiveFocus"));
        QCoreApplication::processEvents();
        QTest::keyClick(mainWindow, Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(timerDialog->property("visible").toBool(), true);
        QCOMPARE(timerDialog->property("taskDetails").toString(), QStringLiteral("Detailed markdown body 2"));
        QVERIFY(QMetaObject::invokeMethod(timerDialog, "close"));

        mainWindow->hide();
        root.reset();
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }


    // Mini player & Ambient Sound:
    // 1. TimerView's ambient sound ComboBox previously bound textRole: "label" while availableModes()
    //    only provided "name", leaving all dropdown items blank.
    // 2. MiniPlayerWindow previously set Qt::Tool, causing window managers to hide it when switching
    //    windows, and lacked a toggle for Always on Top.
    // 3. MiniPlayerWindow lacked a control to switch the ambient sound ("melody").
    // 4. The Play/Pause button on MiniPlayerWindow jumped when toggling running state.
    void testMiniPlayerAlwaysOnTopAmbientSoundAndStablePlayPause() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("mini_ambient.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        controller.addNewTask(QStringLiteral("Focus task for mini player"));
        controller.setActiveTodoRow(0);
        controller.setShowingOverview(false);

        // Verify availableModes() exposes both "name" and "label" with non-empty text.
        const QVariantList modes = controller.ambient()->availableModes();
        QCOMPARE(modes.size(), 5);
        for (const QVariant& m : modes) {
            const QVariantMap map = m.toMap();
            QVERIFY(!map.value(QStringLiteral("id")).toString().isEmpty());
            QVERIFY(!map.value(QStringLiteral("name")).toString().isEmpty());
            QCOMPARE(map.value(QStringLiteral("label")).toString(), map.value(QStringLiteral("name")).toString());
        }

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        engine.rootContext()->setContextProperty(QStringLiteral("showAboutOnStart"), false);
        const QString srcRoot = QCoreApplication::applicationDirPath() + QStringLiteral("/GoForIt/qml");
        const QString qmlRoot = m_testSandbox.filePath(QStringLiteral("mini_ambient_qml"));
        QDir(qmlRoot).removeRecursively();
        QDirIterator it(srcRoot, {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            const QString dst = qmlRoot + f.mid(srcRoot.size());
            QDir().mkpath(QFileInfo(dst).absolutePath());
            QVERIFY(QFile::copy(f, dst));
        }
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlRoot + QStringLiteral("/Main.qml")));
        std::unique_ptr<QObject> root(comp.create());
        QVERIFY2(root, qPrintable(comp.errorString()));
        QWindow* mainWindow = qobject_cast<QWindow*>(root.get());
        QVERIFY(mainWindow);

        QWindow* mini = root->findChild<QWindow*>(QStringLiteral("miniPlayerWindow"));
        QVERIFY2(mini, "mini player window not found");

        // 1. Mini player must NOT have Qt::Tool (so it doesn't hide when switching windows),
        //    and must allow toggling Always on Top while staying visible.
        QVERIFY2(!mini->flags().testFlag(Qt::Tool), "mini player must not use Qt::Tool");
        QVERIFY(mini->flags().testFlag(Qt::WindowStaysOnTopHint));
        mini->show();
        QCoreApplication::processEvents();
        QVERIFY(mini->isVisible());

        QObject* pinBtn = root->findChild<QObject*>(QStringLiteral("miniAlwaysOnTopToggle"));
        QVERIFY2(pinBtn, "miniAlwaysOnTopToggle button not found");
        QVERIFY(QMetaObject::invokeMethod(pinBtn, "clicked"));
        QCoreApplication::processEvents();
        QCOMPARE(mini->property("alwaysOnTop").toBool(), false);
        QVERIFY(!mini->flags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY(mini->isVisible());

        QVERIFY(QMetaObject::invokeMethod(pinBtn, "clicked"));
        QCoreApplication::processEvents();
        QCOMPARE(mini->property("alwaysOnTop").toBool(), true);
        QVERIFY(mini->flags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY(mini->isVisible());

        // 2. TimerView's ambient ComboBox must show non-empty displayText, and clicking
        //    miniAmbientToggle in the mini player must cycle through all ambient modes.
        QObject* ambientCombo = root->findChild<QObject*>(QStringLiteral("timerAmbientCombo"));
        QObject* miniAmbientBtn = root->findChild<QObject*>(QStringLiteral("miniAmbientToggle"));
        QVERIFY2(ambientCombo && miniAmbientBtn, "ambient controls not found");
        QVERIFY2(!ambientCombo->property("displayText").toString().trimmed().isEmpty(),
                 "TimerView ambient ComboBox displayText must not be blank");

        QCOMPARE(controller.ambient()->ambientMode(), QStringLiteral("none"));
        QVERIFY(QMetaObject::invokeMethod(miniAmbientBtn, "clicked"));
        QCoreApplication::processEvents();
        QCOMPARE(controller.ambient()->ambientMode(), QStringLiteral("white_noise"));
        QCOMPARE(ambientCombo->property("currentIndex").toInt(), 1);
        QVERIFY(!ambientCombo->property("displayText").toString().trimmed().isEmpty());

        QVERIFY(QMetaObject::invokeMethod(miniAmbientBtn, "clicked"));
        QCOMPARE(controller.ambient()->ambientMode(), QStringLiteral("rain"));
        QVERIFY(QMetaObject::invokeMethod(miniAmbientBtn, "clicked"));
        QCOMPARE(controller.ambient()->ambientMode(), QStringLiteral("ocean"));
        QVERIFY(QMetaObject::invokeMethod(miniAmbientBtn, "clicked"));
        QCOMPARE(controller.ambient()->ambientMode(), QStringLiteral("clock"));
        QVERIFY(QMetaObject::invokeMethod(miniAmbientBtn, "clicked"));
        QCOMPARE(controller.ambient()->ambientMode(), QStringLiteral("none"));

        // 3. Play/Pause button on the mini player must not jump (x, y, width, height stay constant)
        //    when toggling the timer between stopped and running.
        QObject* playPauseBtn = root->findChild<QObject*>(QStringLiteral("miniPlayPauseButton"));
        QVERIFY2(playPauseBtn, "miniPlayPauseButton not found");
        QCoreApplication::processEvents();
        const double xBefore = playPauseBtn->property("x").toDouble();
        const double yBefore = playPauseBtn->property("y").toDouble();
        const double wBefore = playPauseBtn->property("width").toDouble();
        const double hBefore = playPauseBtn->property("height").toDouble();

        QVERIFY(QMetaObject::invokeMethod(playPauseBtn, "clicked"));
        QCoreApplication::processEvents();
        QCOMPARE(controller.timer()->isRunning(), true);
        QCOMPARE(playPauseBtn->property("x").toDouble(), xBefore);
        QCOMPARE(playPauseBtn->property("y").toDouble(), yBefore);
        QCOMPARE(playPauseBtn->property("width").toDouble(), wBefore);
        QCOMPARE(playPauseBtn->property("height").toDouble(), hBefore);

        QVERIFY(QMetaObject::invokeMethod(playPauseBtn, "clicked"));
        QCoreApplication::processEvents();
        QCOMPARE(controller.timer()->isRunning(), false);
        QCOMPARE(playPauseBtn->property("x").toDouble(), xBefore);
        QCOMPARE(playPauseBtn->property("y").toDouble(), yBefore);
        QCOMPARE(playPauseBtn->property("width").toDouble(), wBefore);
        QCOMPARE(playPauseBtn->property("height").toDouble(), hBefore);

        // 4. Tray menu includes a checkable action to toggle the mini player window.
        QAction* trayMiniAct = controller.desktopIntegration()->toggleMiniPlayerAction();
        QVERIFY2(trayMiniAct, "tray toggleMiniPlayerAction not found");
        QCOMPARE(trayMiniAct->isChecked(), true);
        trayMiniAct->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(mini->isVisible(), false);
        QCOMPARE(trayMiniAct->isChecked(), false);
        trayMiniAct->trigger();
        QCoreApplication::processEvents();
        QCOMPARE(mini->isVisible(), true);
        QCOMPARE(trayMiniAct->isChecked(), true);

        mini->hide();
        mainWindow->hide();
        root.reset();
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }


    // KB-91: d92a8a6 added Keys.onReturnPressed handlers to TaskListPage, TaskRowDelegate and
    // TimerView that open the task details dialog. TextInput emits accepted() on Return and then
    // ignores the key event, so it propagates up the parent chain: pressing Return in the
    // "Add new task" field, the inline title editor, the filter field or an editable timer
    // SpinBox both performs the field's action and opens the details dialog.
    void testReturnInEditorsDoesNotOpenTaskDetails() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb91.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        const QString listId = controller.listManager()->addList(QStringLiteral("KB91 List"));
        controller.selectListById(listId);
        while (controller.todoProxyModel()->rowCount() > 0) {
            QVERIFY(controller.removeTask(false, 0));
        }
        controller.addNewTask(QStringLiteral("KB91 existing task"));
        QVERIFY(controller.updateTaskDetails(false, 0, QStringLiteral("KB91 details")));
        controller.setActiveTodoRow(0);
        controller.setShowingOverview(false);
        QVERIFY(controller.timer()->hasActiveTask());

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        engine.rootContext()->setContextProperty(QStringLiteral("showAboutOnStart"), false);
        const QString qmlRoot = m_testSandbox.filePath(QStringLiteral("kb91_qml"));
        QVERIFY(copyBuiltQmlTree(qmlRoot));
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlRoot + QStringLiteral("/Main.qml")));
        std::unique_ptr<QObject> root(comp.create());
        QVERIFY2(root, qPrintable(comp.errorString()));
        QWindow* mainWindow = qobject_cast<QWindow*>(root.get());
        QVERIFY(mainWindow);
        mainWindow->show();
        QCoreApplication::processEvents();

        QObject* todoDialog = root->findChild<QObject*>(QStringLiteral("todoTaskDetailsDialog"));
        QObject* timerDialog = root->findChild<QObject*>(QStringLiteral("timerTaskDetailsDialog"));
        QObject* todoListView = root->findChild<QObject*>(QStringLiteral("todoListView"));
        QObject* timerView = root->findChild<QObject*>(QStringLiteral("timerViewRoot"));
        QVERIFY(todoDialog && timerDialog && todoListView && timerView);

        const auto findByClass = [](QObject* parent, const char* className) {
            QList<QObject*> found;
            for (QObject* c : parent->findChildren<QObject*>()) {
                if (QByteArray(c->metaObject()->className()).contains(className)) found << c;
            }
            return found;
        };
        QStringList opened;
        const auto checkAndClose = [&](const QString& what) {
            QCoreApplication::processEvents();
            for (QObject* dlg : {todoDialog, timerDialog}) {
                if (dlg->property("visible").toBool()) {
                    opened << what + QStringLiteral(" -> ") + dlg->objectName();
                    QMetaObject::invokeMethod(dlg, "close");
                    QCoreApplication::processEvents();
                }
            }
        };

        // 1. "Add new task" field: Return adds the task and must do nothing else.
        QVERIFY(QMetaObject::invokeMethod(todoListView, "focusInput"));
        QCoreApplication::processEvents();
        for (char ch : QByteArray("KB91 new task")) {
            QTest::keyClick(mainWindow, ch);
        }
        QTest::keyClick(mainWindow, Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(controller.todoProxyModel()->rowCount(), 2);
        checkAndClose(QStringLiteral("new task field"));

        // 2. Inline title editor (the row's edit button): Return saves the title only.
        QVariant delegateVar;
        QVERIFY(QMetaObject::invokeMethod(todoListView, "delegateAt",
                                          Q_RETURN_ARG(QVariant, delegateVar), Q_ARG(QVariant, 0)));
        QObject* firstDelegate = delegateVar.value<QObject*>();
        QVERIFY(firstDelegate);
        firstDelegate->setProperty("editing", true);
        QCoreApplication::processEvents();
        QObject* rowEditor = nullptr;
        for (QObject* c : findByClass(firstDelegate, "TextField")) {
            if (c->property("visible").toBool() && c->property("placeholderText").isValid()) {
                rowEditor = c;
                break;
            }
        }
        QVERIFY2(rowEditor, "inline title editor not found");
        rowEditor->setProperty("text", QStringLiteral("KB91 renamed task"));
        QVERIFY(QMetaObject::invokeMethod(rowEditor, "forceActiveFocus"));
        QCoreApplication::processEvents();
        QTest::keyClick(mainWindow, Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(firstDelegate->property("editing").toBool(), false);
        checkAndClose(QStringLiteral("inline title editor"));

        // 3. Filter field (the query still matches the tasks, so the list is not empty). The UI
        // language may be translated, so the field is found by its text, which is bound to the
        // proxy's filterQuery.
        root->setProperty("showFilterBar", true);
        QVERIFY(controller.todoProxyModel()->setProperty("filterQuery", QStringLiteral("KB91")));
        QCoreApplication::processEvents();
        QObject* filterField = nullptr;
        for (QObject* c : findByClass(todoListView, "TextField")) {
            if (c->property("text").toString() == QStringLiteral("KB91")) {
                filterField = c;
                break;
            }
        }
        QVERIFY2(filterField, "filter field not found");
        QVERIFY(QMetaObject::invokeMethod(filterField, "forceActiveFocus"));
        QCoreApplication::processEvents();
        QTest::keyClick(mainWindow, Qt::Key_Return);
        QCoreApplication::processEvents();
        QVERIFY(controller.todoProxyModel()->rowCount() > 0);
        checkAndClose(QStringLiteral("filter field"));
        QVERIFY(controller.todoProxyModel()->setProperty("filterQuery", QString()));
        root->setProperty("showFilterBar", false);

        // 4. Editable minutes SpinBox in the timer view.
        root->setProperty("currentActivityTab", 1);
        QCoreApplication::processEvents();
        const QList<QObject*> spins = findByClass(timerView, "SpinBox");
        QVERIFY(spins.size() >= 2);
        QVERIFY(spins.at(1)->property("editable").toBool());
        QObject* spinInput = spins.at(1)->property("contentItem").value<QObject*>();
        QVERIFY(spinInput);
        QVERIFY(QMetaObject::invokeMethod(spinInput, "forceActiveFocus"));
        QCoreApplication::processEvents();
        QTest::keyClick(mainWindow, Qt::Key_Return);
        checkAndClose(QStringLiteral("timer minutes spin box"));

        QVERIFY2(opened.isEmpty(), qPrintable(opened.join(QStringLiteral(" | "))));

        mainWindow->hide();
        root.reset();
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }


    // KB-92: the To-Do list highlights the active task (app.activeTodoRow), but Return in the
    // list opens the details of tasksView.currentIndex, which only follows mouse clicks. After
    // switching the active task with Alt+Down/Up, the tray or the mini player, Return opens the
    // details of a task that is not highlighted.
    void testReturnInTodoListOpensHighlightedActiveTask() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb92.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        const QString listId = controller.listManager()->addList(QStringLiteral("KB92 List"));
        controller.selectListById(listId);
        while (controller.todoProxyModel()->rowCount() > 0) {
            QVERIFY(controller.removeTask(false, 0));
        }
        controller.addNewTask(QStringLiteral("KB92 first task"));
        controller.addNewTask(QStringLiteral("KB92 second task"));
        QVERIFY(controller.updateTaskDetails(false, 0, QStringLiteral("KB92 details 1")));
        QVERIFY(controller.updateTaskDetails(false, 1, QStringLiteral("KB92 details 2")));
        controller.setActiveTodoRow(0);
        controller.setShowingOverview(false);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        engine.rootContext()->setContextProperty(QStringLiteral("showAboutOnStart"), false);
        const QString qmlRoot = m_testSandbox.filePath(QStringLiteral("kb92_qml"));
        QVERIFY(copyBuiltQmlTree(qmlRoot));
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlRoot + QStringLiteral("/Main.qml")));
        std::unique_ptr<QObject> root(comp.create());
        QVERIFY2(root, qPrintable(comp.errorString()));
        QWindow* mainWindow = qobject_cast<QWindow*>(root.get());
        QVERIFY(mainWindow);
        mainWindow->show();
        QCoreApplication::processEvents();

        QObject* todoDialog = root->findChild<QObject*>(QStringLiteral("todoTaskDetailsDialog"));
        QObject* todoList = root->findChild<QObject*>(QStringLiteral("todoTasksView"));
        QVERIFY(todoDialog && todoList);

        // Same path as Alt+Down, the tray "next task" action and the mini player's next button.
        controller.switchToNextTask();
        QCOMPARE(controller.activeTodoRow(), 1);
        QCoreApplication::processEvents();

        QVERIFY(QMetaObject::invokeMethod(todoList, "forceActiveFocus"));
        QCoreApplication::processEvents();
        QTest::keyClick(mainWindow, Qt::Key_Return);
        QCoreApplication::processEvents();
        QCOMPARE(todoDialog->property("visible").toBool(), true);
        QCOMPARE(todoDialog->property("taskDetails").toString(), QStringLiteral("KB92 details 2"));
        QVERIFY(QMetaObject::invokeMethod(todoDialog, "close"));

        mainWindow->hide();
        root.reset();
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }


    // KB-93: Main.qml declares `Shortcut { sequence: "Ctrl+M" }` twice in the same window
    // (64b8b82 and a0ff6ae). Qt treats the sequence as ambiguous and emits
    // activatedAmbiguously() instead of activated(), so the documented Ctrl+M shortcut never
    // shows or hides the mini player.
    void testCtrlMTogglesMiniPlayer() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb93.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);
        controller.setShowingOverview(false);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        engine.rootContext()->setContextProperty(QStringLiteral("showAboutOnStart"), false);
        const QString qmlRoot = m_testSandbox.filePath(QStringLiteral("kb93_qml"));
        QVERIFY(copyBuiltQmlTree(qmlRoot));
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlRoot + QStringLiteral("/Main.qml")));
        std::unique_ptr<QObject> root(comp.create());
        QVERIFY2(root, qPrintable(comp.errorString()));
        QWindow* mainWindow = qobject_cast<QWindow*>(root.get());
        QVERIFY(mainWindow);
        mainWindow->show();
        mainWindow->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(mainWindow));

        QWindow* mini = qobject_cast<QWindow*>(root->findChild<QObject*>(QStringLiteral("miniPlayerWindow")));
        QVERIFY(mini);
        QCOMPARE(mini->isVisible(), false);

        QTest::keyClick(mainWindow, Qt::Key_M, Qt::ControlModifier);
        QCoreApplication::processEvents();
        QCOMPARE(mini->isVisible(), true);

        mainWindow->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(mainWindow));
        QTest::keyClick(mainWindow, Qt::Key_M, Qt::ControlModifier);
        QCoreApplication::processEvents();
        QCOMPARE(mini->isVisible(), false);

        mini->hide();
        mainWindow->hide();
        root.reset();
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }


    // KB-96: Under Wayland (KWin / KDE Plasma), regular xdg_toplevel windows cannot set
    // WindowStaysOnTopHint via standard xdg-shell protocols, and declaring MiniPlayerWindow
    // inside ApplicationWindow sets its QWindow/QObject parent to the main window. As a result,
    // the mini player is treated as a transient child: when switching to other application
    // windows the mini player drops behind them or disappears.
    // configureMiniPlayerWindow detaches the window as a true independent top-level window
    // (setParent(nullptr), setTransientParent(nullptr)), sets Qt::WindowStaysOnTopHint, and
    // invokes KWin scripting (keepAbove, onAllDesktops) when running under KWin/Wayland.
    void testMiniPlayerDetachAndAlwaysOnTopConfiguration() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb96.db"));
        QFile::remove(dbPath);
        AppController controller(QString(), dbPath);

        QWindow dummyParent;
        QWindow miniWindow(&dummyParent);
        miniWindow.setTransientParent(&dummyParent);
        QCOMPARE(miniWindow.parent(), &dummyParent);
        QCOMPARE(miniWindow.transientParent(), &dummyParent);

        controller.configureMiniPlayerWindow(&miniWindow, true);
        QVERIFY(miniWindow.parent() == nullptr);
        QVERIFY(miniWindow.transientParent() == nullptr);
        QVERIFY(miniWindow.flags().testFlag(Qt::WindowStaysOnTopHint));

        controller.configureMiniPlayerWindow(&miniWindow, false);
        QVERIFY(miniWindow.parent() == nullptr);
        QVERIFY(miniWindow.transientParent() == nullptr);
        QVERIFY(!miniWindow.flags().testFlag(Qt::WindowStaysOnTopHint));

        // Loading real Main.qml should configure miniPlayerWindow automatically on creation
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);
        engine.rootContext()->setContextProperty(QStringLiteral("showAboutOnStart"), false);
        const QString qmlRoot = m_testSandbox.filePath(QStringLiteral("kb96_qml"));
        QVERIFY(copyBuiltQmlTree(qmlRoot));
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlRoot + QStringLiteral("/Main.qml")));
        std::unique_ptr<QObject> root(comp.create());
        QVERIFY2(root, qPrintable(comp.errorString()));
        QWindow* mini = qobject_cast<QWindow*>(root->findChild<QObject*>(QStringLiteral("miniPlayerWindow")));
        QVERIFY(mini);
        QVERIFY(mini->parent() == nullptr);
        QVERIFY(mini->transientParent() == nullptr);
        QVERIFY(mini->flags().testFlag(Qt::WindowStaysOnTopHint));

        // KB-96 strengthening: simulate compositor transient hierarchy and verify Main.qml
        // and MiniPlayerWindow.qml reconfigure the window on toggle and visibility changes
        QWindow* rootWin = qobject_cast<QWindow*>(root.get());
        QVERIFY(rootWin);
        mini->setTransientParent(rootWin);
        QCOMPARE(mini->transientParent(), rootWin);

        QVERIFY(QMetaObject::invokeMethod(root.get(), "toggleMiniPlayer"));
        QVERIFY(mini->transientParent() == nullptr);

        QVERIFY(QMetaObject::invokeMethod(mini, "toggleAlwaysOnTop"));
        QVERIFY(!mini->flags().testFlag(Qt::WindowStaysOnTopHint));

        QVERIFY(QMetaObject::invokeMethod(mini, "toggleAlwaysOnTop"));
        QVERIFY(mini->flags().testFlag(Qt::WindowStaysOnTopHint));

        root.reset();
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }


    // KB-97: the KWin pin script runs over every window of the KDE session. It must change only the
    // mini player window of this process, never other applications' windows (substring captions such
    // as "VLC Mini-Player") or this app's own main window when it is resized below 150 px.
    void testKWinPinScriptTouchesOnlyOwnMiniPlayerWindow() {
        const QString miniTitle = QStringLiteral("Go for It — Mini Player");
        const qint64 ownPid = QCoreApplication::applicationPid();
        const qint64 foreignPid = ownPid + 1;

        struct FakeWindow {
            QString caption;
            QString resourceClass;
            qint64 pid;
            int height;
            bool keepAbove;
            bool onAllDesktops;
            bool skipTaskbar;
            bool skipPager;
        };
        const QList<FakeWindow> initial = {
            {miniTitle, QStringLiteral("go-for-it-qt"), ownPid, 90, false, false, false, false},
            {QStringLiteral("Go for It — Mini Player — Go for It Qt"), QStringLiteral("go-for-it-qt"), ownPid, 90, false, false, false, false},
            {miniTitle, QStringLiteral("go-for-it-qt"), 0, 90, false, false, false, false},
            {QStringLiteral("VLC Mini-Player"), QStringLiteral("vlc"), foreignPid, 300, false, false, false, false},
            // a foreign window the user pinned on their own: our unpin must not clear it
            {QStringLiteral("Spotify Mini Player"), QStringLiteral("spotify"), foreignPid, 200, true, true, false, false},
            {QStringLiteral("Mini Player review — Mozilla Firefox"), QStringLiteral("firefox"), foreignPid, 900, false, false, false, false},
            {QStringLiteral("Міні-плеєр — Chromium"), QStringLiteral("chromium"), foreignPid, 900, false, false, false, false},
            {QStringLiteral("Inbox — Go for It Qt"), QStringLiteral("go-for-it-qt"), ownPid, 120, false, false, false, false},
        };

        QJSEngine engine;
        QJSValue wins = engine.newArray(initial.size());
        for (int i = 0; i < initial.size(); ++i) {
            const FakeWindow& f = initial.at(i);
            QJSValue w = engine.newObject();
            w.setProperty(QStringLiteral("caption"), f.caption);
            w.setProperty(QStringLiteral("resourceClass"), f.resourceClass);
            w.setProperty(QStringLiteral("pid"), static_cast<double>(f.pid));
            w.setProperty(QStringLiteral("height"), f.height);
            w.setProperty(QStringLiteral("keepAbove"), f.keepAbove);
            w.setProperty(QStringLiteral("onAllDesktops"), f.onAllDesktops);
            w.setProperty(QStringLiteral("skipTaskbar"), f.skipTaskbar);
            w.setProperty(QStringLiteral("skipPager"), f.skipPager);
            wins.setProperty(static_cast<quint32>(i), w);
        }
        engine.globalObject().setProperty(QStringLiteral("__gofiWindows"), wins);
        QJSValue setup = engine.evaluate(QStringLiteral(
            "var workspace = { windowList: function() { return __gofiWindows; } };"));
        QVERIFY2(!setup.isError(), qPrintable(setup.toString()));

        auto runScript = [&](bool alwaysOnTop) -> bool {
            const QJSValue result = engine.evaluate(DesktopIntegration::kwinPinScript(miniTitle, alwaysOnTop));
            if (result.isError()) {
                qWarning() << "KWin pin script failed:" << result.toString();
                return false;
            }
            return true;
        };
        auto prop = [&](int i, const char* name) {
            return wins.property(static_cast<quint32>(i)).property(QString::fromLatin1(name)).toBool();
        };
        auto verifyUntouched = [&](int i) -> bool {
            const FakeWindow& f = initial.at(i);
            const bool same = prop(i, "keepAbove") == f.keepAbove && prop(i, "onAllDesktops") == f.onAllDesktops
                && prop(i, "skipTaskbar") == f.skipTaskbar && prop(i, "skipPager") == f.skipPager;
            if (!same) {
                qWarning() << "window changed by the pin script:" << f.caption;
            }
            return same;
        };

        QVERIFY(runScript(true));
        for (int i = 0; i < 3; ++i) {
            QVERIFY2(prop(i, "keepAbove"), qPrintable(initial.at(i).caption));
            QVERIFY2(prop(i, "onAllDesktops"), qPrintable(initial.at(i).caption));
        }
        for (int i = 3; i < initial.size(); ++i) {
            QVERIFY2(verifyUntouched(i), qPrintable(initial.at(i).caption));
        }

        QVERIFY(runScript(false));
        for (int i = 0; i < 3; ++i) {
            QVERIFY2(!prop(i, "keepAbove"), qPrintable(initial.at(i).caption));
            QVERIFY2(!prop(i, "onAllDesktops"), qPrintable(initial.at(i).caption));
        }
        for (int i = 3; i < initial.size(); ++i) {
            QVERIFY2(verifyUntouched(i), qPrintable(initial.at(i).caption));
        }
    }


    // KB-102: since KB-100 (177089d) executeKWinPinScript keeps the temporary script file only in
    // the loadScript reply handler, so the file is deleted right after `run` is sent. KWin's
    // Script::run() reads the file asynchronously and answers only after evaluating it, so it finds
    // no file and never runs the pin script. The plugin name (`gofi_pin_<pid>_<msecs>`) is also
    // reused by calls in the same millisecond (one mini player show calls it twice): KWin answers
    // -1, the code runs /Scripting/Script-1 and then unloads the *first* script by that name.
    void testKWinScriptFileStaysReadableUntilRunAndNamesAreUnique() {
        PrivateDBus bus;
        if (bus.address.isEmpty()) {
            QSKIP("dbus-daemon is not available");
        }
        const QString fakeName = QStringLiteral("kb102_fake_kwin");
        const QString clientName = QStringLiteral("kb102_client");
        {
            QDBusConnection fakeConn = QDBusConnection::connectToBus(bus.address, fakeName);
            QDBusConnection clientConn = QDBusConnection::connectToBus(bus.address, clientName);
            QVERIFY(fakeConn.isConnected());
            QVERIFY(clientConn.isConnected());
            const QString service = QStringLiteral("org.kde.KWin");
            FakeKWinScripting kwin(fakeConn);
            QVERIFY(fakeConn.registerService(service));
            QVERIFY(fakeConn.registerObject(QStringLiteral("/Scripting"), &kwin, QDBusConnection::ExportScriptableSlots));

            const QString title = QStringLiteral("Go for It — Mini Player");
            const QString pinScript = DesktopIntegration::kwinPinScript(title, true);
            const QString unpinScript = DesktopIntegration::kwinPinScript(title, false);

            // 1. One script: KWin must be able to read and evaluate it.
            DesktopIntegration::runKWinScript(pinScript, clientConn, service);
            QTRY_VERIFY_WITH_TIMEOUT(kwin.unloads >= 1, 3000);
            QCOMPARE(kwin.unreadable, 0);
            QCOMPARE(kwin.executed, QStringList{pinScript});

            // 2. Two scripts started in the same event-loop pass (as the mini player does on show):
            //    both must be evaluated.
            kwin.executed.clear();
            DesktopIntegration::runKWinScript(pinScript, clientConn, service);
            DesktopIntegration::runKWinScript(unpinScript, clientConn, service);
            QTRY_VERIFY_WITH_TIMEOUT(kwin.unloads >= 3, 3000);
            QTest::qWait(100);
            QCOMPARE(kwin.unreadable, 0);
            QCOMPARE(kwin.executed.size(), 2);
            QVERIFY(kwin.executed.contains(pinScript));
            QVERIFY(kwin.executed.contains(unpinScript));

            fakeConn.unregisterObject(QStringLiteral("/Scripting"));
            fakeConn.unregisterService(service);
        }
        QDBusConnection::disconnectFromBus(fakeName);
        QDBusConnection::disconnectFromBus(clientName);
    }


    // KB-117: TaskDetailsDialog.qml and TuiApp reported comment as posted and displayed it even
    // when database save failed (AppController::addTaskCommentById / addTaskComment returned false).
    void testPostCommentDoesNotDisplayCommentWhenDbSaveFails() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb117_comment.db"));
        QFile::remove(dbPath);
        AppController ctrl(QString(), dbPath);
        const QString list = ctrl.listManager()->addList(QStringLiteral("KB117"));
        ctrl.selectListById(list);

        // The TUI path is covered by testTuiCommentReportsDbFailureForExistingTask (KB-120): a new list gets
        // default tasks, so a TUI ":comment" here would legitimately succeed.

        // QML verification: TaskDetailsDialog must not append comment or clear text area on failure
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("app"), &ctrl);
        const QString qmlRoot = m_testSandbox.filePath(QStringLiteral("kb117_qml"));
        QVERIFY(copyBuiltQmlTree(qmlRoot));
        QQmlComponent comp(&engine, QUrl::fromLocalFile(qmlRoot + QStringLiteral("/dialogs/TaskDetailsDialog.qml")));
        std::unique_ptr<QObject> dialog(comp.create());
        QVERIFY2(dialog, qPrintable(comp.errorString()));

        // Target non-existent task ID so DB save fails
        dialog->setProperty("taskId", 999999);
        dialog->setProperty("taskRow", -1);
        QObject* area = nullptr;
        QObject* postBtn = nullptr;
        for (QObject* c : dialog->findChildren<QObject*>()) {
            if (c->property("placeholderText").isValid() && c->property("wrapMode").isValid()) {
                area = c;
            }
            if (c->inherits("QQuickAbstractButton")) {
                const QString txt = c->property("text").toString();
                if (txt.contains(QStringLiteral("💬")) || txt.contains(QStringLiteral("Post Comment")) || txt.contains(QStringLiteral("Додати коментар"))) {
                    postBtn = c;
                }
            }
        }
        QVERIFY(area);
        QVERIFY(postBtn);
        area->setProperty("text", QStringLiteral("Failing Comment"));

        QVERIFY(QMetaObject::invokeMethod(postBtn, "clicked"));

        const QVariantList comments = dialog->property("taskComments").toList();
        QCOMPARE(comments.size(), 0);
        QCOMPARE(area->property("text").toString(), QStringLiteral("Failing Comment"));

        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

};

QTEST_MAIN(TestQml)
#include "tst_qml.moc"
