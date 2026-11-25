import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// A square, touch-sized chrome button with a themed icon.
AbstractButton {
    id: button
    property string glyph
    property string tip
    property bool active: false
    property color tint: active ? theme.accentForeground : theme.foreground

    implicitWidth: 40
    implicitHeight: 40
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    opacity: enabled ? 1 : 0.35

    background: Rectangle {
        radius: Math.min(8, theme.radius)
        color: button.active ? theme.accent
             : button.down ? Qt.alpha(theme.foreground, 0.16)
             : button.hovered ? Qt.alpha(theme.foreground, 0.08) : "transparent"
        Behavior on color { ColorAnimation { duration: 90 } }
    }
    contentItem: Item {
        OmIcon {
            anchors.centerIn: parent
            name: button.glyph
            color: button.tint
        }
    }

    ToolTip.visible: hovered && tip.length > 0 && !down
    ToolTip.delay: 600
    ToolTip.text: tip
}
