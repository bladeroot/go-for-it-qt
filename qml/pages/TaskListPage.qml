import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

Item {
    id: taskListPageRoot
    focus: true

    property int currentTab: 0
    property bool filterVisible: false
    signal currentTabRequested(int tabIndex)

    function focusNewTaskInput() {
        todoListView.focusInput()
    }

    function openCurrentTaskDetails() {
        if (taskListPageRoot.currentTab === 0) {
            return todoListView.openSelectedTaskDetails()
        } else if (taskListPageRoot.currentTab === 1) {
            return timerView.openActiveTaskDetails()
        } else if (taskListPageRoot.currentTab === 2) {
            return doneListView.openSelectedTaskDetails()
        }
        return false
    }

    function isEditorFocused() {
        let item = Window.window ? Window.window.activeFocusItem : null
        while (item) {
            if (item.hasOwnProperty("text") && item.hasOwnProperty("cursorPosition")) return true
            if (item.hasOwnProperty("editable") && item.editable) return true
            item = item.parent
        }
        return false
    }

    Keys.onReturnPressed: function(event) {
        if (!taskListPageRoot.isEditorFocused() && taskListPageRoot.openCurrentTaskDetails()) {
            event.accepted = true
        }
    }
    Keys.onEnterPressed: function(event) {
        if (!taskListPageRoot.isEditorFocused() && taskListPageRoot.openCurrentTaskDetails()) {
            event.accepted = true
        }
    }

    SwipeView {
        id: swipeView
        anchors.fill: parent
        currentIndex: taskListPageRoot.currentTab
        onCurrentIndexChanged: {
            if (currentIndex !== taskListPageRoot.currentTab) {
                taskListPageRoot.currentTabRequested(currentIndex)
            }
        }

        TaskListView {
            id: todoListView
            objectName: "todoListView"
            proxyModel: app.todoProxyModel
            isDoneList: false
            showFilter: taskListPageRoot.filterVisible
            showAddEntry: true
        }

        TimerView {
            id: timerView
        }

        TaskListView {
            id: doneListView
            proxyModel: app.doneProxyModel
            isDoneList: true
            showFilter: taskListPageRoot.filterVisible
            showAddEntry: false
        }

        DashboardView {
            id: dashboardView
        }
    }
}
