import QtQuick
import QtQuick.Controls

// A labeled chrome button: quiet by default, filled when primary.
AbstractButton {
    id: button
    property bool primary: false
    property bool destructive: false
    readonly property color fill: destructive ? "#c62828" : theme.accent

    implicitWidth: Math.max(72, label.implicitWidth + 28)
    implicitHeight: 40
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    opacity: enabled ? 1 : 0.4

    background: Rectangle {
        radius: Math.min(8, theme.radius)
        color: button.primary ? (button.down ? Qt.darker(button.fill, 1.15) : button.hovered ? Qt.lighter(button.fill, 1.08) : button.fill)
             : button.down ? Qt.alpha(theme.foreground, 0.16)
             : button.hovered ? Qt.alpha(theme.foreground, 0.08) : "transparent"
    }
    contentItem: Label {
        id: label
        text: button.text
        color: button.primary ? (button.destructive ? "white" : theme.accentForeground) : theme.foreground
        font.pixelSize: 14
        font.weight: button.primary ? Font.DemiBold : Font.Normal
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
