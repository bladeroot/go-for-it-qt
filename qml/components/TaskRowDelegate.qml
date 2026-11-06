import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ItemDelegate {
    id: rowDelegate

    required property int index
    required property string description
    required property string simpleText
    required property string richText
    required property bool done
    required property string taskStatus
    required property string priorityString
    required property int duration
    required property int timerValue
    required property string jiraKey
    required property string details
    required property var comments

    property int totalCount: 0
    property bool isDoneList: false
    property bool isSelectedActive: false
    property bool editing: false

    signal rowClicked()
    signal moveRequested(int fromIndex, int toIndex)
    signal filterTagClicked(string tagQuery)
    signal detailsRequested()

    highlighted: isSelectedActive
    topPadding: 4
    bottomPadding: 4

    onClicked: rowDelegate.rowClicked()
    onDoubleClicked: {
        editField.text = rowDelegate.simpleText
        rowDelegate.editing = true
        editField.forceActiveFocus()
    }

    function statusShortLabel(st) {
        if (st === "in_progress") return qsTr("In Progress")
        if (st === "paused") return qsTr("Paused")
        if (st === "done") return qsTr("Done")
        if (st === "canceled") return qsTr("Canceled")
        return qsTr("To-Do")
    }

    function statusBadgeColor(st) {
        if (st === "in_progress") return "#2980b9"
        if (st === "paused") return "#d35400"
        if (st === "done") return "#27ae60"
        if (st === "canceled") return "#7f8c8d"
        return "#555555"
    }

    contentItem: RowLayout {
        spacing: 6

        Label {
            id: dragHandle
            text: "⋮⋮"
            opacity: rowDelegate.hovered ? 0.8 : 0.35
            font.bold: true
            visible: !rowDelegate.editing && rowDelegate.totalCount > 1

            MouseArea {
                id: dragArea
                anchors.fill: parent
                anchors.margins: -4
                cursorShape: Qt.SizeVerCursor
                property real startY: 0
                onPressed: function(mouse) {
                    startY = mouse.y
                    rowDelegate.rowClicked()
                }
                onPositionChanged: function(mouse) {
                    const dy = mouse.y - startY
                    if (dy < -rowDelegate.height && rowDelegate.index > 0) {
                        rowDelegate.moveRequested(rowDelegate.index, rowDelegate.index - 1)
                    } else if (dy > rowDelegate.height && rowDelegate.index + 1 < rowDelegate.totalCount) {
                        rowDelegate.moveRequested(rowDelegate.index, rowDelegate.index + 1)
                    }
                }
            }
        }

        CheckBox {
            checked: rowDelegate.done
            onToggled: Qt.callLater(function() {
                app.toggleTaskDone(rowDelegate.isDoneList, rowDelegate.index)
            })
        }

        Label {
            visible: !rowDelegate.editing && rowDelegate.priorityString !== ""
            text: rowDelegate.priorityString
            font.bold: true
            color: palette.highlight
        }

        Label {
            id: taskTextLabel
            Layout.fillWidth: true
            visible: !rowDelegate.editing
            text: rowDelegate.richText
            textFormat: Text.RichText
            wrapMode: Text.WordWrap
            font.strikeout: rowDelegate.done
            color: palette.windowText
            onLinkActivated: function(link) {
                if (link.startsWith("+") || link.startsWith("@")) {
                    rowDelegate.filterTagClicked(link)
                } else {
                    Qt.openUrlExternally(link)
                }
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                cursorShape: parent.hoveredLink !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
        }

        TextField {
            id: editField
            visible: rowDelegate.editing
            Layout.fillWidth: true
            onAccepted: {
                rowDelegate.editing = false
                app.updateTaskText(rowDelegate.isDoneList, rowDelegate.index, text)
            }
            onActiveFocusChanged: {
                if (!activeFocus && rowDelegate.editing) {
                    rowDelegate.editing = false
                    app.updateTaskText(rowDelegate.isDoneList, rowDelegate.index, text)
                }
            }
        }

        Label {
            visible: !rowDelegate.editing && (rowDelegate.duration > 0 || rowDelegate.timerValue > 0)
            Layout.preferredWidth: 68
            horizontalAlignment: Text.AlignRight
            text: Math.floor(rowDelegate.timerValue / 60) + "m / " + Math.round(rowDelegate.duration / 60) + "m"
            font.italic: true
            font.pixelSize: 11
            opacity: 0.75
        }

        ToolButton {
            id: statusBtn
            visible: !rowDelegate.editing
            Layout.preferredWidth: 108
            Layout.minimumWidth: 108
            Layout.maximumWidth: 108
            implicitHeight: 22
            leftPadding: 6
            rightPadding: 6
            topPadding: 0
            bottomPadding: 0
            text: rowDelegate.statusShortLabel(rowDelegate.taskStatus)
            font.pixelSize: 10
            font.bold: true
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Change task status")
            onClicked: statusMenu.open()

            background: Rectangle {
                radius: 4
                color: rowDelegate.statusBadgeColor(rowDelegate.taskStatus)
                opacity: statusBtn.hovered ? 0.35 : 0.2
                border.color: rowDelegate.statusBadgeColor(rowDelegate.taskStatus)
                border.width: 1
            }

            Menu {
                id: statusMenu
                y: statusBtn.height
                MenuItem {
                    text: qsTr("To-Do")
                    onTriggered: Qt.callLater(function() { app.changeTaskStatus(rowDelegate.isDoneList, rowDelegate.index, "todo") })
                }
                MenuItem {
                    text: qsTr("In Progress")
                    onTriggered: Qt.callLater(function() { app.changeTaskStatus(rowDelegate.isDoneList, rowDelegate.index, "in_progress") })
                }
                MenuItem {
                    text: qsTr("Paused")
                    onTriggered: Qt.callLater(function() { app.changeTaskStatus(rowDelegate.isDoneList, rowDelegate.index, "paused") })
                }
                MenuSeparator {}
                MenuItem {
                    text: qsTr("Done")
                    onTriggered: Qt.callLater(function() { app.changeTaskStatus(rowDelegate.isDoneList, rowDelegate.index, "done") })
                }
                MenuItem {
                    text: qsTr("Canceled / Rejected")
                    onTriggered: Qt.callLater(function() { app.changeTaskStatus(rowDelegate.isDoneList, rowDelegate.index, "canceled") })
                }
            }
        }

        ToolButton {
            visible: !rowDelegate.editing
            opacity: (rowDelegate.hovered || rowDelegate.details !== "" || (rowDelegate.comments && rowDelegate.comments.length > 0)) ? 1.0 : 0.0
            enabled: visible && opacity > 0
            implicitWidth: 28
            implicitHeight: 24
            padding: 0
            text: (rowDelegate.comments && rowDelegate.comments.length > 0)
                ? ("💬" + rowDelegate.comments.length)
                : "📄"
            font.pixelSize: 11
            ToolTip.visible: hovered
            ToolTip.text: qsTr("View description & comments")
            onClicked: rowDelegate.detailsRequested()
        }

        ToolButton {
            visible: !rowDelegate.editing
            opacity: rowDelegate.hovered ? 1.0 : 0.0
            enabled: rowDelegate.hovered
            implicitWidth: 24
            implicitHeight: 24
            padding: 0
            text: "✎"
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Edit task")
            onClicked: {
                editField.text = rowDelegate.simpleText
                rowDelegate.editing = true
                editField.forceActiveFocus()
            }
        }

        ToolButton {
            visible: !rowDelegate.editing
            opacity: rowDelegate.hovered ? 1.0 : 0.0
            enabled: rowDelegate.hovered
            implicitWidth: 24
            implicitHeight: 24
            padding: 0
            text: "🗑"
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Delete task")
            onClicked: Qt.callLater(function() {
                app.removeTask(rowDelegate.isDoneList, rowDelegate.index)
            })
        }
    }
}
