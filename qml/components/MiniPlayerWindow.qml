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

    // The mini player is declared inside the main ApplicationWindow, which would make that
    // window its transient parent: hiding the main window (closing to the tray) would then hide
    // the mini player too. It has to be an independent top-level window.
    transientParent: null

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

            // Ask the window system to move the window (works on X11 and Wayland, where a client
            // cannot position its own window). Only if that is unsupported, move it by hand using
            // global coordinates so the delta does not change while the window moves.
            property bool manualMove: false
            property point pressGlobal: Qt.point(0, 0)
            property point startPos: Qt.point(0, 0)

            onPressed: function(mouse) {
                manualMove = !miniPlayer.startSystemMove()
                if (manualMove) {
                    pressGlobal = dragArea.mapToGlobal(mouse.x, mouse.y)
                    startPos = Qt.point(miniPlayer.x, miniPlayer.y)
                }
            }
            onPositionChanged: function(mouse) {
                if (!manualMove || !pressed) return
                const g = dragArea.mapToGlobal(mouse.x, mouse.y)
                miniPlayer.x = startPos.x + (g.x - pressGlobal.x)
                miniPlayer.y = startPos.y + (g.y - pressGlobal.y)
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
