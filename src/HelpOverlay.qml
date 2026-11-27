import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: help
    anchors.centerIn: Overlay.overlay
    modal: true
    focus: true
    padding: 24
    Overlay.modal: Rectangle { color: Qt.alpha("#000000", 0.45) }
    background: Pill {}

    readonly property var rows: [
        ["P · H · E", qsTr("Pen · Highlighter · Eraser")],
        ["Ctrl+Z", qsTr("Undo")],
        ["Ctrl+Shift+Z · Ctrl+Y", qsTr("Redo")],
        ["Ctrl+S", qsTr("Save")],
        ["Ctrl+Shift+S", qsTr("Save As")],
        ["Ctrl+O · Ctrl+N", qsTr("Open PDF · New Note")],
        ["PageDown · N", qsTr("Next page")],
        ["PageUp · B", qsTr("Previous page")],
        ["+ · =", qsTr("Add blank page")],
        ["Ctrl+Wheel · Ctrl+± · Ctrl+0", qsTr("Zoom · Fit width")],
        ["Esc", qsTr("Close document")],
        ["?", qsTr("Show or hide shortcuts")],
        ["Q · Ctrl+Q", qsTr("Quit")]
    ]

    // Built only while shown, so the window starts without it.
    contentItem: Loader {
        active: help.visible
        sourceComponent: ColumnLayout {
            spacing: 14
            Label {
                text: qsTr("Shortcuts")
                color: theme.foreground
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
            GridLayout {
                columns: 2
                columnSpacing: 28
                rowSpacing: 8
                Repeater {
                    model: help.rows.length * 2
                    Label {
                        required property int index
                        readonly property bool key: index % 2 === 0
                        text: help.rows[Math.floor(index / 2)][key ? 0 : 1]
                        color: key ? theme.accent : theme.foreground
                        font.pixelSize: 13
                    }
                }
            }
            Label {
                Layout.topMargin: 4
                Layout.maximumWidth: 420
                wrapMode: Text.WordWrap
                text: qsTr("Pen writes, one finger scrolls, two fingers zoom. The pen's eraser end or barrel button erases.")
                color: theme.muted
                font.pixelSize: 12
            }
        }
    }
}
