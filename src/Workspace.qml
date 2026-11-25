import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Omanote

// A document open: top bar, pages, the tool shelf, zoom, and a status line.
Item {
    id: workspace
    property alias view: canvas
    signal homeRequested()
    signal saveRequested()
    signal clearRequested()

    function showStatus(message) {
        status.text = message
        status.opacity = 1
        status.shownAt = Date.now()
        statusTimer.restart()
    }

    TopBar {
        id: top
        anchors { left: parent.left; right: parent.right; top: parent.top }
        view: canvas
        onHomeRequested: workspace.homeRequested()
        onSaveRequested: workspace.saveRequested()
    }

    NoteView {
        id: canvas
        anchors { left: parent.left; right: parent.right; top: top.bottom; bottom: parent.bottom }
        document: backend.document
        tool: backend.tool
        penColor: backend.penColor
        penWidth: backend.penWidth
        highlightColor: backend.highlightColor
        highlightWidth: backend.highlightWidth
        backdrop: theme.backdrop
        onInteracted: if (Date.now() - status.shownAt > 600) status.opacity = 0
    }

    ToolShelf {
        anchors { left: parent.left; top: top.bottom; margins: 12 }
        onClearRequested: workspace.clearRequested()
    }

    Pill {
        id: zoomPill
        anchors { right: parent.right; bottom: parent.bottom; margins: 12 }
        width: zoomRow.implicitWidth + 8
        height: 44
        Row {
            id: zoomRow
            anchors.centerIn: parent
            IconButton { glyph: "minus"; tip: qsTr("Zoom out (Ctrl+-)"); width: 36; height: 36; onClicked: canvas.zoomOut() }
            AbstractButton {
                id: percent
                width: 58; height: 36
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                onClicked: canvas.toggleZoom()
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Fit width / 100 %")
                background: Rectangle {
                    radius: Math.min(8, theme.radius)
                    color: percent.hovered ? Qt.alpha(theme.foreground, 0.08) : "transparent"
                }
                contentItem: Label {
                    text: Math.round(canvas.zoom * 100).toLocaleString(Qt.locale(), 'f', 0) + " %"
                    color: theme.foreground
                    font.pixelSize: 13
                    font.features: { "tnum": 1 }
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    property var _lang: translator.language
                }
            }
            IconButton { glyph: "plus"; tip: qsTr("Zoom in (Ctrl++)"); width: 36; height: 36; onClicked: canvas.zoomIn() }
        }
    }

    Label {
        id: status
        anchors { left: parent.left; bottom: parent.bottom; margins: 16; right: zoomPill.left; rightMargin: 16 }
        elide: Text.ElideRight
        color: theme.foreground
        opacity: 0
        visible: opacity > 0
        font.pixelSize: 13
        leftPadding: 10; rightPadding: 10; topPadding: 6; bottomPadding: 6
        width: Math.min(implicitWidth, parent.width - zoomPill.width - 48)
        background: Rectangle {
            color: theme.surface
            radius: Math.min(6, theme.radius)
            border.color: Qt.alpha(theme.foreground, 0.12)
        }
        Behavior on opacity { NumberAnimation { duration: 220 } }
        // The tap that triggered a message must not hide it at once.
        property double shownAt: 0
        Timer {
            id: statusTimer
            interval: 2600
            onTriggered: status.opacity = 0
        }
    }
}
