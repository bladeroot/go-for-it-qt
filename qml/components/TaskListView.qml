import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../dialogs"

Item {
    id: listRoot

    required property var proxyModel
    property bool isDoneList: false
    property bool showFilter: false
    property bool showAddEntry: true

    function focusInput() {
        newTaskField.forceActiveFocus()
    }

    function delegateAt(proxyRow) {
        tasksView.forceLayout()
        return tasksView.itemAtIndex(proxyRow)
    }

    function openDetailsForRow(proxyRow) {
        if (proxyRow < 0 || !listRoot.proxyModel || proxyRow >= tasksView.count) {
            return false
        }
        app.markTaskViewed(listRoot.isDoneList, proxyRow)
        const info = app.getTaskDetailsAt(listRoot.isDoneList, proxyRow)
        if (!info || info.id === undefined) {
            return false
        }
        taskDetailsDialog.openForTask(
            listRoot.isDoneList,
            proxyRow,
            info.jiraKey !== "" ? info.jiraKey : info.description,
            info.status,
            info.details,
            info.comments,
            info.webUrl,
            info.priorityString,
            info.dueDateString
        )
        return true
    }

    function openSelectedTaskDetails() {
        let row = -1
        if (!listRoot.isDoneList && listRoot.proxyModel && app.activeTodoRow >= 0) {
            row = listRoot.proxyModel.mapFromSourceRow(app.activeTodoRow)
        }
        if (row < 0) {
            row = tasksView.currentIndex
        }
        if (row < 0 && tasksView.count > 0) {
            row = 0
        }
        return listRoot.openDetailsForRow(row)
    }

    Connections {
        target: app
        function onActiveTodoRowChanged() {
            if (!listRoot.isDoneList && listRoot.proxyModel && app.activeTodoRow >= 0) {
                const proxyRow = listRoot.proxyModel.mapFromSourceRow(app.activeTodoRow)
                if (proxyRow >= 0 && proxyRow < tasksView.count) {
                    tasksView.currentIndex = proxyRow
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Sync status / error bar for remote lists
        Rectangle {
            id: syncStatusBanner
            Layout.fillWidth: true
            visible: app.isCurrentListRemote
                     && (app.issueTracker.syncing || app.issueTracker.lastStatusMessage !== "")
            implicitHeight: statusRow.implicitHeight + 10
            color: app.issueTracker.lastStatusMessage.indexOf("error") >= 0
                ? "#33c0392b"
                : "#223daee9"

            RowLayout {
                id: statusRow
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    text: app.issueTracker.syncing
                        ? qsTr("Syncing remote issues (%1)...").arg(app.currentFilterTitle)
                        : app.issueTracker.lastStatusMessage
                }

                Button {
                    text: qsTr("↻ Sync")
                    enabled: !app.issueTracker.syncing
                    implicitHeight: 22
                    font.pixelSize: 11
                    onClicked: app.syncCurrentList()
                }
            }
        }

        // Sub-status filter bar (To-Do / In Progress / Paused OR Done / Canceled)
        TabBar {
            id: statusSubBar
            Layout.fillWidth: true
            currentIndex: 0
            onCurrentIndexChanged: {
                if (!listRoot.proxyModel) return
                if (!listRoot.isDoneList) {
                    const activeStatuses = ["", "todo", "in_progress", "paused"]
                    listRoot.proxyModel.statusFilter = activeStatuses[currentIndex] || ""
                } else {
                    const closedStatuses = ["", "done", "canceled"]
                    listRoot.proxyModel.statusFilter = closedStatuses[currentIndex] || ""
                }
            }

            Repeater {
                model: listRoot.isDoneList
                    ? [qsTr("All"), qsTr("Done"), qsTr("Canceled / Rejected")]
                    : [qsTr("All"), qsTr("To-Do"), qsTr("In Progress"), qsTr("Paused")]

                TabButton {
                    required property string modelData
                    text: modelData
                    font.pixelSize: 12
                }
            }
        }

        // Smart Due Filters (All / Overdue / Today / This Week)
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.topMargin: 2
            Layout.bottomMargin: 2
            visible: !listRoot.isDoneList
            spacing: 6

            Label {
                text: qsTr("Due:")
                font.pixelSize: 11
                font.bold: true
                opacity: 0.7
            }

            ButtonGroup { id: dueGroup }

            Button {
                text: qsTr("All")
                checkable: true
                checked: listRoot.proxyModel ? (listRoot.proxyModel.dueFilter === "" || listRoot.proxyModel.dueFilter === "all") : true
                ButtonGroup.group: dueGroup
                font.pixelSize: 11
                implicitHeight: 22
                padding: 4
                onClicked: if (listRoot.proxyModel) listRoot.proxyModel.dueFilter = ""
            }

            Button {
                text: "⏰ " + qsTr("Overdue")
                checkable: true
                checked: listRoot.proxyModel ? listRoot.proxyModel.dueFilter === "overdue" : false
                ButtonGroup.group: dueGroup
                font.pixelSize: 11
                implicitHeight: 22
                padding: 4
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 11
                    font.bold: parent.checked
                    color: parent.checked ? "#e74c3c" : palette.windowText
                }
                onClicked: if (listRoot.proxyModel) listRoot.proxyModel.dueFilter = "overdue"
            }

            Button {
                text: "📅 " + qsTr("Today")
                checkable: true
                checked: listRoot.proxyModel ? listRoot.proxyModel.dueFilter === "today" : false
                ButtonGroup.group: dueGroup
                font.pixelSize: 11
                implicitHeight: 22
                padding: 4
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 11
                    font.bold: parent.checked
                    color: parent.checked ? "#e67e22" : palette.windowText
                }
                onClicked: if (listRoot.proxyModel) listRoot.proxyModel.dueFilter = "today"
            }

            Button {
                text: "🗓 " + qsTr("This Week")
                checkable: true
                checked: listRoot.proxyModel ? listRoot.proxyModel.dueFilter === "week" : false
                ButtonGroup.group: dueGroup
                font.pixelSize: 11
                implicitHeight: 22
                padding: 4
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 11
                    font.bold: parent.checked
                    color: parent.checked ? "#2980b9" : palette.windowText
                }
                onClicked: if (listRoot.proxyModel) listRoot.proxyModel.dueFilter = "week"
            }

            Item { Layout.fillWidth: true }
        }

        ToolBar {
            Layout.fillWidth: true
            visible: listRoot.showFilter
            RowLayout {
                anchors.fill: parent
                anchors.margins: 4
                TextField {
                    id: filterField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Filter by word, +project, @context, priority:a-c...")
                    text: listRoot.proxyModel ? listRoot.proxyModel.filterQuery : ""
                    onTextChanged: {
                        if (listRoot.proxyModel) {
                            listRoot.proxyModel.filterQuery = text
                        }
                    }
                    Keys.onReturnPressed: function(event) { event.accepted = true }
                    Keys.onEnterPressed: function(event) { event.accepted = true }
                }
                ToolButton {
                    text: "✕"
                    visible: filterField.text.length > 0
                    onClicked: filterField.text = ""
                }
            }
        }

        ListView {
            id: tasksView
            objectName: listRoot.isDoneList ? "doneTasksView" : "todoTasksView"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            focus: true
            keyNavigationEnabled: true
            model: listRoot.proxyModel
            spacing: 2

            onActiveFocusChanged: {
                if (activeFocus && !listRoot.isDoneList && listRoot.proxyModel && app.activeTodoRow >= 0) {
                    const proxyRow = listRoot.proxyModel.mapFromSourceRow(app.activeTodoRow)
                    if (proxyRow >= 0 && proxyRow < tasksView.count) {
                        tasksView.currentIndex = proxyRow
                    }
                }
            }

            Keys.onReturnPressed: function(event) {
                if (listRoot.openSelectedTaskDetails()) {
                    event.accepted = true
                }
            }
            Keys.onEnterPressed: function(event) {
                if (listRoot.openSelectedTaskDetails()) {
                    event.accepted = true
                }
            }

            delegate: TaskRowDelegate {
                width: tasksView.width
                totalCount: tasksView.count
                isDoneList: listRoot.isDoneList
                isSelectedActive: (!listRoot.isDoneList
                    && listRoot.proxyModel.mapToSourceRow(index) === app.activeTodoRow)
                    || (listRoot.isDoneList && tasksView.currentIndex === index)
                onRowClicked: {
                    tasksView.currentIndex = index
                    tasksView.forceActiveFocus()
                    if (!listRoot.isDoneList) {
                        app.activeTodoRow = listRoot.proxyModel.mapToSourceRow(index)
                    }
                    app.markTaskViewed(listRoot.isDoneList, index)
                }
                onMoveRequested: function(fromRow, toRow) {
                    app.moveTaskRow(listRoot.isDoneList, fromRow, toRow)
                }
                onFilterTagClicked: function(tagFilter) {
                    filterField.text = tagFilter
                    if (listRoot.proxyModel) {
                        listRoot.proxyModel.filterQuery = tagFilter
                    }
                }
                onDetailsRequested: {
                    app.markTaskViewed(listRoot.isDoneList, index)
                    const webUrl = app.getTaskBrowserUrl(listId, jiraKey, description).toString()
                    taskDetailsDialog.openForTask(
                        listRoot.isDoneList,
                        index,
                        jiraKey !== "" ? jiraKey : description,
                        taskStatus,
                        details,
                        comments,
                        webUrl,
                        priorityString,
                        dueDateString
                    )
                }
            }

            Label {
                anchors.centerIn: parent
                visible: tasksView.count === 0
                text: listRoot.isDoneList
                    ? qsTr("No completed or canceled tasks in this view.")
                    : qsTr("No tasks in this status view.")
                opacity: 0.6
            }
        }

        Pane {
            Layout.fillWidth: true
            visible: listRoot.showAddEntry
            padding: 8

            RowLayout {
                anchors.fill: parent
                spacing: 6

                TextField {
                    id: newTaskField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Add new task (e.g. (A) Write code +Project @context duration:25m)...")
                    onAccepted: {
                        if (text.trim() !== "") {
                            app.addNewTask(text)
                            text = ""
                        }
                    }
                    Keys.onReturnPressed: function(event) {
                        accepted()
                        event.accepted = true
                    }
                    Keys.onEnterPressed: function(event) {
                        accepted()
                        event.accepted = true
                    }
                }

                Button {
                    text: "+"
                    enabled: newTaskField.text.trim() !== ""
                    onClicked: {
                        app.addNewTask(newTaskField.text)
                        newTaskField.text = ""
                    }
                }
            }
        }
    }

    TaskDetailsDialog {
        id: taskDetailsDialog
        objectName: listRoot.isDoneList ? "doneTaskDetailsDialog" : "todoTaskDetailsDialog"
    }
}
