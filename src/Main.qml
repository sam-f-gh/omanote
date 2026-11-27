import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

ApplicationWindow {
    id: win
    width: 1100
    height: 780
    minimumWidth: 420
    minimumHeight: 420
    visible: true
    title: backend.hasDocument ? backend.title + " — Omanote" : "Omanote"
    color: theme.background

    Material.theme: theme.dark ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.background: theme.surface
    Material.foreground: theme.foreground
    Material.roundedScale: theme.radius

    property bool quitting: false
    property var afterSave: null
    readonly property bool modalOpen: help.opened || unsaved.opened || clearConfirm.opened
    readonly property bool canvasKeys: backend.hasDocument && !modalOpen
    readonly property var view: workspace.item ? workspace.item.view : null

    // Runs an action that would drop the open document, asking first when
    // there is unsaved ink.
    function guarded(action) {
        if (backend.hasDocument && backend.document.dirty) {
            unsaved.pending = action
            unsaved.open()
        } else {
            action()
        }
    }
    function quit() { guarded(() => { win.quitting = true; Qt.quit() }) }
    function requestClear() {
        if (view && view.currentPageHasInk())
            clearConfirm.open()
        else if (workspace.item)
            workspace.item.showStatus(qsTr("This page has no ink"))
    }

    onClosing: (close) => {
        if (!quitting && backend.hasDocument && backend.document.dirty) {
            close.accepted = false
            quit()
        }
    }

    Connections {
        target: backend
        function onStatus(message) { if (workspace.item) workspace.item.showStatus(message) }
        function onSaved() {
            const next = win.afterSave
            win.afterSave = null
            if (next) next()
        }
        function onSaveCancelled() { win.afterSave = null }
    }

    Shortcut { sequences: ["?", "Shift+?"]; context: Qt.ApplicationShortcut; enabled: !unsaved.opened; onActivated: help.opened ? help.close() : help.open() }
    Shortcut { sequences: ["Q", "Ctrl+Q"]; context: Qt.ApplicationShortcut; enabled: !unsaved.opened; onActivated: win.quit() }
    Shortcut { sequence: "Ctrl+N"; context: Qt.ApplicationShortcut; enabled: !win.modalOpen; onActivated: win.guarded(() => backend.newNote()) }
    Shortcut { sequence: "Ctrl+O"; context: Qt.ApplicationShortcut; enabled: !win.modalOpen; onActivated: win.guarded(() => backend.openDialog()) }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: backend.hasDocument && !win.modalOpen; onActivated: win.guarded(() => backend.closeDocument()) }

    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: win.canvasKeys; onActivated: backend.tool = 0 }
    Shortcut { sequence: "H"; context: Qt.ApplicationShortcut; enabled: win.canvasKeys; onActivated: backend.tool = 1 }
    Shortcut { sequence: "E"; context: Qt.ApplicationShortcut; enabled: win.canvasKeys; onActivated: backend.tool = 2 }
    Shortcut { sequence: "Ctrl+Z"; context: Qt.ApplicationShortcut; enabled: win.canvasKeys; onActivated: backend.document.undo() }
    Shortcut { sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]; context: Qt.ApplicationShortcut; enabled: win.canvasKeys; onActivated: backend.document.redo() }
    Shortcut { sequence: "Ctrl+S"; context: Qt.ApplicationShortcut; enabled: win.canvasKeys && !backend.busy; onActivated: backend.save() }
    Shortcut { sequence: "Ctrl+Shift+S"; context: Qt.ApplicationShortcut; enabled: win.canvasKeys && !backend.busy; onActivated: backend.saveAs() }
    Shortcut { sequences: ["PgDown", "N"]; context: Qt.ApplicationShortcut; enabled: win.canvasKeys; onActivated: win.view.nextPage() }
    Shortcut { sequences: ["PgUp", "B"]; context: Qt.ApplicationShortcut; enabled: win.canvasKeys; onActivated: win.view.previousPage() }
    Shortcut { sequences: ["+", "=", "Shift++"]; context: Qt.ApplicationShortcut; enabled: win.canvasKeys; onActivated: win.view.addPage() }
    Shortcut { sequences: ["Ctrl++", "Ctrl+="]; context: Qt.ApplicationShortcut; enabled: win.canvasKeys; onActivated: win.view.zoomIn() }
    Shortcut { sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; enabled: win.canvasKeys; onActivated: win.view.zoomOut() }
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; enabled: win.canvasKeys; onActivated: win.view.fitWidth() }

    // Right-to-left languages mirror the chrome; the page itself never mirrors.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        StartScreen {
            anchors.fill: parent
            visible: !backend.hasDocument
            onHelpRequested: help.open()
        }

        Loader {
            id: workspace
            anchors.fill: parent
            active: backend.hasDocument
            sourceComponent: Workspace {
                LayoutMirroring.enabled: false
                onHomeRequested: win.guarded(() => backend.closeDocument())
                onClearRequested: win.requestClear()
            }
        }
    }

    HelpOverlay { id: help }

    ConfirmDialog {
        id: unsaved
        property var pending: null
        title: qsTr("Save changes to “%1”?").arg(backend.title)
        message: qsTr("Your ink will be lost if you don't save.")
        acceptText: qsTr("Save")
        discardText: qsTr("Discard")
        onAccepted: {
            win.afterSave = pending
            pending = null
            backend.save()
        }
        onDiscarded: {
            const next = pending
            pending = null
            if (next) next()
        }
    }

    ConfirmDialog {
        id: clearConfirm
        title: qsTr("Clear this page?")
        message: qsTr("Every stroke on the page is removed. Undo brings them back.")
        acceptText: qsTr("Clear Page")
        destructive: true
        onAccepted: win.view.clearCurrentPage()
    }
}
