import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: timerViewRoot

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
                implicitWidth: 150
                textRole: "label"
                valueRole: "id"
                model: app.ambient ? app.ambient.availableModes() : []
                currentIndex: {
                    if (!app.ambient) return 0
                    for (let i = 0; i < count; ++i) {
                        if (model[i].id === app.ambient.ambientMode) return i
                    }
                    return 0
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
}
