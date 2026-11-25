import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Home · title | tools (centered) | history · pages · save
Rectangle {
    id: bar
    property var view
    signal homeRequested()
    signal saveRequested()

    readonly property bool narrow: width < 760
    // Very narrow tiles drop the page counter and slim the tool buttons.
    readonly property bool tight: width < 560
    implicitHeight: 52
    color: theme.surface

    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 1
        color: Qt.alpha(theme.foreground, 0.1)
    }

    RowLayout {
        id: left
        anchors { left: parent.left; leftMargin: 6; verticalCenter: parent.verticalCenter }
        width: Math.max(0, tools.x - 12)
        spacing: 4
        IconButton { glyph: "back"; tip: qsTr("Close (Esc)"); onClicked: bar.homeRequested() }
        Label {
            Layout.maximumWidth: Math.max(0, left.width - 70)
            text: backend.title
            elide: Text.ElideMiddle
            color: theme.foreground
            font.pixelSize: 14
            font.weight: Font.Medium
        }
        Rectangle {
            visible: backend.document && backend.document.dirty
            width: 8; height: 8; radius: 4
            color: theme.accent
            ToolTip.visible: dotHover.hovered
            ToolTip.text: qsTr("Unsaved changes")
            HoverHandler { id: dotHover }
        }
        Item { Layout.fillWidth: true }
    }

    // Centered when there's room, sliding aside smoothly in narrow tiles.
    Rectangle {
        id: tools
        readonly property real ideal: (bar.width - width) / 2
        x: Math.max(Math.min(ideal, right.x - width - (bar.tight ? 4 : 12)), bar.tight ? 46 : bar.narrow ? 56 : 160)
        anchors.verticalCenter: parent.verticalCenter
        width: toolRow.width + 8
        height: 44
        radius: Math.min(10, theme.radius + 2)
        color: Qt.alpha(theme.foreground, 0.06)
        Row {
            id: toolRow
            anchors.centerIn: parent
            spacing: 2
            Repeater {
                model: [
                    { glyph: "pen", tip: qsTr("Pen (P)") },
                    { glyph: "highlighter", tip: qsTr("Highlighter (H)") },
                    { glyph: "eraser", tip: qsTr("Eraser (E)") }
                ]
                IconButton {
                    required property var modelData
                    required property int index
                    width: bar.tight ? 36 : 44; height: 36
                    glyph: modelData.glyph
                    tip: modelData.tip
                    active: backend.tool === index
                    onClicked: backend.tool = index
                }
            }
        }
    }

    RowLayout {
        id: right
        anchors { right: parent.right; rightMargin: 6; verticalCenter: parent.verticalCenter }
        spacing: 2
        IconButton {
            glyph: "undo"; tip: qsTr("Undo (Ctrl+Z)")
            enabled: backend.document && backend.document.canUndo
            onClicked: backend.document.undo()
        }
        IconButton {
            glyph: "redo"; tip: qsTr("Redo (Ctrl+Shift+Z)")
            enabled: backend.document && backend.document.canRedo
            onClicked: backend.document.redo()
        }
        Rectangle { visible: !bar.tight; width: 1; height: 24; color: Qt.alpha(theme.foreground, 0.12); Layout.leftMargin: 4; Layout.rightMargin: 4 }
        IconButton {
            glyph: "up"; tip: qsTr("Previous page (PageUp)")
            visible: !bar.narrow
            enabled: bar.view && bar.view.currentPage > 0
            onClicked: bar.view.previousPage()
        }
        Label {
            visible: !bar.tight
            text: bar.view ? qsTr("%1 / %2").arg((bar.view.currentPage + 1).toLocaleString(Qt.locale(), 'f', 0))
                                             .arg(bar.view.pageCount.toLocaleString(Qt.locale(), 'f', 0)) : ""
            color: theme.foreground
            font.pixelSize: 13
            font.features: { "tnum": 1 }
            horizontalAlignment: Text.AlignHCenter
            Layout.minimumWidth: 44
            property var _lang: translator.language
        }
        IconButton {
            glyph: "down"; tip: qsTr("Next page (PageDown)")
            visible: !bar.narrow
            enabled: bar.view && bar.view.currentPage < bar.view.pageCount - 1
            onClicked: bar.view.nextPage()
        }
        IconButton { glyph: "addpage"; tip: qsTr("Add page (+)"); onClicked: bar.view.addPage() }
        Rectangle { width: 1; height: 24; color: Qt.alpha(theme.foreground, 0.12); Layout.leftMargin: 4; Layout.rightMargin: 4 }
        IconButton {
            glyph: "save"; tip: qsTr("Save (Ctrl+S)")
            enabled: !backend.busy
            onClicked: bar.saveRequested()
        }
    }
}
