#pragma once

#include <QByteArray>
#include <QString>

namespace gofi {

class AppController;
class TaskFilterProxyModel;

enum class TaskListKeyResult {
    Handled,
    StartInsert,
    StartEditTitle,
    StartEditDetails,
    StartAddComment,
    OpenStatusPicker,
    OpenListPicker,
    OpenSettings,
    OpenTaskMarkdown,
    StartSearch,
    StartCommand,
    OpenHelp,
    OpenDashboard,
    Quit
};

class TuiTaskListView {
public:
    explicit TuiTaskListView(AppController* controller = nullptr);

    void setController(AppController* controller);

    int selectedRow() const { return m_selectedRow; }
    void setSelectedRow(int row) { m_selectedRow = row; }

    int viewTab() const { return m_viewTab; }
    void setViewTab(int tab);

    int scrollOffset() const { return m_scrollOffset; }
    int detailsScroll() const { return m_detailsScroll; }

    bool showPreview() const { return m_showPreview; }
    void setShowPreview(bool show) { m_showPreview = show; }

    bool isClosedTab() const;
    TaskFilterProxyModel* currentProxy() const;

    void switchViewTab(int tabIndex);
    void clampSelection();
    void resetSelection();

    QString openSelectedTaskInBrowser();

    TaskListKeyResult handleKey(const QByteArray& seq, QString& statusMessage, QString& initialText);

    QString renderTabRow(int cols) const;
    QString renderHelp(int bodyRows, int cols) const;
    QString renderTaskList(int bodyRows, int cols);

private:
    AppController* m_app = nullptr;
    int m_viewTab = 0; // 0:All Open, 1:To-Do, 2:In Progress, 3:Paused, 4:Done, 5:Canceled
    int m_selectedRow = 0;
    int m_scrollOffset = 0;
    int m_detailsScroll = 0;
    bool m_showPreview = true;
    char m_pendingKey = 0; // for 'g' (gg), 'd' (dd), 'c' (cw)
};

} // namespace gofi
