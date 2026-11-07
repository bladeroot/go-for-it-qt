import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ScrollView {
    id: dashboardRoot
    contentWidth: availableWidth
    clip: true

    property string selectedPeriod: "day" // "day", "week", "month", "all"
    property var stats: app.analytics ? app.analytics.getProductivityStats(selectedPeriod, app.currentListId) : ({})
    property var heatmap: app.analytics ? app.analytics.getActivityHeatmap(91, app.currentListId) : []

    function reloadData() {
        if (!app.analytics) return
        stats = app.analytics.getProductivityStats(selectedPeriod, app.currentListId)
        heatmap = app.analytics.getActivityHeatmap(91, app.currentListId)
    }

    Component.onCompleted: reloadData()

    Connections {
        target: app.timer
        function onTimerStopped() { dashboardRoot.reloadData() }
    }

    FileDialog {
        id: saveCsvDialog
        title: qsTr("Export Billing Report to CSV")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("CSV Files (*.csv)"), qsTr("All files (*)")]
        defaultSuffix: "csv"
        onAccepted: {
            let path = selectedFile.toString()
            if (path.startsWith("file://")) path = path.substring(7)
            if (app.analytics.exportBillingCsv(path, dashboardRoot.selectedPeriod, app.currentListId)) {
                app.notifications.notifyTaskEvent(qsTr("Report Exported"), qsTr("CSV report saved successfully."))
            }
        }
    }

    FileDialog {
        id: savePdfDialog
        title: qsTr("Export Billing Report to PDF")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("PDF Files (*.pdf)"), qsTr("All files (*)")]
        defaultSuffix: "pdf"
        onAccepted: {
            let path = selectedFile.toString()
            if (path.startsWith("file://")) path = path.substring(7)
            if (app.analytics.exportBillingPdf(path, dashboardRoot.selectedPeriod, app.currentListId)) {
                app.notifications.notifyTaskEvent(qsTr("Report Exported"), qsTr("PDF report saved successfully."))
            }
        }
    }

    ColumnLayout {
        width: parent.width - 24
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 16

        Item { height: 4 }

        // Top Controls: Period Bar & Export Buttons
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            TabBar {
                id: periodTabBar
                currentIndex: 0
                onCurrentIndexChanged: {
                    if (currentIndex === 0) dashboardRoot.selectedPeriod = "day"
                    else if (currentIndex === 1) dashboardRoot.selectedPeriod = "week"
                    else if (currentIndex === 2) dashboardRoot.selectedPeriod = "month"
                    else dashboardRoot.selectedPeriod = "all"
                    dashboardRoot.reloadData()
                }

                TabButton { text: qsTr("Today") }
                TabButton { text: qsTr("This Week") }
                TabButton { text: qsTr("This Month") }
                TabButton { text: qsTr("All Time") }
            }

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("📊 Export CSV")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Export task billing summary to CSV file")
                onClicked: saveCsvDialog.open()
            }

            Button {
                text: qsTr("📄 Export PDF")
                highlighted: true
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Generate professional printable PDF billing report")
                onClicked: savePdfDialog.open()
            }
        }

        // KPI Summary Cards
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            // Card 1: Focus Time
            Pane {
                Layout.fillWidth: true
                Layout.preferredHeight: 90
                background: Rectangle {
                    color: app.settings.useDarkTheme ? "#2a2e32" : "#ffffff"
                    radius: 8
                    border.color: app.settings.useDarkTheme ? "#3e444a" : "#e0e0e0"
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 4
                    Label {
                        text: qsTr("⏱ TOTAL FOCUS TIME")
                        font.pixelSize: 10
                        font.bold: true
                        color: palette.highlight
                    }
                    Label {
                        text: dashboardRoot.stats.totalFormattedTime || "0m"
                        font.pixelSize: 22
                        font.bold: true
                    }
                    Label {
                        text: qsTr("%1 billable hours").arg(dashboardRoot.stats.totalHours ? dashboardRoot.stats.totalHours.toFixed(2) : "0.00")
                        font.pixelSize: 11
                        opacity: 0.7
                    }
                }
            }

            // Card 2: Completed Pomodoro Cycles
            Pane {
                Layout.fillWidth: true
                Layout.preferredHeight: 90
                background: Rectangle {
                    color: app.settings.useDarkTheme ? "#2a2e32" : "#ffffff"
                    radius: 8
                    border.color: app.settings.useDarkTheme ? "#3e444a" : "#e0e0e0"
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 4
                    Label {
                        text: qsTr("🍅 COMPLETED POMODOROS")
                        font.pixelSize: 10
                        font.bold: true
                        color: "#e74c3c"
                    }
                    Label {
                        text: qsTr("%1 cycles").arg(dashboardRoot.stats.completedPomodoros || 0)
                        font.pixelSize: 22
                        font.bold: true
                    }
                    Label {
                        text: qsTr("%1 work sessions logged").arg(dashboardRoot.stats.totalSessions || 0)
                        font.pixelSize: 11
                        opacity: 0.7
                    }
                }
            }

            // Card 3: Active Tasks
            Pane {
                Layout.fillWidth: true
                Layout.preferredHeight: 90
                background: Rectangle {
                    color: app.settings.useDarkTheme ? "#2a2e32" : "#ffffff"
                    radius: 8
                    border.color: app.settings.useDarkTheme ? "#3e444a" : "#e0e0e0"
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 4
                    Label {
                        text: qsTr("📋 ACTIVE TASKS WORKED")
                        font.pixelSize: 10
                        font.bold: true
                        color: "#27ae60"
                    }
                    Label {
                        text: qsTr("%1 tasks").arg(dashboardRoot.stats.activeTaskCount || 0)
                        font.pixelSize: 22
                        font.bold: true
                    }
                    Label {
                        text: qsTr("in %1").arg(app.currentFilterTitle)
                        font.pixelSize: 11
                        opacity: 0.7
                        elide: Text.ElideRight
                    }
                }
            }
        }

        // GitHub-Style Activity Heatmap Card
        Pane {
            Layout.fillWidth: true
            background: Rectangle {
                color: app.settings.useDarkTheme ? "#2a2e32" : "#ffffff"
                radius: 8
                border.color: app.settings.useDarkTheme ? "#3e444a" : "#e0e0e0"
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        text: qsTr("📅 Activity Heatmap (Last 90 Days)")
                        font.bold: true
                        font.pixelSize: 13
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: qsTr("Hover over squares to view daily focus")
                        font.pixelSize: 11
                        opacity: 0.6
                    }
                }

                ScrollView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 125
                    contentHeight: 120
                    clip: true

                    RowLayout {
                        spacing: 8

                        // Day of week labels (Mon, Wed, Fri)
                        ColumnLayout {
                            spacing: 4
                            Layout.alignment: Qt.AlignTop
                            Item { height: 2 }
                            Label { text: "Mon"; font.pixelSize: 9; opacity: 0.6 }
                            Item { height: 11 }
                            Label { text: "Wed"; font.pixelSize: 9; opacity: 0.6 }
                            Item { height: 11 }
                            Label { text: "Fri"; font.pixelSize: 9; opacity: 0.6 }
                        }

                        // Grid: 13 columns of 7 days
                        Row {
                            spacing: 3
                            Repeater {
                                model: Math.ceil(dashboardRoot.heatmap.length / 7)
                                Column {
                                    id: colRepeater
                                    property int colIndex: index
                                    spacing: 3

                                    Repeater {
                                        model: 7
                                        Rectangle {
                                            id: heatSq
                                            property int itemIdx: colRepeater.colIndex * 7 + index
                                            property var dayData: itemIdx < dashboardRoot.heatmap.length ? dashboardRoot.heatmap[itemIdx] : null

                                            width: 12
                                            height: 12
                                            radius: 2
                                            visible: dayData !== null

                                            color: {
                                                if (!dayData) return "transparent"
                                                const lvl = dayData.intensity || 0
                                                if (lvl === 0) return app.settings.useDarkTheme ? "#1b1e20" : "#ebedf0"
                                                if (app.settings.useDarkTheme) {
                                                    if (lvl === 1) return "#0e4429"
                                                    if (lvl === 2) return "#006d32"
                                                    if (lvl === 3) return "#26a641"
                                                    return "#39d353"
                                                } else {
                                                    if (lvl === 1) return "#9be9a8"
                                                    if (lvl === 2) return "#40c463"
                                                    if (lvl === 3) return "#30a14e"
                                                    return "#216e39"
                                                }
                                            }

                                            ToolTip.visible: sqMa.containsMouse && dayData !== null
                                            ToolTip.text: dayData ? (dayData.dayName + ", " + dayData.date + ": " + dayData.formattedTime + " (" + dayData.pomodoros + " " + qsTr("pomodoros") + ")") : ""

                                            MouseArea {
                                                id: sqMa
                                                anchors.fill: parent
                                                hoverEnabled: true
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Legend
                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: 4
                    Label { text: qsTr("Less"); font.pixelSize: 10; opacity: 0.6 }
                    Rectangle { width: 10; height: 10; radius: 2; color: app.settings.useDarkTheme ? "#1b1e20" : "#ebedf0" }
                    Rectangle { width: 10; height: 10; radius: 2; color: app.settings.useDarkTheme ? "#0e4429" : "#9be9a8" }
                    Rectangle { width: 10; height: 10; radius: 2; color: app.settings.useDarkTheme ? "#006d32" : "#40c463" }
                    Rectangle { width: 10; height: 10; radius: 2; color: app.settings.useDarkTheme ? "#26a641" : "#30a14e" }
                    Rectangle { width: 10; height: 10; radius: 2; color: app.settings.useDarkTheme ? "#39d353" : "#216e39" }
                    Label { text: qsTr("More"); font.pixelSize: 10; opacity: 0.6 }
                }
            }
        }

        // Projects & Contexts Breakdown (2 Columns)
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            // Projects Breakdown
            Pane {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                background: Rectangle {
                    color: app.settings.useDarkTheme ? "#2a2e32" : "#ffffff"
                    radius: 8
                    border.color: app.settings.useDarkTheme ? "#3e444a" : "#e0e0e0"
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    Label {
                        text: qsTr("📁 Projects (+Project)")
                        font.bold: true
                        font.pixelSize: 13
                    }

                    Label {
                        visible: !dashboardRoot.stats.projectBreakdown || dashboardRoot.stats.projectBreakdown.length === 0
                        text: qsTr("No project tags (+tag) recorded in this period.")
                        font.italic: true
                        opacity: 0.6
                    }

                    Repeater {
                        model: dashboardRoot.stats.projectBreakdown || []
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            required property var modelData

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: "+" + modelData.name
                                    font.bold: true
                                    font.pixelSize: 11
                                    color: "#e67e22"
                                }
                                Item { Layout.fillWidth: true }
                                Label {
                                    text: modelData.formattedTime + " (" + modelData.percentage.toFixed(1) + "%)"
                                    font.pixelSize: 11
                                    opacity: 0.75
                                }
                            }
                            ProgressBar {
                                Layout.fillWidth: true
                                value: modelData.percentage / 100.0
                            }
                        }
                    }
                }
            }

            // Contexts Breakdown
            Pane {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                background: Rectangle {
                    color: app.settings.useDarkTheme ? "#2a2e32" : "#ffffff"
                    radius: 8
                    border.color: app.settings.useDarkTheme ? "#3e444a" : "#e0e0e0"
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    Label {
                        text: qsTr("🏷 Contexts (@Context)")
                        font.bold: true
                        font.pixelSize: 13
                    }

                    Label {
                        visible: !dashboardRoot.stats.contextBreakdown || dashboardRoot.stats.contextBreakdown.length === 0
                        text: qsTr("No context tags (@tag) recorded in this period.")
                        font.italic: true
                        opacity: 0.6
                    }

                    Repeater {
                        model: dashboardRoot.stats.contextBreakdown || []
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            required property var modelData

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: "@" + modelData.name
                                    font.bold: true
                                    font.pixelSize: 11
                                    color: "#3498db"
                                }
                                Item { Layout.fillWidth: true }
                                Label {
                                    text: modelData.formattedTime + " (" + modelData.percentage.toFixed(1) + "%)"
                                    font.pixelSize: 11
                                    opacity: 0.75
                                }
                            }
                            ProgressBar {
                                Layout.fillWidth: true
                                value: modelData.percentage / 100.0
                            }
                        }
                    }
                }
            }
        }

        // Task Billing Detailed List
        Pane {
            Layout.fillWidth: true
            background: Rectangle {
                color: app.settings.useDarkTheme ? "#2a2e32" : "#ffffff"
                radius: 8
                border.color: app.settings.useDarkTheme ? "#3e444a" : "#e0e0e0"
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: 8

                Label {
                    text: qsTr("💼 Task Time & Billing Log")
                    font.bold: true
                    font.pixelSize: 13
                }

                Label {
                    visible: !dashboardRoot.stats.taskBreakdown || dashboardRoot.stats.taskBreakdown.length === 0
                    text: qsTr("No tasks tracked during this period. Run the timer to log activity!")
                    font.italic: true
                    opacity: 0.6
                }

                Repeater {
                    model: dashboardRoot.stats.taskBreakdown || []
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: taskCol.implicitHeight + 12
                        radius: 6
                        color: app.settings.useDarkTheme ? "#232629" : "#f8f9fa"
                        border.color: app.settings.useDarkTheme ? "#31363b" : "#e9ecef"
                        required property var modelData

                        ColumnLayout {
                            id: taskCol
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 4

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6

                                Rectangle {
                                    visible: modelData.jiraKey !== ""
                                    implicitHeight: 18
                                    implicitWidth: keyLbl.implicitWidth + 8
                                    radius: 3
                                    color: "#5c6b73"
                                    Label {
                                        id: keyLbl
                                        anchors.centerIn: parent
                                        text: modelData.jiraKey
                                        font.pixelSize: 9
                                        font.bold: true
                                        color: "#ffffff"
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.description
                                    font.bold: true
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }

                                Label {
                                    text: modelData.formattedTime
                                    font.bold: true
                                    font.pixelSize: 12
                                    color: palette.highlight
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    text: qsTr("List: %1").arg(modelData.listName)
                                    font.pixelSize: 10
                                    opacity: 0.6
                                }
                                Item { Layout.fillWidth: true }
                                Label {
                                    text: qsTr("%1 sessions (%2h)").arg(modelData.sessions).arg(modelData.hours.toFixed(2))
                                    font.pixelSize: 10
                                    opacity: 0.7
                                }
                            }
                        }
                    }
                }
            }
        }

        Item { height: 16 }
    }
}
