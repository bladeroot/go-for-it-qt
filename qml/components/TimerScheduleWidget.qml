import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Frame {
    id: schedWidgetRoot

    property var schedulePairs: [1500, 300]
    signal scheduleModified(var newPairs)

    function loadSchedule(rawList) {
        pairsModel.clear()
        if (!rawList || rawList.length < 2) {
            pairsModel.append({ taskMin: 25, breakMin: 5 })
            return
        }
        for (let i = 0; i + 1 < rawList.length; i += 2) {
            pairsModel.append({
                taskMin: Math.max(1, Math.round(rawList[i] / 60)),
                breakMin: Math.max(1, Math.round(rawList[i + 1] / 60))
            })
        }
    }

    function exportRawSeconds() {
        let out = []
        for (let i = 0; i < pairsModel.count; ++i) {
            const item = pairsModel.get(i)
            out.push(item.taskMin * 60)
            out.push(item.breakMin * 60)
        }
        return out
    }

    Component.onCompleted: loadSchedule(schedulePairs)

    ListModel {
        id: pairsModel
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Task duration (min)")
                font.bold: true
                font.pixelSize: 12
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Break duration (min)")
                font.bold: true
                font.pixelSize: 12
            }
        }

        ListView {
            id: pairsList
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            clip: true
            model: pairsModel
            spacing: 4

            delegate: RowLayout {
                required property int index
                required property int taskMin
                required property int breakMin

                width: pairsList.width
                spacing: 8

                SpinBox {
                    Layout.fillWidth: true
                    from: 1
                    to: 1439
                    value: taskMin
                    onValueModified: {
                        pairsModel.setProperty(index, "taskMin", value)
                        schedWidgetRoot.scheduleModified(schedWidgetRoot.exportRawSeconds())
                    }
                }

                SpinBox {
                    Layout.fillWidth: true
                    from: 1
                    to: 1439
                    value: breakMin
                    onValueModified: {
                        pairsModel.setProperty(index, "breakMin", value)
                        schedWidgetRoot.scheduleModified(schedWidgetRoot.exportRawSeconds())
                    }
                }

                ToolButton {
                    text: "✕"
                    enabled: pairsModel.count > 1
                    onClicked: {
                        pairsModel.remove(index)
                        schedWidgetRoot.scheduleModified(schedWidgetRoot.exportRawSeconds())
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Button {
                Layout.fillWidth: true
                text: qsTr("+ Add Interval")
                onClicked: {
                    pairsModel.append({ taskMin: 25, breakMin: 5 })
                    schedWidgetRoot.scheduleModified(schedWidgetRoot.exportRawSeconds())
                }
            }
        }
    }
}
