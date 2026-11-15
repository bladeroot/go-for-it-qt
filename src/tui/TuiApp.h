#pragma once

#include "core/AppController.h"
#include "tui/TuiMarkdownView.h"
#include "tui/TuiDashboardView.h"
#include "tui/TuiCommandHandler.h"
#include "tui/TuiSettingsView.h"
#include "tui/TuiListManagerView.h"
#include "tui/TuiInputLine.h"
#include "tui/TuiTaskListView.h"
#include <QObject>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>
#include <termios.h>

namespace gofi {

class TuiApp : public QObject {
    Q_OBJECT
public:
    enum class Mode {
        Normal,
        InsertTask,
        EditTask,
        EditDetails,
        Search,
        Command,
        StatusPicker,
        ListPicker,
        ListEdit,
        Settings,
        EditSettingValue,
        EditListField,
        MarkdownView,
        Help,
        Dashboard
    };

    explicit TuiApp(AppController* controller, QObject* parent = nullptr);
    ~TuiApp() override;

    bool start();
    void openMarkdownFile(const QString& filePath);
    static QString sanitizeTerminalText(const QString& input, bool allowNewlines = false);

private slots:
    void onStdinActivated();
    void render();

private:
    void enableRawMode();
    void disableRawMode();
    void quitApp();
    void updateTerminalSize();

    void handleKeySequence(const QByteArray& seq);
    void handleNormalKey(const QByteArray& seq);
    void handleInputLineKey(const QByteArray& seq);
    void handleStatusPickerKey(const QByteArray& seq);
    void handleListPickerKey(const QByteArray& seq);
    void handleListEditKey(const QByteArray& seq);
    void handleSettingsKey(const QByteArray& seq);
    void handleMarkdownViewKey(const QByteArray& seq);
    void handleHelpKey(const QByteArray& seq);
    void handleDashboardKey(const QByteArray& seq);
    void openListPicker();
    void openTaskMarkdownView();
    void openDashboardView();
    void editTaskDetailsInVim();
    void addTaskCommentInVim();
    void openSelectedTaskInBrowser();
    static QStringList renderMarkdownToAnsi(const QString& markdown, int maxWidth);
    static QString formatInlineMarkdown(const QString& text);

    void executeExCommand(const QString& cmdLine);
    void switchViewTab(int tabIndex) { m_taskListView.switchViewTab(tabIndex); }
    void clampSelection() { m_taskListView.clampSelection(); }
    void processPendingStdin();
    TaskFilterProxyModel* currentProxy() const { return m_taskListView.currentProxy(); }
    bool isClosedTab() const { return m_taskListView.isClosedTab(); }
    int selectedRow() const { return m_taskListView.selectedRow(); }
    void setSelectedRow(int row) { m_taskListView.setSelectedRow(row); }
    int viewTab() const { return m_taskListView.viewTab(); }

    static QString truncateVisible(const QString& str, int maxCols);
    static QString padRight(const QString& str, int width);
    static QString formatSeconds(uint secs);

    AppController* m_app = nullptr;
    QSocketNotifier* m_stdinNotifier = nullptr;
    QTimer m_resizeTimer;
    QTimer m_escTimer;
    struct termios m_origTermios {};
    bool m_rawModeActive = false;
    QByteArray m_stdinPending;

    int m_rows = 24;
    int m_cols = 80;

    Mode m_mode = Mode::Normal;

    TuiTaskListView m_taskListView;
    TuiInputLine m_inputLine;
    QString m_statusMessage;
    TuiMarkdownView m_markdownView;
    TuiDashboardView m_dashboardView;
    TuiCommandHandler m_commandHandler;
    TuiSettingsView m_settingsView;
    TuiListManagerView m_listManagerView;

    friend class TuiCommandHandler;
};

} // namespace gofi
