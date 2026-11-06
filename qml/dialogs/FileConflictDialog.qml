import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: conflictDialog
    title: qsTr("File Conflict")
    anchors.centerIn: parent
    width: 400
    modal: true

    property var pendingConfig: ({})
    property string conflictText: ""

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: conflictDialog.conflictText
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.bold: true
            text: qsTr("Should the existing file be replaced by your current list file, or should the existing file at the new location be kept?")
        }
    }

    footer: DialogButtonBox {
        Button {
            text: qsTr("Keep Existing")
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: {
                const id = app.listManager.saveListConfig(conflictDialog.pendingConfig, "keep")
                app.selectListById(id)
                conflictDialog.close()
            }
        }
        Button {
            text: qsTr("Replace File")
            DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            onClicked: {
                const id = app.listManager.saveListConfig(conflictDialog.pendingConfig, "replace")
                app.selectListById(id)
                conflictDialog.close()
            }
        }
        Button {
            text: qsTr("Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: conflictDialog.close()
        }
    }
}
