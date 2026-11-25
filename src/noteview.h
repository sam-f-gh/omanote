#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QTimer>

#include "document.h"
#include "pagerenderer.h"
#include "viewport.h"

class QTabletEvent;

// The workspace: white pages on the themed backdrop, live ink, and every
// input path (stylus, touch, mouse, wheel, touchpad pinch).
//
// Committed ink is cached per page as one composited image at the current
// scale; the stroke being drawn is painted on top and only its dirty region
// is repainted per sample, so 240 Hz pens stay cheap however full the page.
class NoteView : public QQuickPaintedItem {
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

    void paint(QPainter *painter) override;

signals:
    void documentChanged();
    void toolChanged();
    void backdropChanged();
    void viewChanged();
    void eraserChanged();
    void interacted();  // Any new gesture; transient status hides on it.

protected:
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

    struct Cache {
        QImage background;       // Poppler render, white paper otherwise.
        double backgroundScale = 0;
        double requestedScale = 0;
        QImage composite;        // Background + committed ink.
        double compositeScale = 0;
        int compositeRevision = -1;
        double compositeBackground = -1;
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
    bool touchBlocked() const;
    void setHover(QPointF pos, bool visible);
    void setEraseEngaged(bool on);

    void syncPages();
    void viewMoved();
    void zoomTo(double zoom, QPointF focus);
    void scheduleRenders();
    void onRendered(quint64 key, double scale, const QImage &image, int generation);
    void onPageChanged(quint64 id);
    double deviceRatio() const;
    double backgroundScaleFor(const Page &page) const;
    bool needsBackground(const Page &page) const;
    QRectF snapped(QRectF r) const;
    void paintPage(QPainter &p, int index, const QRectF &rect);
    void buildComposite(const Page &page, Cache &cache, double scale);
    QRectF strokeScreenRect(int fromSample) const;
    void drawLive(int fromSample);
    QColor activeColor(Tool tool) const;
    double activeWidth(Tool tool) const;

    QPointer<Document> m_doc;
    Viewport m_view;
    PageRenderer m_renderer;
    QHash<quint64, Cache> m_cache;
    bool m_needsInitialZoom = true;
    bool m_zooming = false;
    bool m_fitMode = false;  // Keep fit-width as the window is resized.
    QTimer m_zoomSettle;
    QTimer m_fling;
    QPointF m_flingVelocity;

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
    // The stroke being drawn, rendered opaque into a view-sized layer one
    // tail at a time, so each sample costs the same however long the stroke.
    // Highlighters get their alpha and multiply when the layer is composited.
    QImage m_live;
    QRectF m_liveDirty;
    double m_liveZoom = 0;
    QPointF m_liveOrigin;
    QPointF m_last;
    QPointF m_lastEraseDoc;
    int m_erasePage = -1;
    QElapsedTimer m_panClock;
    QPointF m_panVelocity;

    // Pinch state.
    double m_pinchStartZoom = 1, m_pinchStartDistance = 1;
    QPointF m_pinchCentroid;
    bool m_touchIgnored = false;

    // Palm rejection: touches are ignored while the pen is near or down and
    // for a short grace period after it leaves.
    bool m_stylusNear = false;
    bool m_stylusDown = false;
    QElapsedTimer m_stylusClock;
    bool m_eraseButton = false;
    bool m_eraseEngaged = false;

    QPointF m_hover;
    bool m_hoverVisible = false;
};
