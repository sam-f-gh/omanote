import QtQuick
import QtQuick.Shapes
import "icons.js" as Icons

// Stroke icons drawn as vector paths by the scene graph, so they stay crisp
// at any display scale (1.25×, 1.5×…) and any sub-pixel position, instead of
// a pre-rasterized image being resampled.
Item {
    id: icon
    property string name
    property color color: theme.foreground
    property real weight: 1.8
    width: 20
    height: 20

    Shape {
        width: 24
        height: 24
        scale: icon.width / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: icon.color
            strokeWidth: icon.weight
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: Icons.paths[icon.name] || "" }
        }
    }
}
