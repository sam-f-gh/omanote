#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>

#include "document.h"
#include "tilerenderer.h"
#include "viewport.h"

class QTabletEvent;
class QSGImageNode;
class QSGRectangleNode;
class QSGGeometryNode;
class LiveLayer;

// The workspace: white pages on the themed backdrop, live ink, and every
// input path (stylus, touch, mouse, wheel, touchpad pinch).
//
// Pages are scene-graph textures, rendered in tiles on worker threads, so
// scrolling and zooming are GPU transforms with no repainting. The stroke
// being drawn lives on a small painted overlay updated segment by segment,
// and stays there until the tiles that include it arrive.
class NoteView : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(Document *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(QColor penColor MEMBER m_penColor NOTIFY toolChanged)
    Q_PROPERTY(double penWidth MEMBER m_penWidth NOTIFY toolChanged)
    Q_PROPERTY(QColor highlightColor MEMBER m_highlightColor NOTIFY toolChanged)
    Q_PROPERTY(double highlightWidth MEMBER m_highlightWidth NOTIFY toolChanged)
    Q_PROPERTY(QColor backdrop READ backdrop WRITE setBackdrop NOTIFY backdropChanged)
    Q_PROPERTY(double zoom READ zoom NOTIFY viewChanged)
    Q_PROPERTY(int currentPage READ currentPage NOTIFY viewChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY viewChanged)
    Q_PROPERTY(bool eraserEngaged READ eraserEngaged NOTIFY eraserChanged)

public:
    explicit NoteView(QQuickItem *parent = nullptr);
    ~NoteView() override;

    Document *document() const { return m_doc; }
    void setDocument(Document *doc);
    int tool() const { return int(m_tool); }
    void setTool(int tool);
    QColor backdrop() const { return m_backdrop; }
    void setBackdrop(const QColor &c);
    double zoom() const { return m_view.zoom(); }
    int currentPage() const { return m_view.currentPage(); }
    int pageCount() const { return m_view.pageCount(); }
    bool eraserEngaged() const;
    const Viewport &viewport() const { return m_view; }
    // True once every visible tile shows the current ink (for tests).
    bool tilesCurrent() const;

    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void toggleZoom();   // Fit width <-> 100 %.
    Q_INVOKABLE void fitWidth();
    Q_INVOKABLE void nextPage();
    Q_INVOKABLE void previousPage();
    Q_INVOKABLE void goToPage(int index);
    Q_INVOKABLE void addPage();      // Blank page after the current one.
    Q_INVOKABLE bool clearCurrentPage();
    Q_INVOKABLE bool currentPageHasInk() const;

signals:
    void documentChanged();
    void toolChanged();
    void backdropChanged();
    void viewChanged();
    void eraserChanged();
    void interacted();  // Any new gesture; transient status hides on it.

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void releaseResources() override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void touchEvent(QTouchEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void touchUngrabEvent() override;
    void mouseUngrabEvent() override;

private:
    enum class Gesture { None, Ink, Erase, Pan, VerticalPan, Pinch };
    enum class Source { None, Stylus, Mouse, Touch };

    struct Tile {
        double scale = 0;
        int tx = 0, ty = 0;
        int revision = -1;        // Ink revision shown; -1 = nothing yet.
        int requested = -1;       // Revision asked of the renderer.
        bool blank = false;       // Plain white paper: no texture needed.
        QImage image;             // Waiting for upload, then dropped.
        quint64 serial = 0;       // Bumped per new image.
    };
    struct PageTiles {
        int revision = 0;
        QList<Tile> tiles;
    };

    void tabletEvent(QTabletEvent *event, QPointF pos);
    bool hitsMe(QPointF scenePos) const;
    void beginStroke(QPointF pos, double pressure, bool erase, Source source);
    void extendStroke(QPointF pos, double pressure);
    void endStroke();
    void eraseAlong(QPointF from, QPointF to);
    void beginPan(QPointF pos, Gesture kind, Source source);
    void panTo(QPointF pos);
    void endPan();
    void stopFling() { m_flinging = false; }
    void startFling(QPointF velocity);
    bool scrollBy(double dx, double dy);
    bool canGrowPaper() const;
    double paperZone() const;
    void flingFrame();
    bool touchBlocked() const;
    void setHover(QPointF pos, bool visible);
    void setEraseEngaged(bool on);

    void syncPages();
    void viewMoved();
    void trimUnusedPaper();
    void zoomTo(double zoom, QPointF focus);
    void scheduleRenders();
    void onRendered(quint64 page, double scale, int tx, int ty, int revision, const QImage &image, int generation);
    void onPageChanged(quint64 id, const QRectF &region);
    void resetTiles();
    double deviceRatio() const;
    double targetScale() const;
    bool needsBackground(const Page &page) const;
    QRect tileRange(int index, double scale, double margin) const;
    QRectF snapped(QRectF r) const;
    QRectF strokeScreenRect(int fromSample) const;
    void drawLive(int fromSample);
    void clearLive();
    void maybeClearLive();
    QColor activeColor(Tool tool) const;
    double activeWidth(Tool tool) const;

    QPointer<Document> m_doc;
    Viewport m_view;
    TileRenderer m_renderer;
    QHash<quint64, PageTiles> m_tiles;
    LiveLayer *m_overlay = nullptr;
    bool m_needsInitialZoom = true;
    bool m_zooming = false;
    bool m_fitMode = false;  // Keep fit-width as the window is resized.
    QTimer m_zoomSettle;
    QTimer m_scheduleTimer;  // Coalesces render scheduling to once per event batch.
    QTimer m_renderIdle;     // No renders for a while: the renderer lets go of caches.

    // Scene-graph nodes, touched only in updatePaintNode (GUI blocked).
    struct NodeKey {
        quint64 page; qint64 scale; int tx, ty;
        bool operator==(const NodeKey &o) const { return page == o.page && scale == o.scale && tx == o.tx && ty == o.ty; }
    };
    friend size_t qHash(const NodeKey &k, size_t seed) { return qHashMulti(seed, k.page, k.scale, k.tx, k.ty); }
    struct NodeEntry { QSGImageNode *node = nullptr; quint64 serial = 0; quint64 laidOut = 0; double pxPerUnit = 1; };
    QHash<NodeKey, NodeEntry> m_nodes;
    // A stable tree, so the renderer keeps its batches between frames:
    // root > backdrop, then per page: shadow/border/paper rects, stand-in
    // tiles (other scales), current tiles. Frames mostly just move rects.
    struct PageNode {
        QSGNode *group = nullptr;
        QSGGeometryNode *shadow = nullptr; // One soft ring, drawn only around the sheet.
        QSGRectangleNode *paper = nullptr;
        QSGNode *standIns = nullptr;
        QSGNode *current = nullptr;
        quint64 laidOut = 0;
    };
    QHash<quint64, PageNode> m_pageNodes;
    QSGRectangleNode *m_backdropNode = nullptr;
    // Pages live under one transform: scrolling moves only it. Nodes are laid
    // out again only when this stamp changes (zoom, width, scale, pages).
    QSGTransformNode *m_contentNode = nullptr;
    quint64 m_layoutStamp = 1;
    struct LayoutKey { double zoom = 0, width = 0, dpr = 0; quint64 pages = 0; } m_layoutKey;
    quint64 m_pagesVersion = 0;
    QHash<quint64, int> m_indexById;

    // Kinetic scrolling, stepped once per rendered frame.
    bool m_flinging = false;
    QPointF m_flingVelocity;
    QElapsedTimer m_flingClock;
    // Touchpad scrolling keeps its own velocity, flung on lift like a finger.
    QPointF m_wheelVelocity;
    QElapsedTimer m_wheelClock;
    double m_downPush = 0; // Steady downward scrolling near the end of a notebook.

    Tool m_tool = Tool::Pen;
    QColor m_penColor{0x18, 0x18, 0x1c};
    double m_penWidth = 2.5;
    QColor m_highlightColor{0xff, 0xd6, 0x0a};
    double m_highlightWidth = 22;
    QColor m_backdrop{0x10, 0x10, 0x12};

    Gesture m_gesture = Gesture::None;
    Source m_source = Source::None;
    PagePtr m_strokePage;
    StrokePtr m_stroke;
    double m_smoothedPressure = 0.5;
    QPointF m_last;
    QElapsedTimer m_panClock;
    QPointF m_panVelocity;

    // Live-layer bookkeeping: where it was drawn, and which page revision
    // the tiles must reach before it can be cleared.
    double m_liveZoom = 0;
    QPointF m_liveOrigin;
    QHash<quint64, int> m_pending; // Page -> revision the tiles must reach.

    // Last values announced to QML, so bindings only re-run on real changes.
    double m_shownZoom = -1;
    int m_shownPage = -1, m_shownCount = -1;
    QList<TileRenderer::Job> m_lastJobs;

    double m_pinchStartZoom = 1, m_pinchStartDistance = 1;
    QPointF m_pinchCentroid;
    bool m_touchIgnored = false;

    // Palm rejection: touches are ignored while the pen is near or down and
    // for a short grace period after it leaves.
    bool m_stylusNear = false;
    bool m_stylusDown = false;
    QElapsedTimer m_stylusClock;
    bool m_eraseEngaged = false;
};
