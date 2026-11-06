import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../dialogs"

Item {
    id: selectionRoot

    function openNewListDialog() {
        listEditDialog.openForNew()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        ListView {
            id: listOverview
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: app.listManager
            spacing: 4

            delegate: ItemDelegate {
                id: listRowDelegate
                required property int index
                required property string listId
                required property string name
                required property string provider
                required property string directoryPath

                width: listOverview.width
                highlighted: listId === app.currentListId

                contentItem: RowLayout {
                    spacing: 8

                    ColumnLayout {
                        spacing: 0
                        visible: listOverview.count > 1
                        ToolButton {
                            text: "▲"
                            font.pixelSize: 9
                            implicitWidth: 22
                            implicitHeight: 16
                            enabled: listRowDelegate.index > 0
                            onClicked: app.listManager.moveItem(listRowDelegate.index, listRowDelegate.index - 1)
                        }
                        ToolButton {
                            text: "▼"
                            font.pixelSize: 9
                            implicitWidth: 22
                            implicitHeight: 16
                            enabled: listRowDelegate.index + 1 < listOverview.count
                            onClicked: app.listManager.moveItem(listRowDelegate.index, listRowDelegate.index + 1)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            text: listRowDelegate.name
                            font.bold: true
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            text: listRowDelegate.provider + " • " + listRowDelegate.directoryPath
                            font.pixelSize: 11
                            opacity: 0.7
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                    }

                    ToolButton {
                        text: "✎"
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Edit list properties")
                        onClicked: listEditDialog.openForEdit(listRowDelegate.listId)
                    }

                    ToolButton {
                        text: "🗑"
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Delete list")
                        visible: listOverview.count > 1
                        onClicked: app.listManager.removeListById(listRowDelegate.listId)
                    }
                }

                onClicked: app.selectListById(listId)
            }

            Label {
                anchors.centerIn: parent
                visible: listOverview.count === 0
                text: qsTr("Currently, no lists are configured.\nAdd one below!")
                horizontalAlignment: Text.AlignHCenter
                opacity: 0.65
            }
        }

        Button {
            Layout.fillWidth: true
            text: qsTr("Add list")
            onClicked: listEditDialog.openForNew()
        }
    }

    TxtListEditDialog {
        id: listEditDialog
        onConflictDetected: function(cfg, msg) {
            conflictDialog.pendingConfig = cfg
            conflictDialog.conflictText = msg
            conflictDialog.open()
        }
    }

    FileConflictDialog {
        id: conflictDialog
    }
}
