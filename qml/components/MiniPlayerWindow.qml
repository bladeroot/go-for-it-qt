import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: miniPlayer
    width: 330
    height: 56
    flags: Qt.Window | Qt.WindowStaysOnTopHint | Qt.FramelessWindowHint | Qt.Tool
    color: "transparent"
    title: qsTr("Go for It — Mini Player")

    property real dragStartX: 0
    property real dragStartY: 0

    Rectangle {
        id: bg
        anchors.fill: parent
        radius: 12
        color: app.settings.useDarkTheme ? "#1b1e20" : "#ffffff"
        opacity: 0.94
        border.color: app.timer.breakActive ? "#27ae60" : (app.timer.running ? palette.highlight : "#7f8c8d")
        border.width: 1.5

        MouseArea {
            id: dragArea
            anchors.fill: parent
            cursorShape: Qt.SizeAllCursor
            property point clickPos: "0,0"
            onPressed: function(mouse) {
                clickPos = Qt.point(mouse.x, mouse.y)
            }
            onPositionChanged: function(mouse) {
                const delta = Qt.point(mouse.x - clickPos.x, mouse.y - clickPos.y)
                miniPlayer.x += delta.x
                miniPlayer.y += delta.y
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            spacing: 8

            // Status Icon / Pomodoro indicator
            Label {
                text: app.timer.breakActive ? "☕" : "🍅"
                font.pixelSize: 18
                ToolTip.visible: iconMa.containsMouse
                ToolTip.text: app.timer.breakActive ? qsTr("Break Phase") : qsTr("Focus Work Phase")
                MouseArea {
                    id: iconMa
                    anchors.fill: parent
                    hoverEnabled: true
                }
            }

            // Task Name and Remaining Time
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1

                Label {
                    Layout.fillWidth: true
                    text: app.timer.hasActiveTask ? app.timer.activeTaskDescription : qsTr("No active task")
                    font.bold: true
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    color: palette.windowText
                }

                RowLayout {
                    spacing: 6
                    Label {
                        text: app.timer.formattedRemainingTime
                        font.bold: true
                        font.pixelSize: 15
                        color: app.timer.breakActive ? "#27ae60" : palette.highlight
                    }

                    Label {
                        visible: app.ambient && app.ambient.isPlaying
                        text: "🎧"
                        font.pixelSize: 10
                        opacity: 0.75
                    }
                }
            }

            // Controls: Play/Pause, Mark Done, Restore, Close
            ToolButton {
                implicitWidth: 30
                implicitHeight: 30
                padding: 0
                text: app.timer.running ? "⏸" : "▶"
                font.pixelSize: 13
                font.bold: true
                ToolTip.visible: hovered
                ToolTip.text: app.timer.running ? qsTr("Pause Timer") : qsTr("Start Timer")
                onClicked: app.timer.toggleRunning()
            }

            ToolButton {
                visible: app.timer.hasActiveTask && !app.timer.breakActive
                implicitWidth: 30
                implicitHeight: 30
                padding: 0
                text: "✓"
                font.pixelSize: 13
                font.bold: true
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Mark Task as Done")
                onClicked: app.markActiveTaskDone()
            }

            ToolButton {
                implicitWidth: 26
                implicitHeight: 26
                padding: 0
                text: "⤢"
                font.pixelSize: 12
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Restore Full Window")
                onClicked: {
                    miniPlayer.hide()
                    rootWindow.show()
                    rootWindow.raise()
                    rootWindow.requestActivate()
                }
            }

            ToolButton {
                implicitWidth: 24
                implicitHeight: 24
                padding: 0
                text: "✕"
                font.pixelSize: 10
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Close Mini Player")
                onClicked: miniPlayer.hide()
            }
        }
    }
}
