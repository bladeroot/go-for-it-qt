import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: contributeDialog
    title: qsTr("Contributions and Donations")
    anchors.centerIn: parent
    width: 400
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
            text: qsTr("Submitting code, artwork, translations or documentation is a great way of contributing to the project:")
        }

        Label {
            text: "<a href=\"https://github.com/JMoerman/Go-For-It\">https://github.com/JMoerman/Go-For-It</a>"
            onLinkActivated: function(link) { Qt.openUrlExternally(link) }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("If you really like this app, you can buy the author a drink:")
        }

        Label {
            text: "<a href=\"https://jmoerman.github.io/donate/\">https://jmoerman.github.io/donate/</a>"
            onLinkActivated: function(link) { Qt.openUrlExternally(link) }
        }
    }
}
