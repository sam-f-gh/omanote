import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A small modal question with up to three answers.
Popup {
    id: dialog
    property string title
    property string message
    property string acceptText: qsTr("Save")
    property string discardText: ""
    property bool destructive: false
    signal accepted()
    signal discarded()

    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 22
    width: Math.min(420, (parent ? parent.width : 420) - 32)
    Material.roundedScale: theme.radius
    Overlay.modal: Rectangle { color: Qt.alpha("#000000", 0.45) }
    background: Pill { radius: Math.min(14, theme.radius + 4) }

    contentItem: ColumnLayout {
        spacing: 10
        Label {
            text: dialog.title
            color: theme.foreground
            font.pixelSize: 17
            font.weight: Font.DemiBold
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
        Label {
            text: dialog.message
            color: theme.muted
            font.pixelSize: 13
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Flow {
            Layout.fillWidth: true
            Layout.topMargin: 10
            layoutDirection: Qt.RightToLeft
            spacing: 8
            TextButton {
                text: dialog.acceptText
                primary: true
                destructive: dialog.destructive
                onClicked: { dialog.close(); dialog.accepted() }
            }
            TextButton {
                visible: dialog.discardText.length > 0
                text: dialog.discardText
                onClicked: { dialog.close(); dialog.discarded() }
            }
            TextButton {
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
        }
    }
}
