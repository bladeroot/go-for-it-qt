import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: detailsDialog
    title: taskTitle !== "" ? taskTitle : qsTr("Task Details & Markdown Viewer")
    anchors.centerIn: parent
    width: 680
    height: 540
    modal: true
    standardButtons: Dialog.Save | Dialog.Close

    property bool fromDoneList: false
    property int taskRow: -1
    property var taskId: -1
    property string taskTitle: ""
    property string taskStatus: "todo"
    property string taskPriority: ""
    property string taskDueDate: ""
    property string taskDetails: ""
    property var taskComments: []
    property bool editMode: false
    property string loadedFilePath: ""
    property string taskWebUrl: ""

    function openForTask(isDoneList, rowIdx, titleStr, statusStr, detailsStr, commentsList, webUrl, prioStr, dueStr) {
        fromDoneList = isDoneList
        taskRow = rowIdx
        taskId = (typeof app !== "undefined" && app && app.getTaskIdAt) ? app.getTaskIdAt(isDoneList, rowIdx) : -1
        taskTitle = titleStr
        taskStatus = statusStr || "todo"
        taskPriority = prioStr || ""
        taskDueDate = dueStr || ""
        taskDetails = detailsStr || ""
        taskComments = commentsList || []
        taskWebUrl = webUrl || ""
        detailsArea.text = taskDetails
        loadedFilePath = ""
        editMode = (taskDetails.trim() === "")
        detailsTabBar.currentIndex = 0
        prioCombo.syncPrio()
        open()
    }

    FileDialog {
        id: mdFileDialog
        title: qsTr("Open Markdown (.md) File")
        nameFilters: [qsTr("Markdown files (*.md *.markdown *.txt)"), qsTr("All files (*)")]
        onAccepted: {
            let path = selectedFile.toString()
            if (path.startsWith("file://")) {
                path = path.substring(7)
            }
            path = decodeURIComponent(path)
            const content = app.readMarkdownFile(path)
            if (content !== "") {
                detailsDialog.loadedFilePath = path
                detailsArea.text = content
                detailsDialog.editMode = false
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                text: qsTr("Priority:")
                font.bold: true
                font.pixelSize: 11
                opacity: 0.8
            }
            ComboBox {
                id: prioCombo
                implicitWidth: 160
                model: [
                    { id: "A", label: qsTr("(A) - Highest") },
                    { id: "B", label: qsTr("(B) - High") },
                    { id: "C", label: qsTr("(C) - Medium") },
                    { id: "D", label: qsTr("(D) - Low") },
                    { id: "E", label: qsTr("(E) - Lowest") },
                    { id: "none", label: qsTr("None (No priority)") }
                ]
                textRole: "label"
                valueRole: "id"
                function syncPrio() {
                    const p = (detailsDialog.taskPriority || "").replace("(", "").replace(")", "").trim().toUpperCase()
                    for (let i = 0; i < model.length; ++i) {
                        if (model[i].id === p) {
                            currentIndex = i
                            return
                        }
                    }
                    currentIndex = model.length - 1
                }
                onActivated: function(idx) {
                    if (detailsDialog.taskId > 0 && typeof app !== "undefined" && app && app.changeTaskPriorityById) {
                        app.changeTaskPriorityById(detailsDialog.taskId, model[idx].id)
                    } else {
                        app.changeTaskPriority(detailsDialog.fromDoneList, detailsDialog.taskRow, model[idx].id)
                    }
                }
            }
            Item { Layout.fillWidth: true }
            Label {
                visible: detailsDialog.taskDueDate !== ""
                text: "📅 " + qsTr("Due: %1").arg(detailsDialog.taskDueDate)
                font.bold: true
                font.pixelSize: 11
                opacity: 0.85
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TabBar {
                id: detailsTabBar
                Layout.fillWidth: true
                TabButton { text: qsTr("Markdown Description") }
                TabButton { text: qsTr("Comments (%1)").arg(detailsDialog.taskComments ? detailsDialog.taskComments.length : 0) }
            }

            Button {
                visible: detailsTabBar.currentIndex === 0
                text: detailsDialog.editMode ? qsTr("👁 Preview MD") : qsTr("✎ Edit MD")
                onClicked: detailsDialog.editMode = !detailsDialog.editMode
            }

            Button {
                visible: detailsTabBar.currentIndex === 0
                text: qsTr("📂 Open .md...")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Open and view a local Markdown (.md) file")
                onClicked: mdFileDialog.open()
            }

            Button {
                visible: detailsDialog.taskWebUrl !== ""
                text: qsTr("🌐 Open in Browser")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Open task in web browser (%1)").arg(detailsDialog.taskWebUrl)
                onClicked: app.openUrlExternally(detailsDialog.taskWebUrl)
            }
        }

        Label {
            visible: detailsDialog.loadedFilePath !== "" && detailsTabBar.currentIndex === 0
            text: qsTr("Viewing file: %1").arg(detailsDialog.loadedFilePath)
            font.pixelSize: 11
            opacity: 0.75
            elide: Text.ElideMiddle
            Layout.fillWidth: true
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: detailsTabBar.currentIndex

            // Tab 1: Markdown Preview & Editor
            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: detailsDialog.editMode ? 1 : 0

                // 0: Rendered Markdown View
                ScrollView {
                    clip: true
                    TextArea {
                        id: markdownPreview
                        readOnly: true
                        textFormat: TextEdit.MarkdownText
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        text: detailsArea.text.trim() !== ""
                            ? detailsArea.text
                            : qsTr("*No description yet — click **✎ Edit MD** or **📂 Open .md...** above.*")
                        onLinkActivated: function(link) { app.openUrlExternally(link) }
                    }
                }

                // 1: Raw Markdown Editor
                ScrollView {
                    clip: true
                    TextArea {
                        id: detailsArea
                        placeholderText: qsTr("Write task description or notes in Markdown (# Heading, **bold**, `code`, - list)...")
                        wrapMode: TextArea.Wrap
                        selectByMouse: true
                        font.family: "monospace"
                    }
                }
            }

            // Tab 2: Comments List + Add Comment Composer
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 8

                ListView {
                    id: commentsListView
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                clip: true
                spacing: 8
                model: detailsDialog.taskComments

                delegate: Frame {
                    required property var modelData
                    width: commentsListView.width
                    padding: 8

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 4

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: modelData.author || qsTr("Author")
                                font.bold: true
                                Layout.fillWidth: true
                            }
                            Label {
                                text: modelData.created || ""
                                font.pixelSize: 11
                                opacity: 0.7
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.MarkdownText
                            wrapMode: Text.WordWrap
                            text: modelData.body || ""
                            onLinkActivated: function(link) { app.openUrlExternally(link) }
                        }
                    }
                }

                    Label {
                        anchors.centerIn: parent
                        visible: !detailsDialog.taskComments || detailsDialog.taskComments.length === 0
                        text: qsTr("No comments yet — write a comment below.")
                        opacity: 0.6
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    TextArea {
                        id: newCommentArea
                        Layout.fillWidth: true
                        Layout.preferredHeight: 64
                        placeholderText: qsTr("Write a comment in Markdown (syncs to Jira / YouTrack)...")
                        wrapMode: TextArea.Wrap
                    }

                    Button {
                        text: qsTr("💬 Post Comment")
                        enabled: newCommentArea.text.trim() !== ""
                        onClicked: {
                            const body = newCommentArea.text.trim()
                            if (body === "" || (detailsDialog.taskId <= 0 && detailsDialog.taskRow < 0)) return
                            if (detailsDialog.taskId > 0 && typeof app !== "undefined" && app && app.addTaskCommentById) {
                                app.addTaskCommentById(detailsDialog.taskId, body)
                            } else {
                                app.addTaskComment(detailsDialog.fromDoneList, detailsDialog.taskRow, body)
                            }
                            let updated = detailsDialog.taskComments ? detailsDialog.taskComments.slice() : []
                            updated.push({
                                author: qsTr("You"),
                                created: Qt.formatDateTime(new Date(), "yyyy-MM-dd hh:mm"),
                                body: body
                            })
                            detailsDialog.taskComments = updated
                            newCommentArea.text = ""
                        }
                    }
                }
            }
        }
    }

    onAccepted: {
        if (taskId > 0 && typeof app !== "undefined" && app && app.updateTaskDetailsById) {
            app.updateTaskDetailsById(taskId, detailsArea.text)
        } else if (taskRow >= 0) {
            app.updateTaskDetails(fromDoneList, taskRow, detailsArea.text)
        }
    }
}
