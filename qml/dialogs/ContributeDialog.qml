import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: contributeDialog
    title: qsTr("Contributions and Support")
    anchors.centerIn: parent
    width: 420
    modal: true
    standardButtons: Dialog.Close

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label {
            text: qsTr("Thank you for supporting Go for It Qt!")
            font.bold: true
            font.pixelSize: 15
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Submitting code, bug reports, translations or documentation is a great way of contributing to the project:")
        }

        Label {
            text: "<a href=\"https://github.com/bladeroot/go-for-it-qt\">https://github.com/bladeroot/go-for-it-qt</a>"
            onLinkActivated: function(link) { Qt.openUrlExternally(link) }
        }
    }
}
