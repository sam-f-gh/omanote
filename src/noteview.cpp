#include "noteview.h"
#include "touchpadhold.h"

#include <QGuiApplication>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGImageNode>
#include <QSGVertexColorMaterial>
#include <QSGNode>
#include <QSGRectangleNode>
#include <QSGTexture>
#include <QTabletEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
constexpr int PalmGraceMs = 200;
constexpr int HoverTrustMs = 1500;
const int LayerIdleMs = qEnvironmentVariableIntValue("OMANOTE_LAYER_IDLE_MS") > 0 ? qEnvironmentVariableIntValue("OMANOTE_LAYER_IDLE_MS") : 15000;
constexpr int UploadsPerFrame = 4; // 512² tiles: ~4 MB a frame keeps frames smooth.

// A soft shadow as one ring of vertex-colored triangles from the sheet's edge
// (darkest, doubling as the hairline border) out to transparent, shifted a
// little down. It covers only the ring, not the whole page like stacked rects.
void layoutShadow(QSGGeometry *g, const QRectF &r) {
    const QRectF o = r.adjusted(-5, -3, 5, 7);
    auto *v = g->vertexDataAsColoredPoint2D();
    const QPointF in[4] = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};
    const QPointF out[4] = {o.topLeft(), o.topRight(), o.bottomRight(), o.bottomLeft()};
    for (int i = 0; i < 4; ++i) {
        v[i].set(float(in[i].x()), float(in[i].y()), 0, 0, 0, 46);  // Premultiplied black.
        v[4 + i].set(float(out[i].x()), float(out[i].y()), 0, 0, 0, 0);
    }
}

QSGGeometryNode *makeShadow() {
    auto *g = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 8, 24, QSGGeometry::UnsignedShortType);
    g->setDrawingMode(QSGGeometry::DrawTriangles);
    quint16 *idx = g->indexDataAsUShort();
    for (int i = 0; i < 4; ++i) {
        const quint16 a = quint16(i), b = quint16((i + 1) % 4);
        const quint16 quad[6] = {a, b, quint16(4 + b), a, quint16(4 + b), quint16(4 + a)};
        std::copy(quad, quad + 6, idx + i * 6);
    }
    auto *n = new QSGGeometryNode;
    n->setGeometry(g);
    n->setFlag(QSGNode::OwnsGeometry);
    n->setMaterial(new QSGVertexColorMaterial);
    n->setFlag(QSGNode::OwnsMaterial);
    return n;
}

bool realMouse(const QPointerEvent *e) {
    const auto type = e->device() ? e->device()->type() : QInputDevice::DeviceType::Mouse;
    return type == QInputDevice::DeviceType::Mouse || type == QInputDevice::DeviceType::TouchPad;
}

double length(QPointF p) { return std::hypot(p.x(), p.y()); }
}

// The transparent layer above the pages: the stroke being drawn (and ones
// just committed, until their tiles arrive) plus the eraser ring. Strokes are
// drawn opaque, pens and highlighters on separate images, so overlapping
// segments never darken; highlighters get their alpha when composited.
class LiveLayer : public QQuickPaintedItem {
public:
    explicit LiveLayer(QQuickItem *parent) : QQuickPaintedItem(parent) {
        setAcceptedMouseButtons(Qt::NoButton);
        setFillColor(Qt::transparent);
        m_idle.setSingleShot(true);
        m_idle.setInterval(LayerIdleMs);
        // View-sized images are a lot to hold for nothing; once the pen has
        // rested a while, let go of the layers and, while hidden, of the
        // item's own backing image and texture (a zero-size painted item has
        // no node). Within the idle window, showing it again stays free.
        QObject::connect(&m_idle, &QTimer::timeout, this, [this] {
            if (!dirty.isEmpty())
                return;
            pen = QImage();
            highlight = QImage();
            if (!isVisible() && !m_parked) {
                m_parked = true;
                setSize(QSizeF(0, 0));
            }
        });
    }

    void setFullSize(QSizeF size) {
        m_fullSize = size;
        if (!m_parked)
            setSize(size);
    }

    void unpark() {
        if (m_parked) {
            m_parked = false;
            setSize(m_fullSize);
        }
    }

    QImage &layer(Tool tool, double dpr) {
        unpark();
        QImage &img = tool == Tool::Highlighter ? highlight : pen;
        const QSize size(int(std::ceil(width() * dpr)), int(std::ceil(height() * dpr)));
        if (img.size() != size || img.devicePixelRatio() != dpr) {
            img = QImage(size, QImage::Format_ARGB32_Premultiplied);
            img.setDevicePixelRatio(dpr);
            img.fill(Qt::transparent);
        }
        m_idle.start();
        return img;
    }

    void clear() {
        for (QImage *img : {&pen, &highlight}) {
            if (img->isNull())
                continue;
            QPainter p(img);
            p.setCompositionMode(QPainter::CompositionMode_Source);
            p.fillRect(dirty, Qt::transparent);
        }
        const QRect old = dirty.toAlignedRect();
        dirty = {};
        refresh(old);
        m_idle.start();
    }

    // An empty overlay is hidden, so the scene graph skips it entirely.
    void refresh(const QRect &rect) {
        const bool show = !dirty.isEmpty() || hoverVisible;
        if (show)
            unpark();
        if (show != isVisible()) {
            setVisible(show);
            // Coming back: repaint all of it, or pixels from before it was
            // hidden would reappear. An explicit full rect, because a bare
            // update() gets narrowed by partial ones later in the frame.
            if (show)
                update(QRect(0, 0, int(std::ceil(width())), int(std::ceil(height()))));
            return;
        }
        if (show)
            update(rect);
    }

    void paint(QPainter *p) override {
        p->setCompositionMode(QPainter::CompositionMode_Source);
        p->fillRect(QRectF(0, 0, width(), height()), Qt::transparent);
        p->setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (!highlight.isNull() && !dirty.isEmpty()) {
            p->save();
            p->setOpacity(Ink::HighlightAlpha);
            p->drawImage(QPointF(0, 0), highlight);
            p->restore();
        }
        if (!pen.isNull() && !dirty.isEmpty())
            p->drawImage(QPointF(0, 0), pen);
        if (hoverVisible) {
            p->setRenderHint(QPainter::Antialiasing);
            p->setBrush(Qt::NoBrush);
            p->setPen(QPen(QColor(255, 255, 255, 200), 3));
            p->drawEllipse(hover, hoverRadius, hoverRadius);
            QPen dash(QColor(40, 40, 46), 1.25, Qt::DashLine);
            dash.setDashPattern({3, 3});
            p->setPen(dash);
            p->drawEllipse(hover, hoverRadius, hoverRadius);
        }
    }

    QImage pen, highlight;
    QRectF dirty;
    QPointF hover;
    bool hoverVisible = false;
    double hoverRadius = 0;

    bool parked() const { return m_parked; }

private:
    QTimer m_idle;
    QSizeF m_fullSize;
    bool m_parked = false;
};

NoteView::NoteView(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents);
    setClip(true); // Pages scrolled past the edge must not cover the chrome.
    setAcceptTouchEvents(true);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);

    m_overlay = new LiveLayer(this);
    m_overlay->setZ(1);
    m_overlay->setVisible(false);

    m_zoomSettle.setSingleShot(true);
    m_zoomSettle.setInterval(140);
    connect(&m_zoomSettle, &QTimer::timeout, this, [this] {
        m_zooming = false;
        scheduleRenders();
    });
    m_scheduleTimer.setSingleShot(true);
    m_scheduleTimer.setInterval(0);
    connect(&m_scheduleTimer, &QTimer::timeout, this, &NoteView::scheduleRenders);

    connect(&m_renderer, &TileRenderer::rendered, this, &NoteView::onRendered);
    // Fingers landing on the touchpad stop a fling, before they even move.
    connect(TouchpadHold::instance(), &TouchpadHold::fingersDown, this, [this] {
        stopFling();
        m_wheelVelocity = {};
    });
    m_renderIdle.setSingleShot(true);
    m_renderIdle.setInterval(qEnvironmentVariableIntValue("OMANOTE_RENDER_IDLE_MS") > 0
                                 ? qEnvironmentVariableIntValue("OMANOTE_RENDER_IDLE_MS") : 30000);
    connect(&m_renderIdle, &QTimer::timeout, &m_renderer, &TileRenderer::releaseIdle);
    // Proximity events go to the application, not to items.
    qApp->installEventFilter(this);
}

NoteView::~NoteView() { qApp->removeEventFilter(this); }

// --- Properties -------------------------------------------------------------

void NoteView::setDocument(Document *doc) {
    if (doc == m_doc)
        return;
    if (m_doc)
        disconnect(m_doc, nullptr, this, nullptr);
    m_doc = doc;
    m_tiles.clear();
    m_stroke.reset();
    m_strokePage.reset();
    m_gesture = Gesture::None;
    m_source = Source::None;
    clearLive();
    m_renderer.setPdf(doc ? doc->source() : nullptr);
    m_lastJobs.clear();
    if (doc) {
        connect(doc, &Document::pagesChanged, this, &NoteView::syncPages);
        connect(doc, &Document::pageChanged, this, &NoteView::onPageChanged);
    }
    m_needsInitialZoom = true;
    syncPages();
    emit documentChanged();
}

void NoteView::setTool(int tool) {
    const Tool t = Tool(std::clamp(tool, 0, 2));
    if (t == m_tool)
        return;
    m_tool = t;
    setHover(m_overlay->hover, m_overlay->hoverVisible && m_tool == Tool::Eraser);
    emit toolChanged();
}

void NoteView::setBackdrop(const QColor &c) {
    if (c == m_backdrop)
        return;
    m_backdrop = c;
    update();
    emit backdropChanged();
}

bool NoteView::eraserEngaged() const { return m_eraseEngaged || m_tool == Tool::Eraser; }

void NoteView::setEraseEngaged(bool on) {
    if (on == m_eraseEngaged)
        return;
    m_eraseEngaged = on;
    emit eraserChanged();
}

QColor NoteView::activeColor(Tool tool) const { return tool == Tool::Highlighter ? m_highlightColor : m_penColor; }
double NoteView::activeWidth(Tool tool) const { return tool == Tool::Highlighter ? m_highlightWidth : m_penWidth; }

// --- Navigation -------------------------------------------------------------

void NoteView::zoomTo(double zoom, QPointF focus) {
    m_view.zoomAt(zoom, focus);
    m_fitMode = std::abs(m_view.zoom() - m_view.fitWidthZoom()) < 1e-3;
    // While zooming, existing tiles are simply scaled on the GPU; sharp ones
    // are rendered once the gesture settles.
    m_zooming = true;
    m_zoomSettle.start();
    viewMoved();
}

void NoteView::zoomIn() { zoomTo(m_view.zoom() * 1.25, {width() / 2, height() / 2}); }
void NoteView::zoomOut() { zoomTo(m_view.zoom() / 1.25, {width() / 2, height() / 2}); }
void NoteView::fitWidth() { zoomTo(m_view.fitWidthZoom(), {width() / 2, height() / 2}); }

void NoteView::toggleZoom() {
    const double fit = m_view.fitWidthZoom();
    zoomTo(std::abs(m_view.zoom() - fit) < 0.01 ? 1.0 : fit, {width() / 2, height() / 3});
}

void NoteView::goToPage(int index) {
    stopFling();
    m_view.scrollToPage(std::clamp(index, 0, std::max(0, m_view.pageCount() - 1)));
    viewMoved();
}

void NoteView::nextPage() { goToPage(currentPage() + 1); }
void NoteView::previousPage() { goToPage(currentPage() - 1); }

void NoteView::addPage() {
    if (!m_doc)
        return;
    const int index = currentPage() + 1;
    m_doc->insertPage(index);
    goToPage(index);
}

bool NoteView::clearCurrentPage() { return m_doc && m_doc->clearPage(m_doc->page(currentPage())); }

bool NoteView::currentPageHasInk() const {
    const auto page = m_doc ? m_doc->page(currentPage()) : nullptr;
    return page && page->hasInk();
}

// --- Layout -----------------------------------------------------------------

void NoteView::syncPages() {
    QList<QSizeF> sizes;
    m_indexById.clear();
    if (m_doc) {
        for (const auto &p : m_doc->pages()) {
            m_indexById.insert(p->id, sizes.size());
            sizes.append(p->size);
        }
    }
    for (auto it = m_tiles.begin(); it != m_tiles.end();)
        it = m_indexById.contains(it.key()) ? std::next(it) : m_tiles.erase(it);
    ++m_pagesVersion;
    m_view.setPages(sizes);
    if (m_needsInitialZoom && width() > 0 && !sizes.isEmpty()) {
        m_needsInitialZoom = false;
        // Fit the width, but not absurdly large on wide landscape screens.
        m_view.zoomAt(std::min(m_view.fitWidthZoom(), 1.5), {width() / 2, 0});
        m_fitMode = m_view.fitWidthZoom() <= 1.5;
        m_view.setScrollY(0);
    }
    viewMoved();
}

void NoteView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    m_overlay->setFullSize(newGeometry.size());
    const bool fitted = m_view.horizontallyLocked();
    m_view.setViewSize(newGeometry.size());
    // A page that fit before keeps fitting when a tile shrinks.
    if (!m_needsInitialZoom && (m_fitMode || (fitted && !m_view.horizontallyLocked())))
        m_view.zoomAt(std::min(m_view.fitWidthZoom(), m_fitMode ? 4.0 : m_view.zoom()), {newGeometry.width() / 2, 0});
    if (m_needsInitialZoom)
        syncPages();
    else
        viewMoved();
}

void NoteView::viewMoved() {
    trimUnusedPaper();
    update();
    // Ink waiting on tiles was drawn for the old view; it's a few ms from
    // landing in the tiles, so just drop it rather than redraw it.
    if (!m_stroke && !m_pending.isEmpty())
        clearLive();
    if (!m_zooming)
        m_scheduleTimer.start();
    // QML bindings (page counter, zoom label) only re-run on real changes,
    // not on every scrolled frame.
    if (m_view.zoom() != m_shownZoom || currentPage() != m_shownPage || pageCount() != m_shownCount) {
        m_shownZoom = m_view.zoom();
        m_shownPage = currentPage();
        m_shownCount = pageCount();
        emit viewChanged();
    }
}

// Blank paper only exists while you can see it: sheets at the end with no
// ink are dropped once scrolled out of view below, so they never count as
// pages (and a save never writes them). A sheet must sit a gap and a margin
// past the bottom edge first, so removing it can't nudge the view.
void NoteView::trimUnusedPaper() {
    if (!m_doc || m_doc->pageCount() < 2 || m_needsInitialZoom)
        return;
    const double limit = m_view.toDoc({0, height()}).y() + Viewport::Gap + Viewport::Pad / m_view.zoom();
    // Walk back from the end: only the tail can be trimmed.
    int keep = m_view.pageCount() - 1;
    while (keep > 0 && m_view.pageDocRect(keep).top() > limit)
        --keep;
    if (keep == m_view.pageCount() - 1)
        return;
    if (m_strokePage)
        keep = std::max(keep, m_doc->indexOf(m_strokePage));
    m_doc->trimPaperAfter(keep); // Re-enters via syncPages -> viewMoved; nothing left to trim then.
}

void NoteView::itemChange(ItemChange change, const ItemChangeData &value) {
    if (change == ItemSceneChange && value.window)
        // Kinetic scrolling steps once per presented frame (vsync), not on a timer.
        connect(value.window, &QQuickWindow::frameSwapped, this, &NoteView::flingFrame, Qt::QueuedConnection);
    QQuickItem::itemChange(change, value);
}

double NoteView::deviceRatio() const { return window() ? window()->effectiveDevicePixelRatio() : 1.0; }

double NoteView::targetScale() const { return std::round(m_view.zoom() * deviceRatio() * 1e4) / 1e4; }

bool NoteView::needsBackground(const Page &page) const {
    return page.source >= 0 && !page.note && m_doc && m_doc->source();
}

QRectF NoteView::snapped(QRectF r) const {
    const double d = deviceRatio();
    return {std::round(r.x() * d) / d, std::round(r.y() * d) / d, std::round(r.width() * d) / d,
            std::round(r.height() * d) / d};
}

// Grid cells of a page at a scale that fall within the view (plus margin).
QRect NoteView::tileRange(int index, double scale, double margin) const {
    const auto page = m_doc->page(index);
    const QRectF screen = m_view.pageScreenRect(index);
    const QRectF visible = QRectF(0, 0, width(), height()).adjusted(-margin, -margin, margin, margin) & screen;
    if (visible.isEmpty())
        return {};
    const double k = scale / m_view.zoom(); // Device pixels (at scale) per logical pixel.
    const QPointF a = (visible.topLeft() - screen.topLeft()) * k, b = (visible.bottomRight() - screen.topLeft()) * k;
    const QSize grid = TileRenderer::gridSize(page->size, scale);
    const int T = TileRenderer::TileSize;
    return QRect(QPoint(std::clamp(int(a.x()) / T, 0, grid.width() - 1), std::clamp(int(a.y()) / T, 0, grid.height() - 1)),
                 QPoint(std::clamp(int(std::ceil(b.x())) / T, 0, grid.width() - 1),
                        std::clamp(int(std::ceil(b.y())) / T, 0, grid.height() - 1)));
}

// --- Tiles ------------------------------------------------------------------

void NoteView::scheduleRenders() {
    if (!m_doc || m_zooming || width() <= 0)
        return;
    const double scale = targetScale();
    const double margin = height() * 0.5;
    QList<TileRenderer::Job> urgent, later;
    QSet<quint64> keep;
    bool changed = false; // Anything the scene graph must show differently?

    for (int index : m_view.visiblePages(margin)) {
        const auto page = m_doc->page(index);
        keep.insert(page->id);
        auto &pt = m_tiles[page->id];
        pt.revision = page->revision;
        const QRect wantedRange = tileRange(index, scale, margin);
        const QRect visibleRange = tileRange(index, scale, 0);
        const bool background = needsBackground(*page);

        for (int ty = wantedRange.top(); ty <= wantedRange.bottom() && !wantedRange.isEmpty(); ++ty) {
            for (int tx = wantedRange.left(); tx <= wantedRange.right(); ++tx) {
                Tile *tile = nullptr;
                for (auto &t : pt.tiles)
                    if (t.scale == scale && t.tx == tx && t.ty == ty)
                        tile = &t;
                if (!tile) {
                    Tile fresh;
                    fresh.scale = scale;
                    fresh.tx = tx;
                    fresh.ty = ty;
                    pt.tiles.append(fresh);
                    tile = &pt.tiles.last();
                    changed = true;
                }
                if (tile->revision == page->revision)
                    continue;
                // White paper with no ink here needs no texture at all.
                const QRectF area = TileRenderer::tilePoints(page->size, scale, tx, ty);
                bool inked = false;
                for (const auto &s : page->strokes)
                    if (s->bounds().intersects(area)) { inked = true; break; }
                if (!background && !inked) {
                    tile->blank = true;
                    tile->revision = page->revision;
                    tile->image = QImage();
                    ++tile->serial;
                    changed = true;
                    continue;
                }
                tile->requested = page->revision;
                TileRenderer::Job job{page->id, background ? page->source : -1, page->size, scale, tx, ty,
                                      page->revision, page->strokes};
                (visibleRange.contains(tx, ty) ? urgent : later).append(job);
            }
        }

        // Tiles at other scales stay as stand-ins until the current scale
        // covers what's visible; tiles drift out of range get dropped.
        bool covered = true;
        for (const auto &t : pt.tiles)
            if (t.scale == scale && visibleRange.contains(t.tx, t.ty) && t.revision < 0)
                covered = false;
        if (pt.tiles.removeIf([&](const Tile &t) {
                if (t.scale == scale)
                    return !wantedRange.contains(t.tx, t.ty);
                return covered;
            }))
            changed = true;
    }

    // Pages out of reach give back their tiles and stroke outlines.
    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
        if (keep.contains(it.key())) {
            ++it;
            continue;
        }
        if (const auto page = m_doc->page(m_indexById.value(it.key(), -1)))
            for (const auto &s : page->strokes)
                s->releasePath();
        it = m_tiles.erase(it);
        changed = true;
    }
    // Same wish list as last time (the usual case mid-scroll): leave the
    // workers asleep instead of waking them to find nothing new.
    const QList<TileRenderer::Job> jobs = urgent + later;
    const bool sameJobs = jobs.size() == m_lastJobs.size() &&
        std::equal(jobs.begin(), jobs.end(), m_lastJobs.begin(), [](const auto &a, const auto &b) {
            return a.page == b.page && a.tx == b.tx && a.ty == b.ty && a.scale == b.scale && a.revision == b.revision;
        });
    if (!sameJobs) {
        m_renderer.setJobs(jobs);
        m_lastJobs = jobs;
    }
    if (changed)
        update();
    maybeClearLive();
}

void NoteView::onRendered(quint64 page, double scale, int tx, int ty, int revision, const QImage &image, int generation) {
    if (generation != m_renderer.generation())
        return;
    m_renderIdle.start();
    auto it = m_tiles.find(page);
    if (it == m_tiles.end())
        return;
    for (auto &t : it->tiles) {
        if (t.scale == scale && t.tx == tx && t.ty == ty) {
            if (revision < t.revision)
                return;
            t.image = image;
            t.revision = revision;
            t.blank = false;
            ++t.serial;
            update();
            maybeClearLive();
            if (revision != it->revision || !m_zooming)
                m_scheduleTimer.start(); // Retire stand-ins, or catch up.
            return;
        }
    }
}

void NoteView::onPageChanged(quint64 id, const QRectF &region) {
    auto it = m_tiles.find(id);
    const auto pageIt = std::find_if(m_doc->pages().begin(), m_doc->pages().end(),
                                     [id](const PagePtr &p) { return p->id == id; });
    if (it != m_tiles.end() && pageIt != m_doc->pages().end()) {
        // Tiles the change didn't touch are still exactly right.
        const int now = (*pageIt)->revision;
        const QRectF touched = region.adjusted(-1, -1, 1, 1);
        for (auto &t : it->tiles)
            if (t.revision == it->revision &&
                !TileRenderer::tilePoints((*pageIt)->size, t.scale, t.tx, t.ty).intersects(touched))
                t.revision = now;
        it->revision = now;
    }
    m_scheduleTimer.start();
}

void NoteView::resetTiles() {
    m_tiles.clear();
    m_lastJobs.clear();
    scheduleRenders();
}

bool NoteView::tilesCurrent() const {
    if (!m_doc)
        return true;
    const double scale = targetScale();
    for (int index : m_view.visiblePages(0)) {
        const auto page = m_doc->page(index);
        const QRect range = tileRange(index, scale, 0);
        const auto pt = m_tiles.value(page->id);
        for (int ty = range.top(); ty <= range.bottom() && !range.isEmpty(); ++ty)
            for (int tx = range.left(); tx <= range.right(); ++tx) {
                bool ok = false;
                for (const auto &t : pt.tiles)
                    if (t.scale == scale && t.tx == tx && t.ty == ty && t.revision == page->revision)
                        ok = true;
                if (!ok)
                    return false;
            }
    }
    return true;
}

void NoteView::releaseResources() {
    // The scene graph (and every texture) is going away; tiles must be
    // rendered again for the next one.
    m_nodes.clear();
    m_pageNodes.clear();
    m_backdropNode = nullptr;
    m_contentNode = nullptr;
    QMetaObject::invokeMethod(this, &NoteView::resetTiles, Qt::QueuedConnection);
}

QSGNode *NoteView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    // Containers are (identity) transform nodes, not plain QSGNodes: Qt's
    // software renderer only tracks inherited transform and clip for those,
    // and children added later under a plain node would lose the item's
    // position.
    QSGNode *root = old;
    if (!root) {
        root = new QSGTransformNode;
        m_nodes.clear();
        m_pageNodes.clear();
        m_backdropNode = window()->createRectangleNode();
        root->appendChildNode(m_backdropNode);
        m_contentNode = new QSGTransformNode;
        root->appendChildNode(m_contentNode);
        ++m_layoutStamp;
    }
    m_backdropNode->setRect(boundingRect());
    m_backdropNode->setColor(m_backdrop);

    // Content coordinates are the screen at zero pan and scroll; scrolling is
    // just this one translation, snapped to device pixels.
    const double dpr = deviceRatio();
    const QPointF offset(std::round(m_view.panX() * dpr) / dpr, std::round(-m_view.scrollY() * dpr) / dpr);
    QTransform shift;
    shift.translate(offset.x(), offset.y());
    m_contentNode->setMatrix(shift);
    const LayoutKey key{m_view.zoom(), width(), dpr, m_pagesVersion};
    if (key.zoom != m_layoutKey.zoom || key.width != m_layoutKey.width || key.dpr != m_layoutKey.dpr ||
        key.pages != m_layoutKey.pages) {
        m_layoutKey = key;
        ++m_layoutStamp;
    }

    struct Upload { Tile *tile; NodeEntry *entry; QRectF rect; QSGNode *parent; int priority; };
    QList<Upload> uploads;
    auto place = [](NodeEntry &e, const QRectF &rect, QSGNode *parent, quint64 stamp) {
        if (e.node->parent() != parent) {
            if (e.node->parent())
                e.node->parent()->removeChildNode(e.node);
            parent->appendChildNode(e.node);
        }
        if (e.laidOut != stamp) {
            // The source matches the visible part (rect may be clamped at the
            // sheet's edge), so the image is never squeezed.
            const QSizeF tex = e.node->texture()->textureSize();
            e.node->setRect(rect);
            e.node->setSourceRect(QRectF(0, 0, std::min(tex.width(), rect.width() * e.pxPerUnit),
                                         std::min(tex.height(), rect.height() * e.pxPerUnit)));
            e.laidOut = stamp;
        }
    };

    QSet<NodeKey> used;
    QSet<quint64> livePages;
    bool lost = false;
    if (m_doc) {
        const double scale = targetScale();
        const QRectF view = boundingRect();
        // Only pages on screen or still holding tiles, never the whole document.
        QList<int> indices = m_view.visiblePages(8);
        for (auto it = m_tiles.begin(); it != m_tiles.end(); ++it) {
            const int i = m_indexById.value(it.key(), -1);
            if (i >= 0 && !indices.contains(i))
                indices.append(i);
        }
        for (int index : indices) {
            const auto page = m_doc->page(index);
            if (!page)
                continue;
            const QRectF r = snapped(m_view.pageScreenRect(index).translated(-m_view.panX(), m_view.scrollY()));
            livePages.insert(page->id);
            PageNode &pn = m_pageNodes[page->id];
            if (!pn.group) {
                pn.group = new QSGTransformNode;
                pn.shadow = makeShadow();
                pn.paper = window()->createRectangleNode();
                pn.paper->setColor(Qt::white);
                pn.standIns = new QSGTransformNode;
                pn.current = new QSGTransformNode;
                pn.group->appendChildNode(pn.shadow);
                pn.group->appendChildNode(pn.paper);
                pn.group->appendChildNode(pn.standIns);
                pn.group->appendChildNode(pn.current);
                m_contentNode->appendChildNode(pn.group);
            }
            if (pn.laidOut != m_layoutStamp) {
                layoutShadow(pn.shadow->geometry(), r);
                pn.shadow->markDirty(QSGNode::DirtyGeometry);
                pn.paper->setRect(r);
                pn.laidOut = m_layoutStamp;
            }
            auto pt = m_tiles.find(page->id);
            if (pt == m_tiles.end())
                continue;

            for (auto &t : pt->tiles) {
                if (t.blank || t.revision < 0)
                    continue;
                const NodeKey nodeKey{page->id, qint64(std::llround(t.scale * 1e4)), t.tx, t.ty};
                NodeEntry &entry = m_nodes[nodeKey];
                if (!entry.node) {
                    entry.node = window()->createImageNode();
                    entry.node->setOwnsTexture(true);
                    entry.node->setFiltering(QSGTexture::Linear);
                }
                const QRect px = TileRenderer::tilePixels(page->size, t.scale, t.tx, t.ty);
                const double f = m_view.zoom() / t.scale;
                // Edge tiles round up to whole pixels; clamp to the sheet so
                // none pokes a hairline past the paper.
                const QRectF rect = QRectF(r.x() + px.x() * f, r.y() + px.y() * f, px.width() * f, px.height() * f) & r;
                QSGNode *parent = t.scale == scale ? pn.current : pn.standIns;
                entry.pxPerUnit = 1.0 / f;
                used.insert(nodeKey);
                if (entry.serial != t.serial) {
                    if (t.image.isNull()) {
                        lost = true; // Shouldn't happen; re-render to be safe.
                    } else {
                        const bool onScreen = t.scale == scale && rect.translated(offset).intersects(view);
                        uploads.append({&t, &entry, rect, parent, onScreen ? 0 : 1});
                    }
                }
                if (entry.node->texture())
                    place(entry, rect, parent, m_layoutStamp); // Old texture shows until the new one lands.
            }
        }
    }

    // Uploads are spread over frames, visible tiles first, so a zoom that
    // settles with a screenful of new tiles never stalls one frame.
    std::stable_sort(uploads.begin(), uploads.end(),
                     [](const Upload &a, const Upload &b) { return a.priority < b.priority; });
    for (int i = 0; i < uploads.size(); ++i) {
        if (i == UploadsPerFrame) {
            QMetaObject::invokeMethod(this, [this] { update(); }, Qt::QueuedConnection);
            break;
        }
        Upload &u = uploads[i];
        // The node owns its texture and frees the previous one.
        u.entry->node->setTexture(window()->createTextureFromImage(u.tile->image, QQuickWindow::TextureIsOpaque));
        u.entry->serial = u.tile->serial;
        u.tile->image = QImage(); // Uploaded: the GPU copy is the one that counts now.
        u.entry->laidOut = 0;
        place(*u.entry, u.rect, u.parent, m_layoutStamp);
    }
    if (lost)
        QMetaObject::invokeMethod(this, &NoteView::resetTiles, Qt::QueuedConnection);

    for (auto it = m_nodes.begin(); it != m_nodes.end();) {
        if (used.contains(it.key())) {
            ++it;
            continue;
        }
        if (it->node->parent())
            it->node->parent()->removeChildNode(it->node);
        delete it->node;
        it = m_nodes.erase(it);
    }
    for (auto it = m_pageNodes.begin(); it != m_pageNodes.end();) {
        if (livePages.contains(it.key())) {
            ++it;
            continue;
        }
        m_contentNode->removeChildNode(it->group);
        delete it->group; // Shadow, paper and the empty tile groups go with it.
        it = m_pageNodes.erase(it);
    }
    return root;
}

// --- Live layer -------------------------------------------------------------

QRectF NoteView::strokeScreenRect(int from) const {
    if (!m_stroke || !m_doc || m_stroke->samples.isEmpty())
        return {};
    const int index = m_doc->indexOf(m_strokePage);
    QRectF r;
    for (int i = std::max(0, from); i < m_stroke->samples.size(); ++i) {
        const QPointF pt = m_stroke->point(i);
        r = r.isNull() ? QRectF(pt, QSizeF(0.01, 0.01)) : r.united(QRectF(pt, QSizeF(0.01, 0.01)));
    }
    const double reach = Ink::radiusFor(m_stroke->tool, m_stroke->width, 1.0) + 1;
    r.adjust(-reach, -reach, reach, reach);
    const QRectF page = m_view.pageScreenRect(index);
    return QRectF(page.topLeft() + r.topLeft() * m_view.zoom(), r.size() * m_view.zoom()).adjusted(-2, -2, 2, 2);
}

void NoteView::drawLive(int from) {
    if (!m_stroke || !m_doc)
        return;
    const int index = m_doc->indexOf(m_strokePage);
    if (index < 0)
        return;
    const QRectF page = snapped(m_view.pageScreenRect(index));
    if (m_liveZoom != m_view.zoom() || m_liveOrigin != page.topLeft()) {
        // The view moved under the layer: anything on it is stale.
        clearLive();
        m_liveZoom = m_view.zoom();
        m_liveOrigin = page.topLeft();
        from = 0;
    }
    // Redraw only the last few segments: the spline reshapes the previous
    // segment as each sample arrives, and opaque overdraw is harmless.
    Stroke tail;
    tail.tool = m_stroke->tool;
    tail.width = m_stroke->width;
    tail.samples = m_stroke->samples.mid(std::max(0, from));
    QColor c = m_stroke->color;
    c.setAlpha(255);
    QImage &img = m_overlay->layer(m_stroke->tool, deviceRatio());
    QPainter lp(&img);
    lp.setRenderHint(QPainter::Antialiasing);
    lp.setClipRect(page);
    lp.translate(page.topLeft());
    lp.scale(m_view.zoom(), m_view.zoom());
    lp.setPen(Qt::NoPen);
    lp.setBrush(c);
    lp.drawPath(*tail.path());
    const QRectF b = tail.bounds();
    const QRectF screen = QRectF(page.topLeft() + b.topLeft() * m_view.zoom(), b.size() * m_view.zoom())
                              .adjusted(-2, -2, 2, 2) & boundingRect();
    m_overlay->dirty |= screen;
    m_overlay->refresh(screen.toAlignedRect());
}

void NoteView::clearLive() {
    m_pending.clear();
    m_liveZoom = 0;
    if (m_overlay && !m_overlay->dirty.isEmpty())
        m_overlay->clear();
}

// Committed ink leaves the layer once the tiles showing it have arrived.
void NoteView::maybeClearLive() {
    if (m_stroke || m_pending.isEmpty() || !m_doc)
        return;
    const double scale = targetScale();
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        const int index = m_indexById.value(it.key(), -1);
        if (index < 0)
            continue;
        const QRect range = tileRange(index, scale, 0);
        const auto pt = m_tiles.value(it.key());
        for (int ty = range.top(); ty <= range.bottom() && !range.isEmpty(); ++ty)
            for (int tx = range.left(); tx <= range.right(); ++tx) {
                bool ok = false;
                for (const auto &t : pt.tiles)
                    if (t.scale == scale && t.tx == tx && t.ty == ty && t.revision >= it.value())
                        ok = true;
                if (!ok)
                    return;
            }
    }
    clearLive();
}

void NoteView::setHover(QPointF pos, bool visible) {
    const double r = Ink::EraserRadius * m_view.zoom();
    const double pad = r + 4;
    if (m_overlay->hoverVisible)
        m_overlay->refresh(QRectF(m_overlay->hover - QPointF(pad, pad), QSizeF(2 * pad, 2 * pad)).toAlignedRect());
    m_overlay->hover = pos;
    m_overlay->hoverVisible = visible;
    m_overlay->hoverRadius = r;
    m_overlay->refresh(QRectF(pos - QPointF(pad, pad), QSizeF(2 * pad, 2 * pad)).toAlignedRect());
}

// --- Ink --------------------------------------------------------------------

void NoteView::beginStroke(QPointF pos, double pressure, bool erase, Source source) {
    if (!m_doc)
        return;
    stopFling();
    emit interacted();
    QPointF local;
    const int index = m_view.pageAt(pos, &local);
    if (index < 0) {
        // Margins and gaps scroll; ink only starts on paper.
        beginPan(pos, source == Source::Stylus ? Gesture::VerticalPan : Gesture::Pan, source);
        return;
    }
    m_source = source;
    m_last = pos;
    if (erase || m_tool == Tool::Eraser) {
        m_gesture = Gesture::Erase;
        m_doc->beginErase();
        eraseAlong(pos, pos);
        return;
    }
    m_gesture = Gesture::Ink;
    m_strokePage = m_doc->page(index);
    m_stroke = std::make_shared<Stroke>();
    m_stroke->id = Document::newId();
    m_stroke->tool = m_tool;
    m_stroke->color = activeColor(m_tool);
    m_stroke->width = float(activeWidth(m_tool));
    m_smoothedPressure = pressure;
    m_stroke->samples.append({float(local.x()), float(local.y()), float(pressure)});
    drawLive(0);
}

void NoteView::extendStroke(QPointF pos, double pressure) {
    if (m_gesture == Gesture::Erase) {
        eraseAlong(m_last, pos);
        m_last = pos;
        return;
    }
    if (m_gesture != Gesture::Ink || !m_stroke)
        return;
    const int index = m_doc->indexOf(m_strokePage);
    if (index < 0)
        return;
    const QPointF local = m_view.toPage(index, pos);
    const auto &last = m_stroke->samples.last();
    // Light low-pass on pressure: sensors are noisy, ink shouldn't wobble.
    m_smoothedPressure = 0.6 * m_smoothedPressure + 0.4 * pressure;
    if (length(local - QPointF(last.x, last.y)) < 0.25 / m_view.zoom())
        return;
    m_stroke->samples.append({float(local.x()), float(local.y()), float(m_smoothedPressure)});
    drawLive(m_stroke->samples.size() - 4);
    m_last = pos;
}

void NoteView::endStroke() {
    if (m_gesture == Gesture::Ink && m_stroke && m_doc) {
        auto stroke = m_stroke;
        auto page = m_strokePage;
        m_stroke.reset();
        m_strokePage.reset();
        stroke->samples = Ink::simplify(stroke->samples, stroke->tool, stroke->width);
        stroke->invalidate();
        m_doc->addStroke(page, stroke);
        // The layer keeps showing it until the tiles catch up.
        m_pending[page->id] = page->revision;
    } else if (m_gesture == Gesture::Erase && m_doc) {
        m_doc->endErase();
    }
    m_gesture = Gesture::None;
    m_source = Source::None;
}

void NoteView::eraseAlong(QPointF from, QPointF to) {
    const QPointF a = m_view.toDoc(from), b = m_view.toDoc(to);
    const double r = Ink::EraserRadius;
    const QRectF sweep = QRectF(a, b).normalized().adjusted(-r, -r, r, r);
    for (int i : m_view.visiblePages(0)) {
        const QRectF pr = m_view.pageDocRect(i);
        if (!pr.intersects(sweep))
            continue;
        const auto page = m_doc->page(i);
        const QList<StrokePtr> strokes = page->strokes;
        for (const auto &s : strokes)
            if (Ink::hits(*s, a - pr.topLeft(), b - pr.topLeft(), r))
                m_doc->eraseLive(page, s);
    }
    setHover(to, true);
}

// --- Pan --------------------------------------------------------------------

void NoteView::beginPan(QPointF pos, Gesture kind, Source source) {
    stopFling();
    m_gesture = kind;
    m_source = source;
    m_last = pos;
    m_panVelocity = {};
    m_panClock.restart();
}

void NoteView::panTo(QPointF pos) {
    QPointF d = pos - m_last;
    if (m_gesture == Gesture::VerticalPan)
        d.setX(0);
    scrollBy(d.x(), d.y());
    const double dt = std::max<qint64>(1, m_panClock.restart());
    m_panVelocity = 0.7 * (d / double(dt)) + 0.3 * m_panVelocity;
    m_last = pos;
    viewMoved();
}

void NoteView::endPan() {
    // A finger or the pen in the margin carries on with its momentum when
    // lifted mid-swipe; a mouse drag stops where it's let go.
    if ((m_source == Source::Touch || m_source == Source::Stylus) && m_panClock.elapsed() < 60)
        startFling(m_panVelocity);
    m_gesture = Gesture::None;
    m_source = Source::None;
}

void NoteView::startFling(QPointF velocity) {
    if (length(velocity) <= 0.3)
        return;
    m_flingVelocity = velocity;
    m_flinging = true;
    m_flingClock.restart();
    update(); // Frames drive the fling from here.
}

// Every user scroll goes through here. Pushing past the end of a notebook
// adds a fresh sheet, so there's always paper below; returns whether it did.
bool NoteView::scrollBy(double dx, double dy) {
    const double before = m_view.scrollY();
    m_view.panBy(dx, dy);
    const bool atEnd = m_view.scrollY() >= m_view.maxScrollY() - 0.5;
    // Only a deliberate push past the end counts, not jitter of a drag along
    // the margin of a page that already fits on screen.
    m_overscroll = (dy < 0 && atEnd) ? m_overscroll + (-dy - (m_view.scrollY() - before)) : 0;
    if (m_overscroll > 48 && m_doc && m_doc->pageCount() > 0 && m_doc->pageCount() < 2000 &&
        m_doc->pages().last()->note) {
        m_overscroll = 0;
        m_doc->appendPaper();
        return true;
    }
    return false;
}

// --- Input ------------------------------------------------------------------

bool NoteView::touchBlocked() const {
    if (m_stylusDown)
        return true;
    if (!m_stylusClock.isValid())
        return false;
    const qint64 since = m_stylusClock.elapsed();
    return since < PalmGraceMs || (m_stylusNear && since < HoverTrustMs);
}

// Is this view the topmost item under a scene point (no chrome above it)?
bool NoteView::hitsMe(QPointF scenePos) const {
    if (!window())
        return false;
    QQuickItem *item = window()->contentItem();
    while (item) {
        QQuickItem *child = item->childAt(item->mapFromScene(scenePos).x(), item->mapFromScene(scenePos).y());
        if (!child)
            break;
        item = child;
    }
    return item == this || item == m_overlay;
}

bool NoteView::eventFilter(QObject *watched, QEvent *event) {
    // Qt Quick hands pen events only to pointer handlers and otherwise turns
    // them into mouse events, losing pressure and the eraser end. Take them
    // at the window instead, whenever the pen is over the paper (not chrome)
    // or already drawing.
    const auto type = event->type();
    if ((type == QEvent::TabletPress || type == QEvent::TabletMove || type == QEvent::TabletRelease) &&
        watched == window() && isVisible() && m_doc) {
        auto *e = static_cast<QTabletEvent *>(event);
        const bool mine = m_source == Source::Stylus || hitsMe(e->scenePosition());
        if (!mine) {
            if (m_overlay->hoverVisible && !m_stylusDown)
                setHover(m_overlay->hover, false);
            return false;
        }
        tabletEvent(e, mapFromScene(e->scenePosition()));
        return true;
    }
    if (type == QEvent::TabletEnterProximity) {
        m_stylusNear = true;
        m_stylusClock.restart();
    } else if (event->type() == QEvent::TabletLeaveProximity) {
        m_stylusNear = false;
        m_stylusClock.restart();
        setHover(m_overlay->hover, false);
        setEraseEngaged(false);
    }
    return QQuickItem::eventFilter(watched, event);
}

bool NoteView::event(QEvent *event) {
    switch (event->type()) {
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        tabletEvent(static_cast<QTabletEvent *>(event), static_cast<QTabletEvent *>(event)->position());
        return true;
    case QEvent::NativeGesture: {
        auto *g = static_cast<QNativeGestureEvent *>(event);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomTo(m_view.zoom() * (1 + g->value()), g->position());
            g->accept();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QQuickItem::event(event);
}

void NoteView::tabletEvent(QTabletEvent *e, QPointF pos) {
    // Accepting keeps Qt from synthesizing a second, mouse copy of the pen.
    e->accept();
    m_stylusNear = true;
    m_stylusClock.restart();
    const bool erase = e->pointerType() == QPointingDevice::PointerType::Eraser ||
                       (e->buttons() & (Qt::MiddleButton | Qt::RightButton));
    const bool hasPressure = e->pointingDevice() &&
                             e->pointingDevice()->capabilities().testFlag(QInputDevice::Capability::Pressure);
    const double pressure = hasPressure ? std::clamp(e->pressure(), 0.0, 1.0) : 0.5;

    switch (e->type()) {
    case QEvent::TabletPress:
        if (m_source == Source::Touch || m_source == Source::Mouse) {
            m_gesture = Gesture::None; // The pen wins over a resting palm.
            m_source = Source::None;
        }
        stopFling();
        m_stylusDown = true;
        setEraseEngaged(erase);
        beginStroke(pos, pressure, erase, Source::Stylus);
        break;
    case QEvent::TabletMove:
        setEraseEngaged(erase);
        if (m_stylusDown && m_source == Source::Stylus) {
            // Barrel button or flipping mid-stroke switches tools on the spot.
            const bool wantErase = erase || m_tool == Tool::Eraser;
            if ((m_gesture == Gesture::Ink && wantErase) || (m_gesture == Gesture::Erase && !wantErase)) {
                endStroke();
                beginStroke(pos, pressure, erase, Source::Stylus);
            } else if (m_gesture == Gesture::Ink || m_gesture == Gesture::Erase) {
                extendStroke(pos, pressure);
            } else if (m_gesture == Gesture::VerticalPan) {
                panTo(pos);
            }
        }
        setHover(pos, erase || m_tool == Tool::Eraser);
        break;
    case QEvent::TabletRelease:
        if (m_source == Source::Stylus) {
            if (m_gesture == Gesture::Ink || m_gesture == Gesture::Erase)
                endStroke();
            else
                endPan();
        }
        m_stylusDown = false;
        setEraseEngaged(e->pointerType() == QPointingDevice::PointerType::Eraser);
        break;
    default:
        break;
    }
}

void NoteView::mousePressEvent(QMouseEvent *e) {
    e->accept();
    if (!realMouse(e) || m_stylusDown || m_source != Source::None)
        return;
    if (e->button() == Qt::LeftButton)
        beginStroke(e->position(), 0.5, false, Source::Mouse);
    else if (e->button() == Qt::MiddleButton)
        beginPan(e->position(), Gesture::Pan, Source::Mouse);
}

void NoteView::mouseMoveEvent(QMouseEvent *e) {
    e->accept();
    if (!realMouse(e) || m_source != Source::Mouse)
        return;
    if (m_gesture == Gesture::Ink || m_gesture == Gesture::Erase)
        extendStroke(e->position(), 0.5);
    else if (m_gesture == Gesture::Pan)
        panTo(e->position());
}

void NoteView::mouseReleaseEvent(QMouseEvent *e) {
    e->accept();
    if (!realMouse(e) || m_source != Source::Mouse)
        return;
    if (m_gesture == Gesture::Ink || m_gesture == Gesture::Erase)
        endStroke();
    else
        endPan();
}

void NoteView::mouseUngrabEvent() {
    if (m_source == Source::Mouse) {
        if (m_gesture == Gesture::Ink || m_gesture == Gesture::Erase)
            endStroke();
        m_gesture = Gesture::None;
        m_source = Source::None;
    }
}

void NoteView::hoverMoveEvent(QHoverEvent *e) {
    TouchpadHold::instance()->ensure(); // Free once bound; needs a pointer to exist.
    if (realMouse(e))
        setHover(e->position(), m_tool == Tool::Eraser);
}

void NoteView::hoverLeaveEvent(QHoverEvent *) { setHover(m_overlay->hover, false); }

void NoteView::wheelEvent(QWheelEvent *e) {
    e->accept();
    TouchpadHold::instance()->ensure();
    emit interacted();
    if (e->modifiers() & Qt::ControlModifier) {
        stopFling();
        const double steps = e->angleDelta().y() / 120.0;
        zoomTo(m_view.zoom() * std::pow(1.15, steps), e->position());
        return;
    }
    // Touchpads report scroll phases: fingers down (begin), moving (update),
    // lifted (end). Track the swipe's velocity and fling on lift, exactly
    // like a finger on the screen. Mouse wheels (no phase) just step.
    switch (e->phase()) {
    case Qt::ScrollBegin:
        stopFling();
        m_wheelVelocity = {};
        m_wheelClock.restart();
        break;
    case Qt::ScrollEnd:
        if (m_wheelClock.isValid() && m_wheelClock.elapsed() < 80)
            startFling(m_wheelVelocity);
        m_wheelVelocity = {};
        return;
    case Qt::ScrollMomentum:
        break; // The platform already supplies momentum (macOS); don't add ours.
    default:
        stopFling();
        break;
    }
    QPointF d = !e->pixelDelta().isNull() ? QPointF(e->pixelDelta()) : QPointF(e->angleDelta()) / 120.0 * 60.0;
    if ((e->modifiers() & Qt::ShiftModifier) && d.x() == 0)
        d = {d.y(), 0};
    if (e->phase() == Qt::ScrollUpdate) {
        const double dt = std::max<qint64>(1, m_wheelClock.restart());
        m_wheelVelocity = 0.6 * (d / dt) + 0.4 * m_wheelVelocity;
    }
    scrollBy(d.x(), d.y());
    viewMoved();
}

void NoteView::touchEvent(QTouchEvent *e) {
    e->accept();
    if (e->type() == QEvent::TouchCancel) {
        touchUngrabEvent();
        return;
    }
    QList<QPointF> active;
    for (const auto &pt : e->points())
        if (pt.state() != QEventPoint::Released)
            active.append(pt.position());
    const bool allUp = active.isEmpty() || e->type() == QEvent::TouchEnd;

    // Palm rejection: a hand resting while the pen writes never scrolls.
    if (touchBlocked() || m_source == Source::Stylus || m_source == Source::Mouse || m_touchIgnored) {
        if (m_source == Source::Touch) {
            m_gesture = Gesture::None;
            m_source = Source::None;
        }
        m_touchIgnored = !allUp;
        return;
    }
    if (allUp) {
        if (m_gesture == Gesture::Pan)
            endPan();
        m_gesture = Gesture::None;
        m_source = Source::None;
        return;
    }
    if (active.size() >= 2) {
        const QPointF c = (active[0] + active[1]) / 2;
        const double d = std::max(1.0, length(active[0] - active[1]));
        if (m_gesture != Gesture::Pinch) {
            stopFling();
            emit interacted();
            m_gesture = Gesture::Pinch;
            m_source = Source::Touch;
            m_pinchStartZoom = m_view.zoom();
            m_pinchStartDistance = d;
            m_pinchCentroid = c;
            return;
        }
        m_view.panBy(c.x() - m_pinchCentroid.x(), c.y() - m_pinchCentroid.y());
        m_pinchCentroid = c;
        zoomTo(m_pinchStartZoom * d / m_pinchStartDistance, c);
        return;
    }
    // One finger only ever scrolls, never draws.
    if (m_gesture == Gesture::Pan)
        panTo(active[0]);
    else {
        if (m_gesture == Gesture::None)
            emit interacted();
        beginPan(active[0], Gesture::Pan, Source::Touch);
    }
}

void NoteView::touchUngrabEvent() {
    if (m_source == Source::Touch) {
        m_gesture = Gesture::None;
        m_source = Source::None;
    }
    m_touchIgnored = false;
}

void NoteView::flingFrame() {
    if (!m_flinging)
        return;
    const double dt = std::clamp<double>(m_flingClock.restart(), 1, 50);
    const double y = m_view.scrollY(), x = m_view.panX();
    const bool grew = scrollBy(m_flingVelocity.x() * dt, m_flingVelocity.y() * dt);
    // Same decay per millisecond whatever the refresh rate.
    m_flingVelocity *= std::pow(0.94, dt / 16.0);
    // A fling into the end of a notebook rolls on into the fresh sheet.
    if (length(m_flingVelocity) < 0.02 || (!grew && y == m_view.scrollY() && x == m_view.panX()))
        m_flinging = false;
    viewMoved();
}
