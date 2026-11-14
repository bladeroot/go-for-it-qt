import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Window {
    id: miniPlayer
    objectName: "miniPlayerWindow"
    width: 440
    height: 58
    property bool alwaysOnTop: true
    flags: Qt.Window | Qt.FramelessWindowHint | (miniPlayer.alwaysOnTop ? Qt.WindowStaysOnTopHint : 0)
    color: "transparent"
    title: qsTr("Go for It — Mini Player")

    // The mini player is declared inside the main ApplicationWindow, which would make that
    // window its transient parent: hiding the main window (closing to the tray) would then hide
    // the mini player too. It has to be an independent top-level window.
    transientParent: null

    Component.onCompleted: {
        if (typeof app !== "undefined" && app && app.configureMiniPlayerWindow) {
            app.configureMiniPlayerWindow(miniPlayer, miniPlayer.alwaysOnTop)
        }
    }

    Timer {
        id: pinRetryTimer
        interval: 150
        repeat: false
        onTriggered: {
            if (miniPlayer.visible && typeof app !== "undefined" && app && app.configureMiniPlayerWindow) {
                app.configureMiniPlayerWindow(miniPlayer, miniPlayer.alwaysOnTop)
            }
        }
    }

    onVisibleChanged: {
        if (visible && typeof app !== "undefined" && app && app.configureMiniPlayerWindow) {
            app.configureMiniPlayerWindow(miniPlayer, miniPlayer.alwaysOnTop)
            pinRetryTimer.restart()
        }
    }

    function toggleAlwaysOnTop() {
        const wasVisible = miniPlayer.visible
        miniPlayer.alwaysOnTop = !miniPlayer.alwaysOnTop
        if (typeof app !== "undefined" && app && app.configureMiniPlayerWindow) {
            app.configureMiniPlayerWindow(miniPlayer, miniPlayer.alwaysOnTop)
        }
        if (wasVisible) {
            miniPlayer.show()
            if (miniPlayer.alwaysOnTop) {
                miniPlayer.raise()
            }
        }
    }

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
            spacing: 6

            // Status Icon / Pomodoro indicator
            Label {
                Layout.preferredWidth: 22
                Layout.alignment: Qt.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
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

            // Switch the active task without restoring the main window
            ToolButton {
                objectName: "miniPrevTask"
                Layout.preferredWidth: 24
                Layout.preferredHeight: 28
                Layout.minimumWidth: 24
                Layout.maximumWidth: 24
                Layout.minimumHeight: 28
                Layout.maximumHeight: 28
                Layout.alignment: Qt.AlignVCenter
                padding: 0
                enabled: app.activeTodoRow > 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Previous Task")
                onClicked: app.switchToPreviousTask()
                contentItem: Text {
                    text: "◀"
                    font.pixelSize: 11
                    color: parent.enabled ? palette.buttonText : "#7f8c8d"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            // Task Name, Remaining Time, and Ambient Sound Switcher
            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
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
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        Layout.minimumWidth: 52
                        text: app.timer.formattedRemainingTime
                        font.family: "monospace"
                        font.bold: true
                        font.pixelSize: 14
                        color: app.timer.breakActive ? "#27ae60" : palette.highlight
                    }

                    ToolButton {
                        id: miniAmbientBtn
                        objectName: "miniAmbientToggle"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 20
                        Layout.minimumHeight: 20
                        Layout.maximumHeight: 20
                        Layout.alignment: Qt.AlignVCenter
                        leftPadding: 5
                        rightPadding: 5
                        topPadding: 0
                        bottomPadding: 0
                        ToolTip.visible: hovered
                        ToolTip.text: "🎧 " + (app.ambient ? app.ambient.ambientModeName : "")
                        onClicked: {
                            if (app.ambient) {
                                app.ambient.cycleAmbientMode()
                            }
                        }
                        background: Rectangle {
                            radius: 4
                            color: (app.ambient && app.ambient.ambientMode !== "none") ? palette.highlight : "transparent"
                            opacity: miniAmbientBtn.hovered ? 0.35 : ((app.ambient && app.ambient.ambientMode !== "none") ? 0.2 : 0.0)
                            border.color: (app.ambient && app.ambient.ambientMode !== "none") ? palette.highlight : "#7f8c8d"
                            border.width: 1
                        }
                        contentItem: Text {
                            text: "🎧 " + (app.ambient ? app.ambient.ambientModeName : "")
                            font.pixelSize: 10
                            font.bold: app.ambient && app.ambient.ambientMode !== "none"
                            color: palette.windowText
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }

            ToolButton {
                objectName: "miniNextTask"
                Layout.preferredWidth: 24
                Layout.preferredHeight: 28
                Layout.minimumWidth: 24
                Layout.maximumWidth: 24
                Layout.minimumHeight: 28
                Layout.maximumHeight: 28
                Layout.alignment: Qt.AlignVCenter
                padding: 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Next Task")
                onClicked: app.switchToNextTask()
                contentItem: Text {
                    text: "▶"
                    font.pixelSize: 11
                    color: palette.buttonText
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            // Controls: Play/Pause, Mark Done, Always on Top, Restore, Close
            ToolButton {
                id: miniPlayPauseBtn
                objectName: "miniPlayPauseButton"
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                Layout.minimumWidth: 30
                Layout.maximumWidth: 30
                Layout.minimumHeight: 30
                Layout.maximumHeight: 30
                Layout.alignment: Qt.AlignVCenter
                padding: 0
                ToolTip.visible: hovered
                ToolTip.text: app.timer.running ? qsTr("Pause Timer") : qsTr("Start Timer")
                onClicked: app.timer.toggleRunning()
                contentItem: Item {
                    // Use fixed vector geometry for Play and Pause so switching state never
                    // changes font metrics or causes the button to jump.
                    Row {
                        anchors.centerIn: parent
                        spacing: 3
                        visible: app.timer.running
                        Rectangle { width: 3.5; height: 12; radius: 1; color: palette.buttonText }
                        Rectangle { width: 3.5; height: 12; radius: 1; color: palette.buttonText }
                    }
                    Text {
                        anchors.fill: parent
                        visible: !app.timer.running
                        text: "▶"
                        font.pixelSize: 12
                        font.bold: true
                        color: palette.buttonText
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            ToolButton {
                objectName: "miniDoneButton"
                enabled: app.timer.hasActiveTask && !app.timer.breakActive
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                Layout.minimumWidth: 28
                Layout.maximumWidth: 28
                Layout.minimumHeight: 28
                Layout.maximumHeight: 28
                Layout.alignment: Qt.AlignVCenter
                padding: 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Mark Task as Done")
                onClicked: app.markActiveTaskDone()
                contentItem: Text {
                    text: "✓"
                    font.pixelSize: 13
                    font.bold: true
                    color: parent.enabled ? palette.buttonText : "#7f8c8d"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            ToolButton {
                id: pinBtn
                objectName: "miniAlwaysOnTopToggle"
                Layout.preferredWidth: 26
                Layout.preferredHeight: 26
                Layout.minimumWidth: 26
                Layout.maximumWidth: 26
                Layout.minimumHeight: 26
                Layout.maximumHeight: 26
                Layout.alignment: Qt.AlignVCenter
                padding: 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Always on Top Mini-Player (Ctrl+M)")
                onClicked: miniPlayer.toggleAlwaysOnTop()
                background: Rectangle {
                    radius: 4
                    color: miniPlayer.alwaysOnTop ? palette.highlight : "transparent"
                    opacity: pinBtn.hovered ? 0.4 : (miniPlayer.alwaysOnTop ? 0.25 : 0.0)
                    border.color: miniPlayer.alwaysOnTop ? palette.highlight : "transparent"
                    border.width: 1
                }
                contentItem: Text {
                    text: "📌"
                    font.pixelSize: 11
                    opacity: miniPlayer.alwaysOnTop ? 1.0 : 0.45
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            ToolButton {
                objectName: "miniRestoreButton"
                Layout.preferredWidth: 26
                Layout.preferredHeight: 26
                Layout.minimumWidth: 26
                Layout.maximumWidth: 26
                Layout.minimumHeight: 26
                Layout.maximumHeight: 26
                Layout.alignment: Qt.AlignVCenter
                padding: 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Restore Full Window")
                onClicked: {
                    miniPlayer.hide()
                    rootWindow.show()
                    rootWindow.raise()
                    rootWindow.requestActivate()
                }
                contentItem: Text {
                    text: "⤢"
                    font.pixelSize: 12
                    color: palette.buttonText
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            ToolButton {
                objectName: "miniCloseButton"
                Layout.preferredWidth: 24
                Layout.preferredHeight: 24
                Layout.minimumWidth: 24
                Layout.maximumWidth: 24
                Layout.minimumHeight: 24
                Layout.maximumHeight: 24
                Layout.alignment: Qt.AlignVCenter
                padding: 0
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Close Mini Player")
                onClicked: miniPlayer.hide()
                contentItem: Text {
                    text: "✕"
                    font.pixelSize: 10
                    color: palette.buttonText
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
}
