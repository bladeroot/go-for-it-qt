import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

Dialog {
    id: settingsDialogRoot
    title: qsTr("Settings")
    anchors.centerIn: parent
    width: 480
    height: 460
    modal: true
    standardButtons: Dialog.Close

    property string capturingShortcutId: ""

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        TabBar {
            id: settingsTabBar
            Layout.fillWidth: true

            TabButton { text: qsTr("Behavior") }
            TabButton { text: qsTr("Notifications") }
            TabButton { text: qsTr("Appearance") }
            TabButton { text: qsTr("Shortcuts") }
            TabButton { text: qsTr("Plugins") }
            TabButton { text: qsTr("Storage") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: settingsTabBar.currentIndex

            // 1. Behavior Page
            ScrollView {
                clip: true
                ColumnLayout {
                    width: settingsDialogRoot.availableWidth - 20
                    spacing: 12

                    Label { text: qsTr("Tasks"); font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Placement of new tasks:"); Layout.fillWidth: true }
                        ComboBox {
                            model: [qsTr("Bottom of the list"), qsTr("Top of the list")]
                            currentIndex: app.settings.newTasksOnTop ? 1 : 0
                            onActivated: function(idx) { app.settings.newTasksOnTop = (idx === 1) }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Auto-sync Jira / YouTrack (minutes, 0 = off):"); Layout.fillWidth: true }
                        SpinBox {
                            from: 0; to: 1440
                            value: app.settings.autoSyncIntervalMinutes
                            onValueModified: app.settings.autoSyncIntervalMinutes = value
                        }
                    }

                    CheckBox {
                        text: qsTr("Start minimized")
                        checked: app.settings.startMinimized
                        onToggled: app.settings.startMinimized = checked
                    }

                    CheckBox {
                        text: qsTr("Start minimized to system tray")
                        checked: app.settings.startInTray
                        onToggled: app.settings.startInTray = checked
                    }

                    MenuSeparator { Layout.fillWidth: true }
                    Label { text: qsTr("Timer"); font.bold: true }

                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Timer mode:"); Layout.fillWidth: true }
                        ComboBox {
                            id: timerModeCombo
                            model: [qsTr("Simple"), qsTr("Pomodoro"), qsTr("Custom")]
                            currentIndex: app.settings.timerMode
                            onActivated: function(idx) { app.settings.timerMode = idx }
                        }
                    }

                    GridLayout {
                        columns: 2
                        Layout.fillWidth: true
                        visible: app.settings.timerMode !== 2
                        rowSpacing: 8
                        columnSpacing: 12

                        Label { text: qsTr("Task duration (minutes):") }
                        SpinBox {
                            from: 1; to: 1439
                            value: Math.round(app.settings.taskDuration / 60)
                            onValueModified: app.settings.taskDuration = value * 60
                        }

                        Label { text: qsTr("Break duration (minutes):") }
                        SpinBox {
                            from: 1; to: 1439
                            value: Math.round(app.settings.breakDuration / 60)
                            onValueModified: app.settings.breakDuration = value * 60
                        }

                        Label {
                            visible: app.settings.timerMode === 1
                            text: qsTr("Long break duration (minutes):")
                        }
                        SpinBox {
                            visible: app.settings.timerMode === 1
                            from: 1; to: 1439
                            value: Math.round(app.settings.longBreakDuration / 60)
                            onValueModified: app.settings.longBreakDuration = value * 60
                        }

                        Label {
                            visible: app.settings.timerMode === 1
                            text: qsTr("Have a long break after short breaks:")
                        }
                        SpinBox {
                            visible: app.settings.timerMode === 1
                            from: 1; to: 99
                            value: Math.max(1, app.settings.pomodoroPeriod - 1)
                            onValueModified: app.settings.pomodoroPeriod = value + 1
                        }
                    }

                    TimerScheduleWidget {
                        Layout.fillWidth: true
                        visible: app.settings.timerMode === 2
                        schedulePairs: app.settings.customSchedule
                        onScheduleModified: function(newPairs) {
                            app.settings.customSchedule = newPairs
                        }
                    }

                    CheckBox {
                        text: qsTr("Resume task after the break")
                        checked: app.settings.resumeTasksAfterBreak
                        onToggled: app.settings.resumeTasksAfterBreak = checked
                    }

                    CheckBox {
                        text: qsTr("Reset timer after switching tasks")
                        checked: app.settings.resetTimerOnTaskSwitch
                        onToggled: app.settings.resetTimerOnTaskSwitch = checked
                    }
                }
            }

            // 2. Notifications Page
            ScrollView {
                clip: true
                ColumnLayout {
                    width: settingsDialogRoot.availableWidth - 20
                    spacing: 12

                    Label { text: qsTr("General"); font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Reminder before task ends (seconds):"); Layout.fillWidth: true }
                        SpinBox {
                            from: 0; to: 600
                            value: app.settings.reminderTime
                            onValueModified: app.settings.reminderTime = value
                        }
                    }

                    CheckBox {
                        text: qsTr("Notify with sound on new tasks or status changes")
                        checked: app.settings.notifyTaskChangesSound
                        onToggled: app.settings.notifyTaskChangesSound = checked
                    }

                    CheckBox {
                        text: qsTr("Notify with system popup on new tasks or status changes")
                        checked: app.settings.notifyTaskChangesPopup
                        onToggled: app.settings.notifyTaskChangesPopup = checked
                    }

                    MenuSeparator { Layout.fillWidth: true }
                    Label { text: qsTr("Notification Sounds"); font.bold: true }

                    property var soundPresets: app.notifications.availableSoundPresets()

                    // Reminder sound
                    Label { text: qsTr("Reminder sound:") }
                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox {
                            Layout.fillWidth: true
                            textRole: "name"
                            valueRole: "id"
                            model: parent.parent.soundPresets
                            Component.onCompleted: currentIndex = Math.max(0, indexOfValue(app.settings.reminderSoundFile))
                            onActivated: {
                                app.settings.reminderSoundFile = currentValue
                                app.notifications.playSoundFile(currentValue, app.settings.reminderSoundVolume)
                            }
                        }
                        Slider {
                            from: 0.0; to: 1.0
                            value: app.settings.reminderSoundVolume
                            Layout.preferredWidth: 100
                            onMoved: app.settings.reminderSoundVolume = value
                        }
                        ToolButton {
                            text: "▶"
                            onClicked: app.notifications.playSoundFile(app.settings.reminderSoundFile, app.settings.reminderSoundVolume)
                        }
                    }

                    // Start of break sound
                    Label { text: qsTr("Start of break sound:") }
                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox {
                            Layout.fillWidth: true
                            textRole: "name"
                            valueRole: "id"
                            model: parent.parent.soundPresets
                            Component.onCompleted: currentIndex = Math.max(0, indexOfValue(app.settings.breakStartSoundFile))
                            onActivated: {
                                app.settings.breakStartSoundFile = currentValue
                                app.notifications.playSoundFile(currentValue, app.settings.breakStartSoundVolume)
                            }
                        }
                        Slider {
                            from: 0.0; to: 1.0
                            value: app.settings.breakStartSoundVolume
                            Layout.preferredWidth: 100
                            onMoved: app.settings.breakStartSoundVolume = value
                        }
                        ToolButton {
                            text: "▶"
                            onClicked: app.notifications.playSoundFile(app.settings.breakStartSoundFile, app.settings.breakStartSoundVolume)
                        }
                    }

                    // End of break sound
                    Label { text: qsTr("End of break sound:") }
                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox {
                            Layout.fillWidth: true
                            textRole: "name"
                            valueRole: "id"
                            model: parent.parent.soundPresets
                            Component.onCompleted: currentIndex = Math.max(0, indexOfValue(app.settings.breakEndSoundFile))
                            onActivated: {
                                app.settings.breakEndSoundFile = currentValue
                                app.notifications.playSoundFile(currentValue, app.settings.breakEndSoundVolume)
                            }
                        }
                        Slider {
                            from: 0.0; to: 1.0
                            value: app.settings.breakEndSoundVolume
                            Layout.preferredWidth: 100
                            onMoved: app.settings.breakEndSoundVolume = value
                        }
                        ToolButton {
                            text: "▶"
                            onClicked: app.notifications.playSoundFile(app.settings.breakEndSoundFile, app.settings.breakEndSoundVolume)
                        }
                    }
                }
            }

            // 3. Appearance Page
            ColumnLayout {
                spacing: 12

                Label { text: qsTr("General"); font.bold: true }

                CheckBox {
                    text: qsTr("Use small toolbar icons")
                    checked: app.settings.useSmallToolbarIcons
                    onToggled: app.settings.useSmallToolbarIcons = checked
                }

                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Appearance of the activity switcher:"); Layout.fillWidth: true }
                    ComboBox {
                        model: [qsTr("Text"), qsTr("Icons")]
                        currentIndex: app.settings.switcherUseIcons ? 1 : 0
                        onActivated: function(idx) { app.settings.switcherUseIcons = (idx === 1) }
                    }
                }

                CheckBox {
                    text: qsTr("Use a header bar")
                    checked: app.settings.useHeaderBar
                    onToggled: app.settings.useHeaderBar = checked
                }

                MenuSeparator { Layout.fillWidth: true }
                Label { text: qsTr("Theme"); font.bold: true }

                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Language:"); Layout.fillWidth: true }
                    ComboBox {
                        id: langCombo
                        model: [
                            { id: "system", label: qsTr("System (Auto)") },
                            { id: "uk", label: "Українська (Ukrainian)" },
                            { id: "de", label: "Deutsch (German)" },
                            { id: "fr", label: "Français (French)" },
                            { id: "es", label: "Español (Spanish)" },
                            { id: "it", label: "Italiano (Italian)" },
                            { id: "en", label: "English" }
                        ]
                        textRole: "label"
                        valueRole: "id"
                        Component.onCompleted: {
                            const cur = app.settings.language
                            let found = 0
                            for (let i = 0; i < model.length; ++i) {
                                if (model[i].id === cur) {
                                    found = i
                                    break
                                }
                            }
                            currentIndex = found
                        }
                        onActivated: function(idx) {
                            app.settings.language = model[idx].id
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Color scheme:"); Layout.fillWidth: true }
                    ComboBox {
                        model: [qsTr("Default"), qsTr("Light"), qsTr("Dark")]
                        currentIndex: app.settings.colorScheme
                        onActivated: function(idx) { app.settings.colorScheme = idx }
                    }
                }

                Item { Layout.fillHeight: true }
            }

            // 4. Shortcuts Page
            ColumnLayout {
                spacing: 8

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: app.shortcuts
                    spacing: 4

                    delegate: RowLayout {
                        required property string shortcutId
                        required property string description
                        required property string sequence
                        required property string readableSequence

                        width: ListView.view.width
                        spacing: 8

                        Label {
                            text: description
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }

                        TextField {
                            Layout.preferredWidth: 140
                            text: sequence
                            placeholderText: qsTr("Disabled")
                            onEditingFinished: app.shortcuts.setShortcut(shortcutId, text)
                        }

                        ToolButton {
                            text: "✕"
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Disable shortcut")
                            visible: sequence !== ""
                            onClicked: app.shortcuts.setShortcut(shortcutId, "")
                        }
                    }
                }

                Button {
                    Layout.alignment: Qt.AlignRight
                    text: qsTr("Reset Defaults")
                    onClicked: app.shortcuts.resetDefaults()
                }
            }

            // 5. Plugins Page
            ColumnLayout {
                spacing: 12

                Label {
                    text: qsTr("Desktop Integration Plugins")
                    font.bold: true
                }

                CheckBox {
                    text: qsTr("System Tray / AppIndicator (panel timer & quick controls)")
                    checked: app.settings.trayIndicatorEnabled
                    onToggled: app.settings.trayIndicatorEnabled = checked
                }

                CheckBox {
                    text: qsTr("Launcher / Dock Indicator (Unity LauncherEntry badge & progress)")
                    checked: app.settings.launcherIndicatorEnabled
                    onToggled: app.settings.launcherIndicatorEnabled = checked
                }

                Item { Layout.fillHeight: true }
            }

            // 6. Storage Page (SQLite Database & Settings Paths + Backup/Restore)
            ScrollView {
                clip: true
                ColumnLayout {
                    width: settingsDialogRoot.availableWidth - 20
                    spacing: 10

                    Label { text: qsTr("SQLite Database Location"); font.bold: true }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 11
                        opacity: 0.75
                        text: qsTr("All lists, tasks, descriptions, comments, Jira/YouTrack settings, and timer logs are stored in this SQLite file:")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: dbPathField
                            Layout.fillWidth: true
                            selectByMouse: true
                            text: app.databasePath
                        }
                        Button {
                            text: qsTr("Move / Apply")
                            onClicked: {
                                const ok = app.changeDatabasePath(dbPathField.text, true)
                                storageStatusLabel.text = ok
                                    ? qsTr("Database location updated successfully.")
                                    : qsTr("Failed to update database location.")
                            }
                        }
                    }

                    MenuSeparator { Layout.fillWidth: true }
                    Label { text: qsTr("Configuration File (INI)"); font.bold: true }
                    TextField {
                        Layout.fillWidth: true
                        readOnly: true
                        selectByMouse: true
                        text: app.configFilePath
                    }

                    MenuSeparator { Layout.fillWidth: true }
                    Label { text: qsTr("Backup & Restore Database"); font.bold: true }

                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: backupPathField
                            Layout.fillWidth: true
                            placeholderText: "/tmp/go-for-it-qt-backup.db"
                        }
                        Button {
                            text: qsTr("Export")
                            enabled: backupPathField.text.trim() !== ""
                            onClicked: {
                                const ok = app.exportBackup(backupPathField.text)
                                storageStatusLabel.text = ok
                                    ? qsTr("Backup exported to: ") + backupPathField.text
                                    : qsTr("Failed to export backup.")
                            }
                        }
                        Button {
                            text: qsTr("Import")
                            enabled: backupPathField.text.trim() !== ""
                            onClicked: {
                                const ok = app.importBackup(backupPathField.text)
                                storageStatusLabel.text = ok
                                    ? qsTr("Backup imported successfully.")
                                    : qsTr("Failed to import backup.")
                            }
                        }
                    }

                    Label {
                        id: storageStatusLabel
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                        color: palette.highlight
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }
    }
}
