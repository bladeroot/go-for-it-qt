import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

Dialog {
    id: listEditDialog
    title: editingId === "" ? qsTr("New to-do list") : qsTr("Edit to-do list properties")
    anchors.centerIn: parent
    width: 480
    height: 510
    modal: true
    standardButtons: Dialog.Ok | Dialog.Cancel

    property string editingId: ""
    property var currentStatusMapping: ({})
    signal conflictDetected(var config, string conflictMessage)

    readonly property string selectedProvider: {
        if (providerCombo.currentIndex === 1) return "Jira"
        if (providerCombo.currentIndex === 2) return "YouTrack"
        if (providerCombo.currentIndex === 3) return "GitHub"
        if (providerCombo.currentIndex === 4) return "GitLab"
        return "SQLite"
    }

    readonly property var localStatusOptions: [
        { id: "todo", label: qsTr("To-Do") },
        { id: "in_progress", label: qsTr("In Progress") },
        { id: "paused", label: qsTr("Paused") },
        { id: "done", label: qsTr("Done") },
        { id: "canceled", label: qsTr("Canceled / Rejected") }
    ]

    ListModel {
        id: statusMappingModel
    }

    function rebuildStatusMappingModel(mappingObj) {
        statusMappingModel.clear()
        currentStatusMapping = mappingObj || ({})
        const keys = Object.keys(currentStatusMapping)
        for (let i = 0; i < keys.length; ++i) {
            statusMappingModel.append({
                remoteName: keys[i],
                localStatus: currentStatusMapping[keys[i]]
            })
        }
    }

    function exportStatusMappingObject() {
        let out = {}
        for (let i = 0; i < statusMappingModel.count; ++i) {
            const item = statusMappingModel.get(i)
            out[item.remoteName] = item.localStatus
        }
        return out
    }

    Connections {
        target: app.issueTracker
        function onRemoteStatusesLoaded(statuses) {
            statusMappingModel.clear()
            for (let i = 0; i < statuses.length; ++i) {
                statusMappingModel.append({
                    remoteName: statuses[i].remoteName,
                    localStatus: statuses[i].localStatus
                })
            }
            statusFetchLabel.text = qsTr("Loaded %1 statuses.").arg(statuses.length)
        }
        function onRemoteStatusesError(err) {
            statusFetchLabel.text = err
        }
    }

    function openForNew() {
        editingId = ""
        editTabBar.currentIndex = 0
        providerCombo.currentIndex = 0
        nameField.text = ""
        jiraUrlField.text = ""
        jiraUserField.text = ""
        apiTokenField.text = ""
        jiraJqlField.text = ""
        autoWorklogCheck.checked = true
        rebuildStatusMappingModel({})
        statusFetchLabel.text = ""
        logUsageCheck.checked = false
        logFilePathField.text = ""
        addCreationDatesCheck.checked = true
        useDefaultTimerCheck.checked = true
        reminderSpin.value = app.settings.reminderTime
        listScheduleWidget.loadSchedule(app.settings.customSchedule)
        errorLabel.text = ""
        open()
    }

    function openForEdit(listId) {
        editingId = listId
        editTabBar.currentIndex = 0
        const cfg = app.listManager.getListConfig(listId)
        if (cfg.provider === "Jira") providerCombo.currentIndex = 1
        else if (cfg.provider === "YouTrack") providerCombo.currentIndex = 2
        else if (cfg.provider === "GitHub") providerCombo.currentIndex = 3
        else if (cfg.provider === "GitLab") providerCombo.currentIndex = 4
        else providerCombo.currentIndex = 0

        nameField.text = cfg.name || ""
        jiraUrlField.text = cfg.jiraUrl || ""
        jiraUserField.text = cfg.jiraUser || ""
        apiTokenField.text = cfg.apiToken || ""
        jiraJqlField.text = cfg.jiraJql || ""
        autoWorklogCheck.checked = cfg.autoWorklog !== undefined ? cfg.autoWorklog : true
        rebuildStatusMappingModel(cfg.statusMapping || ({}))
        statusFetchLabel.text = ""
        logUsageCheck.checked = (cfg.activityLogPath && cfg.activityLogPath !== "")
        logFilePathField.text = cfg.activityLogPath || ""
        addCreationDatesCheck.checked = cfg.addCreationDates !== undefined ? cfg.addCreationDates : true

        const hasCustomSched = cfg.customSchedule && cfg.customSchedule.length >= 2
        useDefaultTimerCheck.checked = (cfg.reminderTime < 0 && !hasCustomSched)
        reminderSpin.value = cfg.reminderTime >= 0 ? cfg.reminderTime : app.settings.reminderTime
        listScheduleWidget.loadSchedule(hasCustomSched ? cfg.customSchedule : app.settings.customSchedule)
        errorLabel.text = ""
        open()
    }

    function buildConfig() {
        return {
            id: editingId,
            name: nameField.text.trim(),
            provider: listEditDialog.selectedProvider,
            jiraUrl: jiraUrlField.text.trim(),
            jiraUser: jiraUserField.text.trim(),
            apiToken: apiTokenField.text.trim(),
            jiraJql: jiraJqlField.text.trim(),
            autoWorklog: autoWorklogCheck.checked,
            statusMapping: exportStatusMappingObject(),
            activityLogPath: logUsageCheck.checked ? logFilePathField.text.trim() : "",
            addCreationDates: addCreationDatesCheck.checked,
            reminderTime: useDefaultTimerCheck.checked ? -1 : reminderSpin.value,
            customSchedule: useDefaultTimerCheck.checked ? [] : listScheduleWidget.exportRawSeconds()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        TabBar {
            id: editTabBar
            Layout.fillWidth: true
            TabButton { text: qsTr("General & Sync") }
            TabButton {
                text: qsTr("Statuses")
                enabled: listEditDialog.selectedProvider !== "SQLite"
            }
            TabButton { text: qsTr("Timer") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: editTabBar.currentIndex

            // Tab 1: General & Issue Tracker Integration
            ScrollView {
                clip: true
                ColumnLayout {
                    width: listEditDialog.availableWidth - 20
                    spacing: 8

                    Label { text: qsTr("List type / Provider:"); font.bold: true }
                    ComboBox {
                        id: providerCombo
                        Layout.fillWidth: true
                        model: [
                            qsTr("Local List (SQLite)"),
                            qsTr("Jira (Cloud / Data Center)"),
                            qsTr("JetBrains YouTrack"),
                            qsTr("GitHub (Issues & Pull Requests)"),
                            qsTr("GitLab (Issues & Merge Requests)")
                        ]
                    }

                    Label { text: qsTr("List name:"); font.bold: true }
                    TextField {
                        id: nameField
                        Layout.fillWidth: true
                        placeholderText: qsTr("e.g. Work / Sprint Tasks")
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: listEditDialog.selectedProvider !== "SQLite"

                        MenuSeparator { Layout.fillWidth: true }
                        Label {
                            text: listEditDialog.selectedProvider + " " + qsTr("Connection")
                            font.bold: true
                        }

                        Label {
                            text: listEditDialog.selectedProvider === "GitHub"
                                ? qsTr("Server URL (leave empty for https://api.github.com):")
                                : (listEditDialog.selectedProvider === "GitLab"
                                    ? qsTr("Server URL (leave empty for https://gitlab.com):")
                                    : qsTr("Server URL:"))
                        }
                        TextField {
                            id: jiraUrlField
                            Layout.fillWidth: true
                            placeholderText: {
                                if (listEditDialog.selectedProvider === "GitHub") return "https://api.github.com"
                                if (listEditDialog.selectedProvider === "GitLab") return "https://gitlab.com"
                                if (listEditDialog.selectedProvider === "Jira") return "https://your-domain.atlassian.net"
                                return "https://youtrack.example.com"
                            }
                        }

                        Label {
                            visible: listEditDialog.selectedProvider === "Jira"
                            text: qsTr("Email / Username (leave empty for Bearer PAT):")
                        }
                        TextField {
                            id: jiraUserField
                            visible: listEditDialog.selectedProvider === "Jira"
                            Layout.fillWidth: true
                            placeholderText: "user@example.com"
                        }

                        Label {
                            text: {
                                if (listEditDialog.selectedProvider === "GitHub") return qsTr("Personal Access Token (optional for public repos):")
                                if (listEditDialog.selectedProvider === "GitLab") return qsTr("Personal Access Token (glpat-...):")
                                if (listEditDialog.selectedProvider === "Jira") return qsTr("API Token / Personal Access Token:")
                                return qsTr("Permanent Token (perm:...):")
                            }
                        }
                        TextField {
                            id: apiTokenField
                            Layout.fillWidth: true
                            echoMode: TextInput.Password
                            placeholderText: "••••••••••••"
                        }

                        Label {
                            text: {
                                if (listEditDialog.selectedProvider === "GitHub") return qsTr("Search Query or repo (e.g. owner/repo, default: is:open involves:@me):")
                                if (listEditDialog.selectedProvider === "GitLab") return qsTr("Project Path or ID (e.g. group/project, or empty for all):")
                                if (listEditDialog.selectedProvider === "Jira") return qsTr("JQL Filter (optional):")
                                return qsTr("YouTrack Search Query (optional):")
                            }
                        }
                        TextField {
                            id: jiraJqlField
                            Layout.fillWidth: true
                            placeholderText: {
                                if (listEditDialog.selectedProvider === "GitHub") return "owner/repo is:open"
                                if (listEditDialog.selectedProvider === "GitLab") return "group/project"
                                if (listEditDialog.selectedProvider === "Jira") return "assignee = currentUser() ORDER BY updated DESC"
                                return "for: me sort by: updated desc"
                            }
                        }

                        CheckBox {
                            id: autoWorklogCheck
                            text: qsTr("Automatically log Pomodoro work time (Worklog)")
                            checked: true
                        }
                    }

                    MenuSeparator { Layout.fillWidth: true }

                    CheckBox {
                        id: addCreationDatesCheck
                        text: qsTr("Record creation date of tasks")
                        checked: true
                    }

                    CheckBox {
                        id: logUsageCheck
                        text: qsTr("Also export timer usage to external CSV file")
                    }

                    TextField {
                        id: logFilePathField
                        visible: logUsageCheck.checked
                        Layout.fillWidth: true
                        placeholderText: "/path/to/timer_log.csv"
                    }

                    Label {
                        id: errorLabel
                        visible: text !== ""
                        color: "#c0392b"
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }

            // Tab 2: Remote Status Mapping (Jira / YouTrack -> Local Statuses)
            ColumnLayout {
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        text: qsTr("Load Statuses from ") + listEditDialog.selectedProvider
                        onClicked: {
                            statusFetchLabel.text = qsTr("Loading statuses...")
                            app.issueTracker.fetchRemoteStatuses(
                                listEditDialog.selectedProvider,
                                jiraUrlField.text.trim(),
                                jiraUserField.text.trim(),
                                apiTokenField.text.trim(),
                                exportStatusMappingObject()
                            )
                        }
                    }
                    Label {
                        id: statusFetchLabel
                        Layout.fillWidth: true
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        opacity: 0.8
                    }
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    opacity: 0.75
                    text: qsTr("Map remote statuses to local views (To-Do, In Progress, Paused, Done, Canceled / Rejected). Synced issues automatically land in the mapped status view.")
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: statusMappingModel
                    spacing: 4

                    delegate: RowLayout {
                        required property int index
                        required property string remoteName
                        required property string localStatus
                        width: ListView.view.width
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: remoteName
                            font.bold: true
                            elide: Text.ElideRight
                        }

                        Label { text: "→"; opacity: 0.6 }

                        ComboBox {
                            Layout.preferredWidth: 180
                            model: listEditDialog.localStatusOptions
                            textRole: "label"
                            valueRole: "id"
                            Component.onCompleted: {
                                for (let i = 0; i < listEditDialog.localStatusOptions.length; ++i) {
                                    if (listEditDialog.localStatusOptions[i].id === localStatus) {
                                        currentIndex = i
                                        break
                                    }
                                }
                            }
                            onActivated: function(idx) {
                                statusMappingModel.setProperty(index, "localStatus", listEditDialog.localStatusOptions[idx].id)
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: statusMappingModel.count === 0
                        text: qsTr("Click 'Load Statuses' above to fetch and customize status mappings.")
                        opacity: 0.6
                    }
                }
            }

            // Tab 3: Per-list Timer Settings
            ColumnLayout {
                spacing: 10

                CheckBox {
                    id: useDefaultTimerCheck
                    text: qsTr("Use global default timer settings")
                    checked: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    enabled: !useDefaultTimerCheck.checked
                    Label { text: qsTr("Reminder before task ends (seconds):"); Layout.fillWidth: true }
                    SpinBox {
                        id: reminderSpin
                        from: 0; to: 600
                        value: 60
                    }
                }

                TimerScheduleWidget {
                    id: listScheduleWidget
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    enabled: !useDefaultTimerCheck.checked
                }
            }
        }
    }

    onAccepted: {
        const cfg = buildConfig()
        const validation = app.listManager.validateAndCheckConflicts(cfg)
        if (!validation.valid) {
            errorLabel.text = validation.error
            open()
            return
        }
        const savedId = app.listManager.saveListConfig(cfg, "keep")
        app.selectListById(savedId)
    }
}
