import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../dialogs"

Item {
    id: timerViewRoot
    objectName: "timerViewRoot"
    focus: true

    function openActiveTaskDetails() {
        if (!app.timer.hasActiveTask) {
            return false
        }
        const info = app.getActiveTaskDetails()
        if (!info || info.id === undefined) {
            return false
        }
        timerTaskDetailsDialog.openForTask(
            false,
            info.proxyRow !== undefined ? info.proxyRow : -1,
            info.jiraKey !== "" ? info.jiraKey : info.description,
            info.status,
            info.details,
            info.comments,
            info.webUrl,
            info.priorityString,
            info.dueDateString
        )
        if (info.id > 0) {
            timerTaskDetailsDialog.taskId = info.id
        }
        return true
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
        if (!timerViewRoot.isEditorFocused() && timerViewRoot.openActiveTaskDetails()) {
            event.accepted = true
        }
    }
    Keys.onEnterPressed: function(event) {
        if (!timerViewRoot.isEditorFocused() && timerViewRoot.openActiveTaskDetails()) {
            event.accepted = true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 16

        Item { Layout.fillHeight: true }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: {
                if (!app.timer.hasActiveTask) return qsTr("Relax.")
                if (app.timer.breakActive) return qsTr("Take a Break!")
                return qsTr("Active Task:")
            }
            font.pixelSize: 16
            opacity: 0.8
        }

        Label {
            id: activeTaskLabel
            objectName: "timerActiveTaskLabel"
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignHCenter
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
            text: app.timer.hasActiveTask
                ? app.timer.activeTaskDescription
                : qsTr("You have nothing to do.")
            font.pixelSize: 20
            font.bold: true
            ToolTip.visible: activeTaskMouseArea.containsMouse && app.timer.hasActiveTask
            ToolTip.text: qsTr("Double-click or press Enter to view description & comments")

            MouseArea {
                id: activeTaskMouseArea
                objectName: "timerActiveTaskMouseArea"
                anchors.fill: parent
                hoverEnabled: true
                enabled: app.timer.hasActiveTask
                cursorShape: app.timer.hasActiveTask ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: timerViewRoot.forceActiveFocus()
                onDoubleClicked: timerViewRoot.openActiveTaskDetails()
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            visible: app.timer.hasActiveTask && app.timer.activeTaskDuration > 0
            text: Math.floor(app.timer.activeTaskTimerValue / 60) + "m / " + Math.round(app.timer.activeTaskDuration / 60) + "m"
            font.italic: true
            color: app.timer.activeTaskTimerValue >= app.timer.activeTaskDuration ? "#c0392b" : palette.windowText
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 6

            SpinBox {
                id: hoursSpin
                from: 0
                to: 59
                editable: !app.timer.running
                enabled: !app.timer.running
                value: Math.floor(app.timer.remainingSeconds / 3600)
                textFromValue: function(value) { return value < 10 ? "0" + value : "" + value }
                onValueModified: app.timer.setTimeHMS(hoursSpin.value, minutesSpin.value, secondsSpin.value)
                Keys.onReturnPressed: function(event) { event.accepted = true }
                Keys.onEnterPressed: function(event) { event.accepted = true }
            }

            Label {
                text: ":"
                font.pixelSize: 28
                font.bold: true
            }

            SpinBox {
                id: minutesSpin
                from: 0
                to: 59
                editable: !app.timer.running
                enabled: !app.timer.running
                value: Math.floor((app.timer.remainingSeconds / 60) % 60)
                textFromValue: function(value) { return value < 10 ? "0" + value : "" + value }
                onValueModified: app.timer.setTimeHMS(hoursSpin.value, minutesSpin.value, secondsSpin.value)
                Keys.onReturnPressed: function(event) { event.accepted = true }
                Keys.onEnterPressed: function(event) { event.accepted = true }
            }

            Label {
                text: ":"
                font.pixelSize: 28
                font.bold: true
            }

            SpinBox {
                id: secondsSpin
                from: 0
                to: 59
                editable: !app.timer.running
                enabled: !app.timer.running
                value: app.timer.remainingSeconds % 60
                textFromValue: function(value) { return value < 10 ? "0" + value : "" + value }
                onValueModified: app.timer.setTimeHMS(hoursSpin.value, minutesSpin.value, secondsSpin.value)
                Keys.onReturnPressed: function(event) { event.accepted = true }
                Keys.onEnterPressed: function(event) { event.accepted = true }
            }
        }

        // Ambient Focus Sound & Mini Player
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 10

            Label {
                text: "🎧 " + qsTr("Ambient Sound:")
                font.pixelSize: 12
                opacity: 0.85
            }

            ComboBox {
                id: ambientCombo
                objectName: "timerAmbientCombo"
                implicitWidth: 210
                textRole: "name"
                valueRole: "id"
                model: app.ambient ? app.ambient.availableModes() : []
                function syncMode() {
                    if (!app.ambient) return
                    for (let i = 0; i < count; ++i) {
                        if (model[i].id === app.ambient.ambientMode) {
                            currentIndex = i
                            return
                        }
                    }
                    currentIndex = 0
                }
                Component.onCompleted: syncMode()
                Connections {
                    target: app.ambient
                    function onAmbientModeChanged() {
                        ambientCombo.syncMode()
                    }
                }
                onActivated: {
                    if (app.ambient) {
                        app.ambient.ambientMode = currentValue
                    }
                }
            }

            Slider {
                id: volumeSlider
                implicitWidth: 90
                from: 0.0
                to: 1.0
                stepSize: 0.05
                value: app.ambient ? app.ambient.volume : 0.5
                visible: app.ambient && app.ambient.ambientMode !== "none"
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Volume: %1%").arg(Math.round(value * 100))
                onMoved: {
                    if (app.ambient) {
                        app.ambient.volume = value
                    }
                }
            }

            Button {
                text: "🗗 " + qsTr("Mini Player")
                font.pixelSize: 12
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Always on Top Mini-Player (Ctrl+M)")
                onClicked: rootWindow.toggleMiniPlayer()
            }
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            visible: app.timer.hasActiveTask
            spacing: 10

            Button {
                text: qsTr("Done")
                visible: !app.timer.breakActive
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Mark the active task as complete")
                onClicked: app.markActiveTaskDone()
            }

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Skip")
                ToolTip.visible: hovered
                ToolTip.text: app.timer.breakActive
                    ? qsTr("Skip the remainder of the break")
                    : qsTr("Skip to the break")
                onClicked: app.timer.skipIteration()
            }

            Button {
                highlighted: !app.timer.running
                text: app.timer.running ? qsTr("Pause") : qsTr("Start")
                onClicked: app.timer.toggleRunning()
            }
        }

        ProgressBar {
            Layout.fillWidth: true
            from: 0.0
            to: 1.0
            value: app.timer.progress
        }
    }

    TaskDetailsDialog {
        id: timerTaskDetailsDialog
        objectName: "timerTaskDetailsDialog"
    }
}
