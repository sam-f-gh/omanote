import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: start
    signal helpRequested()

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 2 * 264 + 16)
        spacing: 0

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            width: 64; height: 64
            radius: Math.min(16, theme.radius * 2)
            color: theme.accent
            OmIcon { anchors.centerIn: parent; width: 34; height: 34; name: "pen"; color: theme.accentForeground; weight: 2 }
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 18
            text: "Omanote"
            color: theme.foreground
            font.pixelSize: 30
            font.weight: Font.DemiBold
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 4
            text: qsTr("Handwritten notes and PDF annotation")
            color: theme.muted
            font.pixelSize: 14
        }

        Flow {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 32
            Layout.preferredWidth: Math.min(start.width - 48, 2 * 264 + 16)
            spacing: 16
            Repeater {
                model: [
                    { icon: "note", title: qsTr("New Note"), detail: qsTr("Endless blank paper"), keys: "Ctrl+N" },
                    { icon: "open", title: qsTr("Open PDF"), detail: qsTr("Write on any PDF"), keys: "Ctrl+O" }
                ]
                AbstractButton {
                    id: card
                    required property var modelData
                    required property int index
                    width: 264; height: 150
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    enabled: !backend.busy
                    onClicked: index === 0 ? backend.newNote() : backend.openDialog()
                    background: Rectangle {
                        radius: theme.radius > 0 ? Math.min(14, theme.radius + 4) : 0
                        color: card.down ? Qt.alpha(theme.accent, 0.18) : card.hovered ? Qt.alpha(theme.foreground, 0.07) : theme.surface
                        border.width: 1
                        border.color: card.hovered ? theme.accent : Qt.alpha(theme.foreground, 0.12)
                        Behavior on color { ColorAnimation { duration: 90 } }
                    }
                    contentItem: ColumnLayout {
                        spacing: 6
                        OmIcon { name: card.modelData.icon; width: 30; height: 30; color: theme.accent; Layout.leftMargin: 18; Layout.topMargin: 4 }
                        Label {
                            text: card.modelData.title
                            color: theme.foreground
                            font.pixelSize: 17
                            font.weight: Font.Medium
                            Layout.leftMargin: 18
                            Layout.topMargin: 8
                        }
                        Label {
                            text: card.modelData.detail + " · " + card.modelData.keys
                            color: theme.muted
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            Layout.leftMargin: 18
                            Layout.rightMargin: 12
                            Layout.fillWidth: true
                        }
                    }
                }
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 18
            Layout.maximumWidth: start.width - 48
            visible: backend.error.length > 0
            text: backend.error
            color: theme.dark ? "#ff8a80" : "#b3261e"
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            font.pixelSize: 13
        }
    }

    AbstractButton {
        anchors { horizontalCenter: parent.horizontalCenter; bottom: parent.bottom; bottomMargin: 18 }
        focusPolicy: Qt.NoFocus
        onClicked: start.helpRequested()
        contentItem: Label {
            text: qsTr("Press ? for shortcuts")
            color: theme.muted
            font.pixelSize: 12
        }
        background: null
        padding: 8
    }
}
