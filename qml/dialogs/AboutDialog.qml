import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: aboutDialog
    title: qsTr("About Go for It Qt")
    anchors.centerIn: parent
    width: 400
    modal: true
    standardButtons: Dialog.Close

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            text: "Go for It Qt (go-for-it-qt) 1.9.5"
            font.bold: true
            font.pixelSize: 17
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("A stylish to-do list with built-in productivity timer, SQLite storage, and Jira / YouTrack integration.")
        }

        MenuSeparator { Layout.fillWidth: true }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            text: qsTr("Based on Go-For-It! by Jonathan Moerman & Manuel Kehl\nArtwork: Micah Ilbery\nLicense: GNU GPL v3")
        }
    }
}
