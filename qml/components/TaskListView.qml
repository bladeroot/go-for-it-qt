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
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: listRoot.proxyModel
            spacing: 2

            delegate: TaskRowDelegate {
                width: tasksView.width
                totalCount: tasksView.count
                isDoneList: listRoot.isDoneList
                isSelectedActive: !listRoot.isDoneList
                    && listRoot.proxyModel.mapToSourceRow(index) === app.activeTodoRow
                onRowClicked: {
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
                    taskDetailsDialog.openForTask(
                        listRoot.isDoneList,
                        index,
                        jiraKey,
                        description,
                        details,
                        comments
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
    }
}
