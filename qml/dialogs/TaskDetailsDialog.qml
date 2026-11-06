import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: detailsDialog
    title: issueKey !== "" ? (issueKey + " — " + qsTr("Task Details")) : qsTr("Task Details & Comments")
    anchors.centerIn: parent
    width: 460
    height: 460
    modal: true
    standardButtons: Dialog.Save | Dialog.Close

    property bool isDoneList: false
    property int proxyRow: -1
    property string issueKey: ""
    property string taskTitle: ""
    property string taskDetails: ""
    property var taskComments: []

    function openForTask(doneList, row, key, titleText, detailsText, commentsList) {
        isDoneList = doneList
        proxyRow = row
        issueKey = key || ""
        taskTitle = titleText || ""
        taskDetails = detailsText || ""
        taskComments = commentsList || []
        detailsArea.text = taskDetails
        detailsTabBar.currentIndex = 0
        open()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: detailsDialog.taskTitle
            font.bold: true
            wrapMode: Text.WordWrap
        }

        TabBar {
            id: detailsTabBar
            Layout.fillWidth: true
            TabButton {
                text: qsTr("Description")
            }
            TabButton {
                text: qsTr("Comments (%1)").arg(detailsDialog.taskComments ? detailsDialog.taskComments.length : 0)
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: detailsTabBar.currentIndex

            // Tab 1: Full Description
            ScrollView {
                clip: true
                TextArea {
                    id: detailsArea
                    width: detailsDialog.availableWidth
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    placeholderText: qsTr("No description provided. You can add notes or task description here...")
                }
            }

            // Tab 2: Comments
            ListView {
                id: commentsListView
                clip: true
                spacing: 8
                model: detailsDialog.taskComments

                delegate: Frame {
                    required property var modelData
                    width: commentsListView.width

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: modelData.author || ""
                                font.bold: true
                                font.pixelSize: 12
                            }
                            Item { Layout.fillWidth: true }
                            Label {
                                text: modelData.created || ""
                                font.pixelSize: 11
                                opacity: 0.65
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: modelData.body || ""
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: !detailsDialog.taskComments || detailsDialog.taskComments.length === 0
                    text: qsTr("No comments on this task.")
                    opacity: 0.6
                }
            }
        }
    }

    onAccepted: {
        if (proxyRow >= 0) {
            app.updateTaskDetails(isDoneList, proxyRow, detailsArea.text)
        }
    }
}
