import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

Item {
    id: taskListPageRoot

    property int currentTab: 0
    property bool filterVisible: false
    signal currentTabRequested(int tabIndex)

    function focusNewTaskInput() {
        todoListView.focusInput()
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
    }
}
