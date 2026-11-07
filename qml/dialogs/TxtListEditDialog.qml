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
    property var currentPriorityMapping: ({})
    signal conflictDetected(var config, string conflictMessage)

    readonly property string selectedProvider: {
        if (providerCombo.currentIndex === 1) return "Jira"
        if (providerCombo.currentIndex === 2) return "YouTrack"
        if (providerCombo.currentIndex === 3) return "GitHub"
        if (providerCombo.currentIndex === 4) return "GitLab"
        if (providerCombo.currentIndex === 5) return "Redmine"
        if (providerCombo.currentIndex === 6) return "Todoist"
        if (providerCombo.currentIndex === 7) return "Nextcloud"
        return "SQLite"
    }

    readonly property var localStatusOptions: [
        { id: "todo", label: qsTr("To-Do") },
        { id: "in_progress", label: qsTr("In Progress") },
        { id: "paused", label: qsTr("Paused") },
        { id: "done", label: qsTr("Done") },
        { id: "canceled", label: qsTr("Canceled / Rejected") }
    ]

    readonly property var localPriorityOptions: [
        { id: "A", label: qsTr("(A) - Highest") },
        { id: "B", label: qsTr("(B) - High") },
        { id: "C", label: qsTr("(C) - Medium") },
        { id: "D", label: qsTr("(D) - Low") },
        { id: "E", label: qsTr("(E) - Lowest") },
        { id: "none", label: qsTr("None (No priority)") }
    ]

    ListModel {
        id: statusMappingModel
    }

    ListModel {
        id: priorityMappingModel
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

    function rebuildPriorityMappingModel(mappingObj) {
        priorityMappingModel.clear()
        currentPriorityMapping = mappingObj || ({})
        const keys = Object.keys(currentPriorityMapping)
        if (keys.length > 0) {
            for (let i = 0; i < keys.length; ++i) {
                priorityMappingModel.append({
                    remoteName: keys[i],
                    localPriority: currentPriorityMapping[keys[i]]
                })
            }
        } else if (selectedProvider !== "SQLite") {
            loadDefaultPriorities()
        }
    }

    function loadDefaultPriorities() {
        const defaults = app.issueTracker.defaultPrioritiesForProvider(selectedProvider, currentPriorityMapping || ({}))
        priorityMappingModel.clear()
        for (let i = 0; i < defaults.length; ++i) {
            priorityMappingModel.append({
                remoteName: defaults[i].remoteName,
                localPriority: defaults[i].localPriority
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

    function exportPriorityMappingObject() {
        let out = {}
        for (let i = 0; i < priorityMappingModel.count; ++i) {
            const item = priorityMappingModel.get(i)
            out[item.remoteName] = item.localPriority
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
        function onRemotePrioritiesLoaded(priorities) {
            priorityMappingModel.clear()
            for (let i = 0; i < priorities.length; ++i) {
                priorityMappingModel.append({
                    remoteName: priorities[i].remoteName,
                    localPriority: priorities[i].localPriority
                })
            }
            priorityFetchLabel.text = qsTr("Loaded %1 priorities.").arg(priorities.length)
        }
        function onRemotePrioritiesError(err) {
            priorityFetchLabel.text = err
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
        rebuildPriorityMappingModel({})
        statusFetchLabel.text = ""
        priorityFetchLabel.text = ""
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
        else if (cfg.provider === "Redmine") providerCombo.currentIndex = 5
        else if (cfg.provider === "Todoist") providerCombo.currentIndex = 6
        else if (cfg.provider === "Nextcloud") providerCombo.currentIndex = 7
        else providerCombo.currentIndex = 0

        nameField.text = cfg.name || ""
        jiraUrlField.text = cfg.jiraUrl || ""
        jiraUserField.text = cfg.jiraUser || ""
        apiTokenField.text = cfg.apiToken || ""
        jiraJqlField.text = cfg.jiraJql || ""
        autoWorklogCheck.checked = cfg.autoWorklog !== undefined ? cfg.autoWorklog : true
        rebuildStatusMappingModel(cfg.statusMapping || ({}))
        rebuildPriorityMappingModel(cfg.priorityMapping || ({}))
        statusFetchLabel.text = ""
        priorityFetchLabel.text = ""
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
            priorityMapping: exportPriorityMappingObject(),
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
            TabButton {
                text: qsTr("Priorities")
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
                            qsTr("GitLab (Issues & Merge Requests)"),
                            qsTr("Redmine (Issues)"),
                            qsTr("Todoist (Tasks)"),
                            qsTr("Nextcloud Tasks (CalDAV / VTODO)")
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
                            text: {
                                if (listEditDialog.selectedProvider === "GitHub") return qsTr("Server URL (leave empty for https://api.github.com):")
                                if (listEditDialog.selectedProvider === "GitLab") return qsTr("Server URL (leave empty for https://gitlab.com):")
                                if (listEditDialog.selectedProvider === "Todoist") return qsTr("Server URL (default: https://api.todoist.com):")
                                if (listEditDialog.selectedProvider === "Nextcloud") return qsTr("CalDAV URL (e.g. https://cloud.example.com/remote.php/dav/calendars/user/tasks/):")
                                return qsTr("Server URL:")
                            }
                        }
                        TextField {
                            id: jiraUrlField
                            Layout.fillWidth: true
                            placeholderText: {
                                if (listEditDialog.selectedProvider === "GitHub") return "https://api.github.com"
                                if (listEditDialog.selectedProvider === "GitLab") return "https://gitlab.com"
                                if (listEditDialog.selectedProvider === "Todoist") return "https://api.todoist.com"
                                if (listEditDialog.selectedProvider === "Nextcloud") return "https://cloud.example.com/remote.php/dav/calendars/username/tasks/"
                                if (listEditDialog.selectedProvider === "Redmine") return "https://redmine.example.com"
                                if (listEditDialog.selectedProvider === "Jira") return "https://your-domain.atlassian.net"
                                return "https://youtrack.example.com"
                            }
                        }

                        Label {
                            visible: listEditDialog.selectedProvider === "Jira" || listEditDialog.selectedProvider === "Redmine" || listEditDialog.selectedProvider === "Nextcloud"
                            text: {
                                if (listEditDialog.selectedProvider === "Redmine") return qsTr("Username (optional if using API key):")
                                if (listEditDialog.selectedProvider === "Nextcloud") return qsTr("Nextcloud Username:")
                                return qsTr("Email / Username (leave empty for Bearer PAT):")
                            }
                        }
                        TextField {
                            id: jiraUserField
                            visible: listEditDialog.selectedProvider === "Jira" || listEditDialog.selectedProvider === "Redmine" || listEditDialog.selectedProvider === "Nextcloud"
                            Layout.fillWidth: true
                            placeholderText: listEditDialog.selectedProvider === "Nextcloud" ? "username" : "user@example.com"
                        }

                        Label {
                            text: {
                                if (listEditDialog.selectedProvider === "GitHub") return qsTr("Personal Access Token (optional for public repos):")
                                if (listEditDialog.selectedProvider === "GitLab") return qsTr("Personal Access Token (glpat-...):")
                                if (listEditDialog.selectedProvider === "Redmine") return qsTr("API Access Key (from /my/account):")
                                if (listEditDialog.selectedProvider === "Todoist") return qsTr("Todoist API Token (Settings -> Integrations):")
                                if (listEditDialog.selectedProvider === "Nextcloud") return qsTr("App Password (Settings -> Security -> App passwords):")
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
                                if (listEditDialog.selectedProvider === "Redmine") return qsTr("Query Filter (e.g. assigned_to_id=me, project_id=123, or empty):")
                                if (listEditDialog.selectedProvider === "Todoist") return qsTr("Filter (e.g. filter=today | overdue or project_id=123):")
                                if (listEditDialog.selectedProvider === "Nextcloud") return qsTr("Calendar name / slug (default: tasks):")
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
                                if (listEditDialog.selectedProvider === "Redmine") return "assigned_to_id=me&status_id=open"
                                if (listEditDialog.selectedProvider === "Todoist") return "filter=today | overdue"
                                if (listEditDialog.selectedProvider === "Nextcloud") return "tasks"
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

            // Tab 3: Remote Priority Mapping (Jira / YouTrack / GitHub / GitLab / Redmine / Todoist / Nextcloud -> Local Priorities)
            ColumnLayout {
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Button {
                        text: qsTr("Load Default Priorities for ") + listEditDialog.selectedProvider
                        onClicked: {
                            listEditDialog.loadDefaultPriorities()
                            priorityFetchLabel.text = qsTr("Default %1 priorities loaded.").arg(listEditDialog.selectedProvider)
                        }
                    }
                    Button {
                        visible: listEditDialog.selectedProvider === "Jira" || listEditDialog.selectedProvider === "Redmine"
                        text: qsTr("Fetch from Server")
                        onClicked: {
                            priorityFetchLabel.text = qsTr("Loading priorities from server...")
                            app.issueTracker.fetchRemotePriorities(
                                listEditDialog.selectedProvider,
                                jiraUrlField.text.trim(),
                                jiraUserField.text.trim(),
                                apiTokenField.text.trim(),
                                exportPriorityMappingObject()
                            )
                        }
                    }
                    Label {
                        id: priorityFetchLabel
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
                    text: qsTr("Map remote priorities (%1) to local priorities ((A), (B), (C), (D), (E) or None). Each integration has its own independent priority mapping.").arg(listEditDialog.selectedProvider)
                }

                // Add Custom Priority row
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    TextField {
                        id: customPrioField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Add custom remote priority or label (e.g. 'P0', 'Urgent', 'Blocker')...")
                    }
                    ComboBox {
                        id: customPrioCombo
                        Layout.preferredWidth: 160
                        model: listEditDialog.localPriorityOptions
                        textRole: "label"
                        valueRole: "id"
                    }
                    Button {
                        text: qsTr("+ Add")
                        enabled: customPrioField.text.trim() !== ""
                        onClicked: {
                            const name = customPrioField.text.trim()
                            if (name !== "") {
                                priorityMappingModel.append({
                                    remoteName: name,
                                    localPriority: customPrioCombo.currentValue
                                })
                                customPrioField.text = ""
                            }
                        }
                    }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: priorityMappingModel
                    spacing: 4

                    delegate: RowLayout {
                        required property int index
                        required property string remoteName
                        required property string localPriority
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
                            model: listEditDialog.localPriorityOptions
                            textRole: "label"
                            valueRole: "id"
                            Component.onCompleted: {
                                for (let i = 0; i < listEditDialog.localPriorityOptions.length; ++i) {
                                    if (listEditDialog.localPriorityOptions[i].id === localPriority) {
                                        currentIndex = i
                                        break
                                    }
                                }
                            }
                            onActivated: function(idx) {
                                priorityMappingModel.setProperty(index, "localPriority", listEditDialog.localPriorityOptions[idx].id)
                            }
                        }

                        ToolButton {
                            text: "✕"
                            implicitWidth: 24
                            implicitHeight: 24
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Remove priority mapping")
                            onClicked: priorityMappingModel.remove(index)
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: priorityMappingModel.count === 0
                        text: qsTr("Click 'Load Default Priorities' above or add custom priority mappings.")
                        opacity: 0.6
                    }
                }
            }

            // Tab 4: Per-list Timer Settings
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
