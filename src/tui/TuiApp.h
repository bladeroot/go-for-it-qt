#pragma once

#include "core/AppController.h"
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
    void switchViewTab(int tabIndex);
    void clampSelection();
    void processPendingStdin();
    TaskFilterProxyModel* currentProxy() const;
    bool isClosedTab() const;

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
    int m_viewTab = 0; // 0:All Open, 1:To-Do, 2:In Progress, 3:Paused, 4:Done, 5:Canceled
    int m_selectedRow = 0;
    int m_scrollOffset = 0;
    int m_detailsScroll = 0;
    int m_listPickerRow = 0;
    QSet<QString> m_selectedListIds;
    int m_settingsRow = 0;
    int m_listEditRow = 0;
    QVariantMap m_editingListCfg;
    bool m_showPreview = true;
    char m_pendingKey = 0; // for 'g' (gg), 'd' (dd), 'c' (cw)

    QString m_inputBuffer;
    int m_inputCursor = 0;
    QString m_statusMessage;
    QString m_markdownTitle;
    QString m_markdownContent;
    int m_markdownScroll = 0;
    int m_dashboardPeriod = 0; // 0:Day, 1:Week, 2:Month, 3:All
    int m_dashboardScroll = 0;
};

} // namespace gofi
