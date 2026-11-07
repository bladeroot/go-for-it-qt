import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../dialogs"

Item {
    id: selectionRoot

    property var checkedListIds: []

    function openNewListDialog() {
        listEditDialog.openForNew()
    }

    function toggleCheckedList(id) {
        let arr = checkedListIds.slice()
        const idx = arr.indexOf(id)
        if (idx >= 0) {
            arr.splice(idx, 1)
        } else {
            arr.push(id)
        }
        checkedListIds = arr
    }

    function isListChecked(id) {
        return checkedListIds.indexOf(id) >= 0
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        // Quick Filter Bar
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Button {
                text: "🌐 " + qsTr("All Lists")
                highlighted: app.isMultiListView && app.activeListIds.length === 0
                font.pixelSize: 11
                onClicked: app.showAllLists()
            }

            Button {
                text: "💻 " + qsTr("Local")
                font.pixelSize: 11
                onClicked: app.showProviderLists("SQLite")
            }

            Button {
                text: "☁️ " + qsTr("All Trackers")
                font.pixelSize: 11
                onClicked: {
                    let trackerIds = []
                    for (let i = 0; i < app.listManager.count; ++i) {
                        const entry = app.listManager.entryAt(i)
                        if (entry.provider !== "SQLite" && entry.provider !== "Todo.txt") {
                            trackerIds.push(entry.id)
                        }
                    }
                    if (trackerIds.length > 0) {
                        app.setMultiListFilter(trackerIds)
                    }
                }
            }

            Item { Layout.fillWidth: true }

            Button {
                visible: selectionRoot.checkedListIds.length > 0
                text: qsTr("Show selected (%1)").arg(selectionRoot.checkedListIds.length)
                highlighted: true
                font.bold: true
                font.pixelSize: 11
                onClicked: app.setMultiListFilter(selectionRoot.checkedListIds)
            }
        }

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
                highlighted: (listId === app.currentListId && !app.isMultiListView) || selectionRoot.isListChecked(listId)

                contentItem: RowLayout {
                    spacing: 8

                    CheckBox {
                        id: listMultiCheck
                        checked: selectionRoot.isListChecked(listRowDelegate.listId)
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Select to view together with other lists")
                        onToggled: selectionRoot.toggleCheckedList(listRowDelegate.listId)
                    }

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

                        RowLayout {
                            spacing: 6
                            Label {
                                text: listRowDelegate.name
                                font.bold: true
                                elide: Text.ElideRight
                            }

                            Rectangle {
                                implicitHeight: 16
                                implicitWidth: providerBadgeLabel.implicitWidth + 6
                                radius: 3
                                color: {
                                    if (listRowDelegate.provider === "GitHub") return "#24292f"
                                    if (listRowDelegate.provider === "GitLab") return "#e24329"
                                    if (listRowDelegate.provider === "Jira") return "#0052cc"
                                    if (listRowDelegate.provider === "YouTrack") return "#e02f5c"
                                    if (listRowDelegate.provider === "Redmine") return "#d32f2f"
                                    if (listRowDelegate.provider === "Todoist") return "#e44332"
                                    return "#5c6b73"
                                }
                                opacity: 0.85

                                Label {
                                    id: providerBadgeLabel
                                    anchors.centerIn: parent
                                    text: listRowDelegate.provider
                                    font.pixelSize: 9
                                    font.bold: true
                                    color: "#ffffff"
                                }
                            }
                        }

                        Label {
                            text: listRowDelegate.directoryPath
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
