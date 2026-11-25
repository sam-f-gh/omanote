import QtQuick

// The floating chrome surface shared by the tool shelf and zoom controls.
Rectangle {
    color: theme.surface
    radius: theme.radius > 0 ? Math.min(height / 2, theme.radius + 6) : 0
    border.color: Qt.alpha(theme.foreground, 0.12)
    border.width: 1
}
