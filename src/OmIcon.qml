import QtQuick
import "icons.js" as Icons

Image {
    property string name
    property color color: theme.foreground
    property real weight: 1.8
    width: 20
    height: 20
    sourceSize: Qt.size(width, height)
    smooth: true
    source: name ? Icons.source(name, color.toString(), weight) : ""
}
