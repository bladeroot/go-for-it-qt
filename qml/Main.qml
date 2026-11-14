import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "pages"
import "dialogs"
import "components"

ApplicationWindow {
    id: rootWindow
    width: app.settings.windowWidth
    height: app.settings.windowHeight
    minimumWidth: 320
    minimumHeight: 420
    visible: !app.settings.startInTray
    title: app.showingOverview ? qsTr("Go for It Qt — Lists") : (app.currentFilterTitle + " — Go for It Qt")

    property int currentActivityTab: 0 // 0: To-Do, 1: Timer, 2: Done
    property bool showFilterBar: false
    property bool forceQuit: false

    palette.window: app.settings.useDarkTheme ? "#232629" : "#eff0f1"
    palette.windowText: app.settings.useDarkTheme ? "#eff0f1" : "#232629"
    palette.base: app.settings.useDarkTheme ? "#1b1e20" : "#ffffff"
    palette.alternateBase: app.settings.useDarkTheme ? "#2a2e32" : "#f7f7f7"
    palette.text: app.settings.useDarkTheme ? "#eff0f1" : "#232629"
    palette.button: app.settings.useDarkTheme ? "#31363b" : "#fcfcfc"
    palette.buttonText: app.settings.useDarkTheme ? "#eff0f1" : "#232629"
    palette.highlight: "#3daee9"
    palette.highlightedText: "#ffffff"

    function persistGeometry() {
        app.settings.saveWindowGeometry(rootWindow.width, rootWindow.height, rootWindow.x, rootWindow.y)
    }

    function toggleMiniPlayer() {
        if (miniPlayerWindow.visible) {
            miniPlayerWindow.hide()
        } else {
            miniPlayerWindow.show()
            miniPlayerWindow.raise()
            miniPlayerWindow.requestActivate()
        }
    }

    onWidthChanged: if (visible) app.settings.windowWidth = width
    onHeightChanged: if (visible) app.settings.windowHeight = height

    Component.onCompleted: {
        if (app.settings.windowX >= 0 && app.settings.windowY >= 0) {
            rootWindow.x = app.settings.windowX
            rootWindow.y = app.settings.windowY
        }
        if (app.settings.startInTray) {
            rootWindow.hide()
        } else if (app.settings.startMinimized) {
            rootWindow.showMinimized()
        }
        if (typeof showAboutOnStart !== "undefined" && showAboutOnStart) {
            aboutDialog.open()
        }
    }

    Component.onDestruction: {
        rootWindow.persistGeometry()
    }

    onClosing: function(close) {
        rootWindow.persistGeometry()
        // A running timer or an open mini player must survive closing the main window.
        if (!rootWindow.forceQuit && (app.timer.running || miniPlayerWindow.visible)) {
            close.accepted = false
            rootWindow.hide()
        }
    }

    Connections {
        target: app
        function onShowWindowRequested() {
            rootWindow.show()
            rootWindow.raise()
            rootWindow.requestActivate()
        }
        function onToggleMiniPlayerRequested() {
            rootWindow.toggleMiniPlayer()
        }
        function onQuitRequested() {
            rootWindow.persistGeometry()
            rootWindow.forceQuit = true
            app.timer.stop()
            Qt.quit()
        }
    }

    // Dynamic Configurable Shortcuts
    Shortcut {
        sequence: "Ctrl+M"
        onActivated: rootWindow.toggleMiniPlayer()
    }
    Shortcut {
        id: filterShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["filter"] !== undefined) ? app.shortcuts.shortcutMap["filter"] : app.shortcuts.shortcutFor("filter")
        onActivated: rootWindow.showFilterBar = !rootWindow.showFilterBar
    }
    Shortcut {
        id: sortShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["sort"] !== undefined) ? app.shortcuts.shortcutMap["sort"] : app.shortcuts.shortcutFor("sort")
        onActivated: app.sortAllTasks()
    }
    Shortcut {
        id: addNewShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["add-new"] !== undefined) ? app.shortcuts.shortcutMap["add-new"] : app.shortcuts.shortcutFor("add-new")
        onActivated: {
            if (app.showingOverview) {
                selectionPage.openNewListDialog()
            } else {
                rootWindow.currentActivityTab = 0
                taskListPage.focusNewTaskInput()
            }
        }
    }
    Shortcut {
        id: toggleTimerShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["toggle-timer"] !== undefined) ? app.shortcuts.shortcutMap["toggle-timer"] : app.shortcuts.shortcutFor("toggle-timer")
        onActivated: app.timer.toggleRunning()
    }
    Shortcut {
        id: skipShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["skip"] !== undefined) ? app.shortcuts.shortcutMap["skip"] : app.shortcuts.shortcutFor("skip")
        onActivated: app.timer.skipIteration()
    }
    Shortcut {
        id: markTaskDoneShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["mark-task-done"] !== undefined) ? app.shortcuts.shortcutMap["mark-task-done"] : app.shortcuts.shortcutFor("mark-task-done")
        onActivated: app.markActiveTaskDone()
    }
    Shortcut {
        id: nextTaskShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["next-task"] !== undefined) ? app.shortcuts.shortcutMap["next-task"] : app.shortcuts.shortcutFor("next-task")
        onActivated: app.switchToNextTask()
    }
    Shortcut {
        id: prevTaskShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["prev-task"] !== undefined) ? app.shortcuts.shortcutMap["prev-task"] : app.shortcuts.shortcutFor("prev-task")
        onActivated: app.switchToPreviousTask()
    }
    Shortcut {
        id: moveRowUpShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["move-row-up"] !== undefined) ? app.shortcuts.shortcutMap["move-row-up"] : app.shortcuts.shortcutFor("move-row-up")
        onActivated: {
            if (!app.showingOverview && rootWindow.currentActivityTab === 0) {
                app.moveActiveTaskByOffset(-1)
            }
        }
    }
    Shortcut {
        id: moveRowDownShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["move-row-down"] !== undefined) ? app.shortcuts.shortcutMap["move-row-down"] : app.shortcuts.shortcutFor("move-row-down")
        onActivated: {
            if (!app.showingOverview && rootWindow.currentActivityTab === 0) {
                app.moveActiveTaskByOffset(1)
            }
        }
    }
    Shortcut {
        id: cyclePageShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["cycle-page"] !== undefined) ? app.shortcuts.shortcutMap["cycle-page"] : app.shortcuts.shortcutFor("cycle-page")
        onActivated: {
            if (app.showingOverview) {
                app.showingOverview = false
            } else if (rootWindow.currentActivityTab < 2) {
                rootWindow.currentActivityTab += 1
            }
        }
    }
    Shortcut {
        id: cyclePageReverseShortcut
        sequence: (app.shortcuts.shortcutMap && app.shortcuts.shortcutMap["cycle-page-reverse"] !== undefined) ? app.shortcuts.shortcutMap["cycle-page-reverse"] : app.shortcuts.shortcutFor("cycle-page-reverse")
        onActivated: {
            if (!app.showingOverview) {
                if (rootWindow.currentActivityTab > 0) {
                    rootWindow.currentActivityTab -= 1
                } else {
                    app.showingOverview = true
                }
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+Q"
        onActivated: {
            rootWindow.persistGeometry()
            rootWindow.forceQuit = true
            app.timer.stop()
            Qt.quit()
        }
    }

    Connections {
        target: app.shortcuts
        function onShortcutsChanged() {
            filterShortcut.sequence = app.shortcuts.shortcutFor("filter")
            sortShortcut.sequence = app.shortcuts.shortcutFor("sort")
            addNewShortcut.sequence = app.shortcuts.shortcutFor("add-new")
            toggleTimerShortcut.sequence = app.shortcuts.shortcutFor("toggle-timer")
            skipShortcut.sequence = app.shortcuts.shortcutFor("skip")
            markTaskDoneShortcut.sequence = app.shortcuts.shortcutFor("mark-task-done")
            nextTaskShortcut.sequence = app.shortcuts.shortcutFor("next-task")
            prevTaskShortcut.sequence = app.shortcuts.shortcutFor("prev-task")
            moveRowUpShortcut.sequence = app.shortcuts.shortcutFor("move-row-up")
            moveRowDownShortcut.sequence = app.shortcuts.shortcutFor("move-row-down")
            cyclePageShortcut.sequence = app.shortcuts.shortcutFor("cycle-page")
            cyclePageReverseShortcut.sequence = app.shortcuts.shortcutFor("cycle-page-reverse")
        }
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            spacing: 8

            ToolButton {
                id: overviewSwitchBtn
                text: app.showingOverview ? "→" : "←"
                font.pixelSize: app.settings.useSmallToolbarIcons ? 13 : 16
                ToolTip.visible: hovered
                ToolTip.text: app.showingOverview
                    ? qsTr("Go back to the to-do list")
                    : qsTr("Go to overview")
                enabled: app.currentListId !== ""
                onClicked: app.showingOverview = !app.showingOverview
            }

            Item { Layout.fillWidth: true }

            Label {
                visible: app.showingOverview
                text: qsTr("Lists")
                font.bold: true
                font.pixelSize: 16
            }

            TabBar {
                id: viewSwitcher
                visible: !app.showingOverview
                currentIndex: rootWindow.currentActivityTab
                onCurrentIndexChanged: rootWindow.currentActivityTab = currentIndex

                TabButton {
                    text: {
                        var base = app.settings.switcherUseIcons ? "📋" : qsTr("To-Do")
                        if (app.totalTodoCount > 0) {
                            if (app.unviewedTodoCount > 0) {
                                return base + " (" + app.unviewedTodoCount + "/" + app.totalTodoCount + ")"
                            }
                            return base + " (" + app.totalTodoCount + ")"
                        }
                        return base
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: app.unviewedTodoCount > 0
                        ? (qsTr("To-Do") + " (" + app.unviewedTodoCount + " " + qsTr("unread") + " / " + app.totalTodoCount + " " + qsTr("total") + ")")
                        : (app.totalTodoCount > 0 ? (qsTr("To-Do") + " (" + app.totalTodoCount + ")") : qsTr("To-Do"))
                    width: implicitWidth + 16
                }
                TabButton {
                    text: app.settings.switcherUseIcons ? "⏱" : qsTr("Timer")
                    ToolTip.visible: hovered && app.settings.switcherUseIcons
                    ToolTip.text: qsTr("Timer")
                    width: implicitWidth + 16
                }
                TabButton {
                    text: app.settings.switcherUseIcons ? "✓" : qsTr("Done")
                    ToolTip.visible: hovered && app.settings.switcherUseIcons
                    ToolTip.text: qsTr("Done")
                    width: implicitWidth + 16
                }
                TabButton {
                    text: app.settings.switcherUseIcons ? "📊" : qsTr("Reports")
                    ToolTip.visible: hovered && app.settings.switcherUseIcons
                    ToolTip.text: qsTr("Productivity Dashboard & Reports")
                    width: implicitWidth + 16
                }
            }

            Item { Layout.fillWidth: true }

            ToolButton {
                visible: !app.showingOverview && app.isCurrentListRemote
                enabled: !app.issueTracker.syncing
                text: app.issueTracker.syncing ? "⏳" : "↻"
                font.pixelSize: app.settings.useSmallToolbarIcons ? 13 : 16
                ToolTip.visible: hovered
                ToolTip.text: app.issueTracker.lastStatusMessage !== ""
                    ? (qsTr("Sync issues (%1)").arg(app.issueTracker.lastStatusMessage))
                    : qsTr("Sync remote issues")
                onClicked: app.syncCurrentList()
            }

            ToolButton {
                visible: !app.showingOverview
                text: "🗗"
                font.pixelSize: app.settings.useSmallToolbarIcons ? 13 : 16
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Always on Top Mini-Player (Ctrl+M)")
                onClicked: rootWindow.toggleMiniPlayer()
            }

            ToolButton {
                text: "☰"
                font.pixelSize: app.settings.useSmallToolbarIcons ? 13 : 16
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Menu")
                onClicked: mainMenu.open()

                Menu {
                    id: mainMenu
                    y: parent.height

                    Menu {
                        title: "📂 " + qsTr("Switch View / Filter")
                        MenuItem {
                            text: "🌐 " + qsTr("All Lists (Unified)")
                            onTriggered: app.showAllLists()
                        }
                        MenuItem {
                            text: "💻 " + qsTr("Local Lists Only")
                            onTriggered: app.showProviderLists("SQLite")
                        }
                        MenuItem {
                            text: "☁️ " + qsTr("Remote Trackers Only")
                            onTriggered: {
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
                        MenuSeparator {}
                        Repeater {
                            model: app.listManager
                            MenuItem {
                                required property string listId
                                required property string name
                                required property string provider
                                text: name + " (" + provider + ")"
                                onTriggered: app.selectListById(listId)
                            }
                        }
                    }

                    MenuSeparator {}

                    MenuItem {
                        contentItem: RowLayout {
                            spacing: 4
                            Button {
                                Layout.fillWidth: true
                                text: "🔍 " + (rootWindow.showFilterBar ? qsTr("Hide Filter") : qsTr("Filter"))
                                enabled: !app.showingOverview && rootWindow.currentActivityTab !== 1
                                onClicked: {
                                    rootWindow.showFilterBar = !rootWindow.showFilterBar
                                    mainMenu.close()
                                }
                            }
                            Button {
                                Layout.fillWidth: true
                                text: app.settings.muteSounds ? "🔇 " + qsTr("Unmute") : "🔊 " + qsTr("Mute")
                                onClicked: app.settings.muteSounds = !app.settings.muteSounds
                            }
                        }
                    }

                    MenuSeparator {
                        visible: !app.showingOverview && rootWindow.currentActivityTab !== 1
                    }
                    MenuItem {
                        visible: !app.showingOverview && rootWindow.currentActivityTab !== 1
                        text: qsTr("Sort Tasks")
                        onTriggered: app.sortAllTasks()
                    }
                    MenuItem {
                        visible: !app.showingOverview && rootWindow.currentActivityTab !== 1
                        text: qsTr("Clear Done List")
                        onTriggered: app.clearDoneTasks()
                    }
                    MenuItem {
                        text: "🗗 " + qsTr("Always on Top Mini-Player") + " (Ctrl+M)"
                        onTriggered: rootWindow.toggleMiniPlayer()
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: qsTr("📊 Productivity Dashboard & Reports")
                        onTriggered: {
                            app.showingOverview = false
                            rootWindow.currentActivityTab = 3
                        }
                    }
                    MenuItem {
                        text: qsTr("Settings")
                        onTriggered: settingsDialog.open()
                    }
                    MenuItem {
                        text: qsTr("Contribute / Donate")
                        onTriggered: contributeDialog.open()
                    }
                    MenuItem {
                        text: qsTr("About")
                        onTriggered: aboutDialog.open()
                    }
                }
            }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: app.showingOverview ? 0 : 1

        SelectionPage {
            id: selectionPage
        }

        TaskListPage {
            id: taskListPage
            currentTab: rootWindow.currentActivityTab
            filterVisible: rootWindow.showFilterBar
            onCurrentTabRequested: function(tabIndex) {
                rootWindow.currentActivityTab = tabIndex
            }
        }
    }

    SettingsDialog {
        id: settingsDialog
    }

    ContributeDialog {
        id: contributeDialog
    }

    AboutDialog {
        id: aboutDialog
    }

    MiniPlayerWindow {
        id: miniPlayerWindow
        onVisibleChanged: {
            if (typeof app !== "undefined" && app && app.setMiniPlayerVisible) {
                app.setMiniPlayerVisible(miniPlayerWindow.visible)
            }
        }
    }
}
