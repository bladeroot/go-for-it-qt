#include "TestSupport.h"

#include "core/ActivityLog.h"
#include "core/AnalyticsService.h"
#include "core/AppController.h"
#include "core/DatabaseManager.h"
#include "core/IssueTrackerService.h"
#include "core/ListManagerModel.h"
#include "core/SettingsManager.h"
#include "tui/TuiApp.h"
#include "todotxt/TaskFilterProxyModel.h"
#include "todotxt/TaskStoreModel.h"
#include "todotxt/TxtTask.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QStringList>
#include <QTimeZone>
#include <QtTest>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace gofi;

class TestTui : public QObject {
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

    // KB-88: the KB-85 fix (f1d8af6) parses the TUI duration fields with toLongLong(&ok) and,
    // when parsing fails, falls back to the *maximum* (1440 min for work/break, 3600 s for the
    // reminder, 1440 min for auto-sync) unless the text starts with '-'. An empty field (the user
    // clears the pre-filled value with Ctrl+U and presses Enter) or a typo such as "25m" or "2 5"
    // therefore silently sets a 24-hour Pomodoro instead of being rejected.
    void testTuiEmptyDurationInputDoesNotSetMaximum() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb88_tui.db"));
        QFile::remove(dbPath);
        AppController ctrl(QString(), dbPath);
        SettingsManager* settings = ctrl.settings();
        const int oldTask = settings->taskDuration();
        settings->setTaskDuration(1500);

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
            feed("O");       // settings, row 0 = work duration
            feed("\r");      // edit (pre-filled with "25")
            feed("\x15");    // Ctrl+U clears the field
            feed("\r");      // confirm the empty value
        }
        std::fflush(stdout);
        ::dup2(savedStdin, STDIN_FILENO);
        ::dup2(savedStdout, STDOUT_FILENO);
        ::close(savedStdin);
        ::close(savedStdout);
        ::close(devNull);
        ::close(fds[0]);
        ::close(fds[1]);

        const int after = settings->taskDuration();
        settings->setTaskDuration(oldTask);
        QCOMPARE(after, 1500);
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // percentage belongs, the percentage instead of the time, and drops the time.
    void testTuiDashboardProjectAndContextLinesAreFormatted() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb103_tui.db"));
        QFile::remove(dbPath);
        AppController ctrl(QString(), dbPath);
        const QString list = ctrl.listManager()->addList(QStringLiteral("KB103"));
        ctrl.selectListById(list);
        const QDateTime noon = QDateTime(QDate::currentDate(), QTime(12, 0, 0), QTimeZone::systemTimeZone()).toUTC();
        QVERIFY(DatabaseManager::instance().insertActivityLog(list, QStringLiteral("KB103"), -1, QString(),
                QStringLiteral("Write API +backend @office"), noon.addSecs(-3700), 3600, noon.addSecs(-100), false, false) > 0);

        QStringList projectLines;
        QStringList contextLines;
        QString screen;
        {
            PtyTerminal term;
            QVERIFY(term.open(60, 140));
            TuiApp tui(&ctrl);
            QVERIFY(tui.start());
            QTest::qWait(100);
            term.takeOutput();
            term.type("U");
            QTest::qWait(300);
            screen = QString::fromUtf8(term.takeOutput());
        }
        // Several frames may have been rendered; every frame starts with ESC[H. Keep the last one.
        const qsizetype lastFrame = screen.lastIndexOf(QStringLiteral("\x1b[H"));
        if (lastFrame >= 0) screen = screen.mid(lastFrame);
        screen.remove(QRegularExpression(QStringLiteral("\x1b\\[[0-9;?]*[A-Za-z]")));
        for (const QString& line : screen.split(QRegularExpression(QStringLiteral("[\r\n]+")))) {
            if (line.contains(QStringLiteral("Write API"))) continue; // the TOP TASKS line
            if (line.contains(QStringLiteral("backend"))) projectLines << line;
            if (line.contains(QStringLiteral("office"))) contextLines << line;
        }
        QVERIFY2(screen.contains(QStringLiteral("Write API +backend @office")), qPrintable(screen.left(2000)));
        QCOMPARE(projectLines.size(), 1);
        QCOMPARE(contextLines.size(), 1);
        for (const QString& line : {projectLines.first(), contextLines.first()}) {
            QVERIFY2(!line.contains(QLatin1Char('%') + QStringLiteral("-16s")), qPrintable(line));
            QVERIFY2(line.contains(QStringLiteral("+backend")) || line.contains(QStringLiteral("@office")), qPrintable(line));
            QVERIFY2(line.contains(QStringLiteral("100.0%")), qPrintable(line));
            QVERIFY2(line.contains(QStringLiteral("(1h 00m)")), qPrintable(line));
            QVERIFY2(line.contains(QStringLiteral("[") + QString(20, QChar(0x2588)) + QStringLiteral("]")), qPrintable(line));
        }
        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-120: the TUI half of testPostCommentDoesNotDisplayCommentWhenDbSaveFails ran on an empty list and
    // looked for a string the TUI never printed. This exercises a real DB write failure on an existing task.
    void testTuiCommentReportsDbFailureForExistingTask() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb120_tui_dbfail.db"));
        QFile::remove(dbPath);
        AppController ctrl(QString(), dbPath);
        const QString list = ctrl.listManager()->addList(QStringLiteral("KB120"));
        ctrl.selectListById(list);
        ctrl.addNewTask(QStringLiteral("Commented task"));
        QVERIFY(ctrl.todoProxyModel()->rowCount() >= 1);

        QSqlQuery trig(QSqlDatabase::database(QStringLiteral("gofi_sqlite_conn")));
        QVERIFY(trig.exec(QStringLiteral(
            "CREATE TRIGGER kb120_fail BEFORE UPDATE ON tasks "
            "BEGIN SELECT RAISE(ABORT, 'Simulated comment save failure'); END;")));

        QString screen;
        {
            PtyTerminal term;
            QVERIFY(term.open(30, 100));
            TuiApp tui(&ctrl);
            QVERIFY(tui.start());
            QTest::qWait(100);
            term.takeOutput();
            term.type(":comment Unsaved Comment\r");
            QTest::qWait(300);
            screen = QString::fromUtf8(term.takeOutput());
        }
        screen.remove(QRegularExpression(QStringLiteral("\x1b\\[[0-9;?]*[A-Za-z]")));
        const QString okMsg = QCoreApplication::translate("TuiApp", "Comment added & queued for Jira/YouTrack sync.");
        const QString failMsg = QCoreApplication::translate("TuiApp", "Failed to save comment to database.");
        QVERIFY2(!screen.contains(okMsg.left(20)), qPrintable(screen));
        QVERIFY2(screen.contains(failMsg.left(20)), qPrintable(screen));

        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }

    // KB-115: TUI dashboard "── 28-DAY ACTIVITY HEATMAP ──" hardcoded 28 days even though
    // getActivityHeatmap returns 28..34 days (Monday-aligned since KB-99), and on an 80-column
    // terminal (2 + 3*days cols = 86..104 cols) truncateVisible cut off the rightmost cells,
    // causing today's activity to be completely invisible.
    void testTuiDashboardHeatmapFitsStandardTerminalAndCountsDaysAccurately() {
        const QString dbPath = m_testSandbox.filePath(QStringLiteral("kb115_tui.db"));
        QFile::remove(dbPath);
        AppController ctrl(QString(), dbPath);
        const QString list = ctrl.listManager()->addList(QStringLiteral("KB115"));
        ctrl.selectListById(list);

        // Record 3 hours today so today's heatmap cell has highest intensity ([█])
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QVERIFY(DatabaseManager::instance().insertActivityLog(list, QStringLiteral("KB115"), -1, QString(),
                QStringLiteral("Intensive Work Session"), now.addSecs(-10800), 10800, now, true, false) > 0);

        const QVariantList heatmap = ctrl.analytics()->getActivityHeatmap(28, ctrl.activeListFilter());
        const int actualDays = heatmap.size();
        QVERIFY(actualDays >= 28 && actualDays <= 34);

        QString screen;
        {
            PtyTerminal term;
            QVERIFY(term.open(30, 80)); // Standard 80-column terminal
            TuiApp tui(&ctrl);
            QVERIFY(tui.start());
            QTest::qWait(100);
            term.takeOutput();
            term.type("U"); // Open Dashboard
            QTest::qWait(300);
            screen = QString::fromUtf8(term.takeOutput());
        }

        const qsizetype lastFrame = screen.lastIndexOf(QStringLiteral("\x1b[H"));
        if (lastFrame >= 0) screen = screen.mid(lastFrame);
        screen.remove(QRegularExpression(QStringLiteral("\x1b\\[[0-9;?]*[A-Za-z]")));

        // Title must show the accurate day count, not hardcoded 28-DAY
        const QString expectedTitle = QCoreApplication::translate("TuiApp", "  ── %1-DAY ACTIVITY HEATMAP ──").arg(actualDays).trimmed();
        QVERIFY2(screen.contains(expectedTitle), qPrintable(screen.left(2000)));

        // Today's cell ([█]) must be visible on the 80-column terminal, not truncated away
        QVERIFY2(screen.contains(QStringLiteral("[█]")), qPrintable(screen));

        // Count all cells in heatmap rows: must equal actualDays
        int renderedCells = 0;
        for (const QString& line : screen.split(QRegularExpression(QStringLiteral("[\r\n]+")))) {
            if (line.contains(QLatin1Char('[')) && line.contains(QLatin1Char(']')) &&
                !line.contains(QStringLiteral("PROJECTS")) && !line.contains(QStringLiteral("CONTEXTS")) &&
                !line.contains(QStringLiteral("TOP TASKS")) && !line.contains(QStringLiteral("Focus"))) {
                renderedCells += line.count(QStringLiteral("[ ]"))
                               + line.count(QStringLiteral("[░]"))
                               + line.count(QStringLiteral("[▒]"))
                               + line.count(QStringLiteral("[▓]"))
                               + line.count(QStringLiteral("[█]"));
            }
        }
        QCOMPARE(renderedCells, actualDays);

        DatabaseManager::instance().close();
        QFile::remove(dbPath);
    }
};

QTEST_MAIN(TestTui)
#include "tst_tui.moc"
