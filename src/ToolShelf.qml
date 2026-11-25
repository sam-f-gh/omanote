import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Context for the active tool: swatches and three widths, or Clear Page.
Pill {
    id: shelf
    signal clearRequested()

    readonly property bool highlighter: backend.tool === 1
    readonly property bool eraser: backend.tool === 2
    readonly property var colors: highlighter
        ? ["#ffd60a", "#7ee36b", "#5ec8ff", "#ff8ac2", "#ffa94d"]
        : ["#18181c", theme.accent, "#1f4fd1", "#d22f2f", "#16804a", "#7b3fc4"]
    readonly property string current: highlighter ? backend.highlightColor : backend.penColor

    function pick(c) {
        if (highlighter) backend.highlightColor = c
        else backend.penColor = c
    }

    width: row.implicitWidth + 16
    height: 52

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: 4

        Repeater {
            model: shelf.eraser ? [] : shelf.colors
            AbstractButton {
                id: swatch
                required property string modelData
                implicitWidth: 36; implicitHeight: 40
                focusPolicy: Qt.NoFocus
                // Long-press or double-tap opens the full grid.
                onPressAndHold: grid.openFor(swatch)
                onDoubleClicked: grid.openFor(swatch)
                onClicked: shelf.pick(modelData)
                contentItem: Item {
                    Rectangle {
                        anchors.centerIn: parent
                        width: 26; height: 26; radius: 13
                        color: swatch.modelData
                        border.width: 1
                        border.color: Qt.alpha(theme.foreground, 0.25)
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        visible: Qt.colorEqual(shelf.current, swatch.modelData)
                        width: 34; height: 34; radius: 17
                        color: "transparent"
                        border.width: 2
                        border.color: theme.accent
                    }
                }
                background: null
            }
        }

        // A custom color the curated swatches don't include.
        Rectangle {
            visible: !shelf.eraser && !shelf.colors.some(c => Qt.colorEqual(c, shelf.current))
            width: 26; height: 26; radius: 13
            color: shelf.current
            border.width: 2
            border.color: theme.accent
            Layout.leftMargin: 4; Layout.rightMargin: 4
        }

        IconButton {
            id: paletteButton
            visible: !shelf.eraser
            glyph: "palette"
            tip: qsTr("More colors")
            onClicked: grid.openFor(paletteButton)
        }

        Rectangle {
            visible: !shelf.eraser
            width: 1; height: 24
            color: Qt.alpha(theme.foreground, 0.12)
            Layout.leftMargin: 4; Layout.rightMargin: 4
        }

        Repeater {
            model: shelf.eraser ? [] : [0, 1, 2]
            AbstractButton {
                id: sizeButton
                required property int modelData
                readonly property bool selected: (shelf.highlighter ? backend.highlightSize : backend.penSize) === modelData
                implicitWidth: 40; implicitHeight: 40
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                onClicked: shelf.highlighter ? backend.highlightSize = modelData : backend.penSize = modelData
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: [qsTr("Fine"), qsTr("Medium"), qsTr("Bold")][modelData]
                background: Rectangle {
                    radius: Math.min(8, theme.radius)
                    color: sizeButton.selected ? Qt.alpha(theme.accent, 0.22)
                         : sizeButton.hovered ? Qt.alpha(theme.foreground, 0.08) : "transparent"
                }
                contentItem: Item {
                    Rectangle {
                        anchors.centerIn: parent
                        readonly property real d: shelf.highlighter ? [8, 12, 17][sizeButton.modelData]
                                                                    : [4, 7, 12][sizeButton.modelData]
                        width: shelf.highlighter ? 8 : d
                        height: d
                        radius: shelf.highlighter ? 2 : d / 2
                        color: shelf.highlighter ? shelf.current : theme.foreground
                    }
                }
            }
        }

        TextButton {
            visible: shelf.eraser
            text: qsTr("Clear Page")
            onClicked: shelf.clearRequested()
        }
    }

    ColorGrid {
        id: grid
        onPicked: (c) => shelf.pick(c)
    }
}
