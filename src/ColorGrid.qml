import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// A compact grid of ink colors, all legible on white paper.
Popup {
    id: popup
    signal picked(string color)

    function openFor(item) {
        const p = item.mapToItem(parent, 0, item.height + 8)
        x = Math.max(0, p.x - 12)
        y = p.y
        open()
    }

    padding: 10
    modal: false
    focus: true
    Material.roundedScale: theme.radius
    background: Pill { radius: Math.min(12, theme.radius + 4) }

    readonly property var palette: [
        "#18181c", "#4a4a52", "#8a8a94", "#ffffff", "#7a4a1f", "#b5651d",
        "#d22f2f", "#e8590c", "#f59f00", "#ffd60a", "#94d82d", "#16804a",
        "#0ca678", "#15aabf", "#5ec8ff", "#1f4fd1", "#4c3bd6", "#7b3fc4",
        "#ae3ec9", "#e64980", "#ff8ac2", "#ffa94d", "#7ee36b", "#a5d8ff"
    ]

    contentItem: Grid {
        columns: 6
        spacing: 6
        Repeater {
            model: popup.palette
            AbstractButton {
                id: cell
                required property string modelData
                width: 36; height: 36
                focusPolicy: Qt.NoFocus
                onClicked: { popup.picked(modelData); popup.close() }
                background: Rectangle {
                    radius: 18
                    color: cell.modelData
                    border.width: cell.hovered ? 2 : 1
                    border.color: cell.hovered ? theme.accent : Qt.alpha(theme.foreground, 0.25)
                }
            }
        }
    }
}
