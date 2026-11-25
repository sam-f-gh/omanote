#include "noteview.h"

#include <QGuiApplication>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QQuickWindow>
#include <QTabletEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
// Page caches above this many pixels fall back to a capped background with
// ink drawn as vectors, so 400 % zoom on a big page can't eat a gigabyte.
constexpr double MaxPagePixels = 16e6;
constexpr int PalmGraceMs = 200;
constexpr int HoverTrustMs = 1500;

bool realMouse(const QPointerEvent *e) {
    const auto type = e->device() ? e->device()->type() : QInputDevice::DeviceType::Mouse;
    return type == QInputDevice::DeviceType::Mouse || type == QInputDevice::DeviceType::TouchPad;
}

double length(QPointF p) { return std::hypot(p.x(), p.y()); }
}

NoteView::NoteView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setOpaquePainting(true);
    setAcceptTouchEvents(true);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    setFlag(ItemIsFocusScope, false);

    m_zoomSettle.setSingleShot(true);
    m_zoomSettle.setInterval(140);
    connect(&m_zoomSettle, &QTimer::timeout, this, [this] {
        m_zooming = false;
        scheduleRenders();
        update();
    });

    m_fling.setInterval(16);
    connect(&m_fling, &QTimer::timeout, this, [this] {
        const double y = m_view.scrollY(), x = m_view.panX();
        m_view.panBy(m_flingVelocity.x() * 16, m_flingVelocity.y() * 16);
        m_flingVelocity *= 0.94;
        if (length(m_flingVelocity) < 0.02 || (y == m_view.scrollY() && x == m_view.panX()))
            m_fling.stop();
        viewMoved();
    });

    connect(&m_renderer, &PageRenderer::rendered, this, &NoteView::onRendered);
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
    m_cache.clear();
    m_stroke.reset();
    m_strokePage.reset();
    m_gesture = Gesture::None;
    m_source = Source::None;
    m_renderer.setPdf(doc ? doc->sourcePdf() : QByteArray());
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
    setHover(m_hover, m_hoverVisible && m_tool == Tool::Eraser);
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
    m_fling.stop();
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

// --- Layout & caches --------------------------------------------------------

void NoteView::syncPages() {
    QList<QSizeF> sizes;
    QSet<quint64> ids;
    if (m_doc) {
        for (const auto &p : m_doc->pages()) {
            sizes.append(p->size);
            ids.insert(p->id);
        }
    }
    for (auto it = m_cache.begin(); it != m_cache.end();)
        it = ids.contains(it.key()) ? std::next(it) : m_cache.erase(it);
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
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
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
    update();
    if (!m_zooming)
        scheduleRenders();
    emit viewChanged();
}

double NoteView::deviceRatio() const { return window() ? window()->effectiveDevicePixelRatio() : 1.0; }

bool NoteView::needsBackground(const Page &page) const {
    return page.source >= 0 && !page.note && m_doc && !m_doc->sourcePdf().isEmpty();
}

double NoteView::backgroundScaleFor(const Page &page) const {
    const double cap = std::sqrt(MaxPagePixels / std::max(1.0, page.size.width() * page.size.height()));
    return std::min(m_view.zoom() * deviceRatio(), cap);
}

void NoteView::scheduleRenders() {
    if (!m_doc)
        return;
    QList<PageRenderer::Job> jobs;
    QList<int> order = m_view.visiblePages(0);
    // One screen above and below, so scrolling finds pages ready.
    for (int i : m_view.visiblePages(height()))
        if (!order.contains(i))
            order.append(i);
    QSet<quint64> keep;
    for (int i : order) {
        const auto page = m_doc->page(i);
        if (!page)
            continue;
        keep.insert(page->id);
        if (!needsBackground(*page))
            continue;
        const double target = backgroundScaleFor(*page);
        if (std::abs(m_cache[page->id].backgroundScale - target) > 1e-6)
            jobs.append({page->id, page->source, target});
    }
    // Pages far away give their memory back.
    for (auto it = m_cache.begin(); it != m_cache.end();)
        it = keep.contains(it.key()) ? std::next(it) : m_cache.erase(it);
    m_renderer.setJobs(jobs);
}

void NoteView::onRendered(quint64 key, double scale, const QImage &image, int generation) {
    if (generation != m_renderer.generation() || !m_doc)
        return;
    for (const auto &p : m_doc->pages()) {
        if (p->id == key) {
            auto &c = m_cache[key];
            c.background = image;
            c.backgroundScale = scale;
            update();
            return;
        }
    }
}

void NoteView::onPageChanged(quint64) { update(); }

void NoteView::buildComposite(const Page &page, Cache &cache, double scale) {
    const QSize size(int(std::ceil(page.size.width() * scale)), int(std::ceil(page.size.height() * scale)));
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    QPainter p(&img);
    if (!cache.background.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRectF(QPointF(0, 0), QSizeF(size)), cache.background);
    }
    p.scale(scale, scale);
    p.setClipRect(QRectF(QPointF(0, 0), page.size));
    Ink::paint(p, page.strokes);
    p.end();
    img.setDevicePixelRatio(deviceRatio());
    cache.composite = img;
    cache.compositeScale = scale;
    cache.compositeRevision = page.revision;
    cache.compositeBackground = cache.backgroundScale;
}

QRectF NoteView::snapped(QRectF r) const {
    const double d = deviceRatio();
    return {std::round(r.x() * d) / d, std::round(r.y() * d) / d, std::round(r.width() * d) / d,
            std::round(r.height() * d) / d};
}

// --- Painting ---------------------------------------------------------------

void NoteView::paint(QPainter *p) {
    p->fillRect(QRectF(0, 0, width(), height()), m_backdrop);
    if (!m_doc)
        return;
    const QRectF clip = p->clipBoundingRect().isEmpty() ? QRectF(0, 0, width(), height()) : p->clipBoundingRect();
    for (int i : m_view.visiblePages(8)) {
        const QRectF rect = snapped(m_view.pageScreenRect(i));
        if (rect.adjusted(-8, -8, 8, 8).intersects(clip))
            paintPage(*p, i, rect);
    }
    if (m_hoverVisible) {
        const double r = Ink::EraserRadius * m_view.zoom();
        p->setRenderHint(QPainter::Antialiasing);
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(QColor(255, 255, 255, 200), 3));
        p->drawEllipse(m_hover, r, r);
        QPen dash(QColor(40, 40, 46), 1.25, Qt::DashLine);
        dash.setDashPattern({3, 3});
        p->setPen(dash);
        p->drawEllipse(m_hover, r, r);
    }
}

void NoteView::paintPage(QPainter &p, int index, const QRectF &rect) {
    const auto page = m_doc->page(index);
    if (!page)
        return;

    // Soft shadow and hairline border keep white sheets distinct on any theme.
    p.setPen(Qt::NoPen);
    for (int k = 1; k <= 4; ++k)
        p.fillRect(rect.adjusted(-k, -k + 2, k, k + 2), QColor(0, 0, 0, 14 - k * 2));
    p.fillRect(rect.adjusted(-1, -1, 1, 1), QColor(0, 0, 0, 40));
    p.fillRect(rect, Qt::white);

    auto &cache = m_cache[page->id];
    const double scale = m_view.zoom() * deviceRatio();
    const double cap = std::sqrt(MaxPagePixels / std::max(1.0, page->size.width() * page->size.height()));
    const bool needBg = needsBackground(*page);
    bool drewInk = false;

    if (scale <= cap) {
        const bool valid = !cache.composite.isNull() && std::abs(cache.compositeScale - scale) < 1e-6 &&
                           cache.compositeRevision == page->revision &&
                           cache.compositeBackground == cache.backgroundScale;
        if (!valid && !m_zooming && (!needBg || cache.backgroundScale > 0))
            buildComposite(*page, cache, scale);
        if (!cache.composite.isNull()) {
            if (std::abs(cache.compositeScale - scale) < 1e-6) {
                p.drawImage(rect.topLeft(), cache.composite);
            } else {
                p.drawImage(rect, cache.composite);
            }
            drewInk = cache.compositeRevision == page->revision;
        } else if (!cache.background.isNull()) {
            p.drawImage(rect, cache.background);
        }
    } else if (!cache.background.isNull()) {
        p.setRenderHint(QPainter::SmoothPixmapTransform, !m_zooming);
        p.drawImage(rect, cache.background);
    }

    if (!drewInk) {
        p.save();
        p.setClipRect(rect, Qt::IntersectClip);
        p.translate(rect.topLeft());
        p.scale(rect.width() / page->size.width(), rect.height() / page->size.height());
        Ink::paint(p, page->strokes);
        p.restore();
    }
    if (m_stroke && m_strokePage == page && !m_live.isNull()) {
        p.save();
        p.setClipRect(rect, Qt::IntersectClip);
        if (m_stroke->tool == Tool::Highlighter) {
            p.setCompositionMode(QPainter::CompositionMode_Multiply);
            p.setOpacity(Ink::HighlightAlpha);
        }
        p.drawImage(QPointF(0, 0), m_live);
        p.restore();
    }
}

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
    const double d = deviceRatio();
    const QSize size(int(std::ceil(width() * d)), int(std::ceil(height() * d)));
    if (m_live.size() != size || m_live.devicePixelRatio() != d) {
        m_live = QImage(size, QImage::Format_ARGB32_Premultiplied);
        m_live.setDevicePixelRatio(d);
        m_live.fill(Qt::transparent);
        m_liveDirty = {};
        m_liveZoom = 0;
    }
    QPainter lp(&m_live);
    // The view moved under the stroke (or it just began): start the layer over.
    if (m_liveZoom != m_view.zoom() || m_liveOrigin != page.topLeft()) {
        lp.setCompositionMode(QPainter::CompositionMode_Source);
        lp.fillRect(m_liveDirty, Qt::transparent);
        lp.setCompositionMode(QPainter::CompositionMode_SourceOver);
        m_liveDirty = {};
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
    lp.setRenderHint(QPainter::Antialiasing);
    lp.setClipRect(page);
    lp.translate(page.topLeft());
    lp.scale(m_view.zoom(), m_view.zoom());
    lp.setPen(Qt::NoPen);
    lp.setBrush(c);
    lp.drawPath(tail.path());
    m_liveDirty |= QRectF(page.topLeft() + tail.bounds().topLeft() * m_view.zoom(),
                          tail.bounds().size() * m_view.zoom()).adjusted(-2, -2, 2, 2);
}

void NoteView::setHover(QPointF pos, bool visible) {
    const double r = Ink::EraserRadius * m_view.zoom() + 4;
    if (m_hoverVisible)
        update(QRectF(m_hover - QPointF(r, r), QSizeF(2 * r, 2 * r)).toAlignedRect());
    m_hover = pos;
    m_hoverVisible = visible;
    if (visible)
        update(QRectF(pos - QPointF(r, r), QSizeF(2 * r, 2 * r)).toAlignedRect());
}

// --- Ink --------------------------------------------------------------------

void NoteView::beginStroke(QPointF pos, double pressure, bool erase, Source source) {
    if (!m_doc)
        return;
    m_fling.stop();
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
    m_liveZoom = 0; // Clears the previous stroke's layer.
    drawLive(0);
    update(strokeScreenRect(0).toAlignedRect());
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
    m_stroke->invalidate();
    drawLive(m_stroke->samples.size() - 4);
    m_last = pos;
    // The spline reshapes the previous couple of segments too.
    update(strokeScreenRect(m_stroke->samples.size() - 4).toAlignedRect());
}

void NoteView::endStroke() {
    if (m_gesture == Gesture::Ink && m_stroke && m_doc) {
        auto stroke = m_stroke;
        auto page = m_strokePage;
        m_stroke.reset();
        m_strokePage.reset();
        stroke->samples = Ink::simplify(stroke->samples, stroke->tool, stroke->width);
        stroke->invalidate();
        const int before = page->revision;
        m_doc->addStroke(page, stroke);
        // A pen stroke lands on top of everything, and the live layer already
        // holds it at this exact scale: copy those pixels into the page cache
        // instead of filling the whole outline again.
        auto it = m_cache.find(page->id);
        const int index = m_doc->indexOf(page);
        const double scale = m_view.zoom() * deviceRatio();
        if (it != m_cache.end() && stroke->tool == Tool::Pen && it->compositeRevision == before &&
            !it->composite.isNull() && std::abs(it->compositeScale - scale) < 1e-6 && index >= 0 &&
            m_liveZoom == m_view.zoom() && m_liveOrigin == snapped(m_view.pageScreenRect(index)).topLeft()) {
            const double d = deviceRatio();
            const QRectF source(m_liveOrigin * d, QSizeF(it->composite.size()));
            QPainter cp(&it->composite);
            cp.drawImage(QRectF(QPointF(0, 0), QSizeF(it->composite.size()) / d), m_live, source);
            it->compositeRevision = page->revision;
        }
        update();
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
    m_fling.stop();
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
    m_view.panBy(d.x(), d.y());
    const double dt = std::max<qint64>(1, m_panClock.restart());
    m_panVelocity = 0.7 * (d / double(dt)) + 0.3 * m_panVelocity;
    m_last = pos;
    viewMoved();
}

void NoteView::endPan() {
    if (m_source == Source::Touch && m_panClock.elapsed() < 60 && length(m_panVelocity) > 0.3) {
        m_flingVelocity = m_panVelocity;
        m_fling.start();
    }
    m_gesture = Gesture::None;
    m_source = Source::None;
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
    return item == this;
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
            if (m_hoverVisible && !m_stylusDown)
                setHover(m_hover, false);
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
        setHover(m_hover, false);
        setEraseEngaged(false);
    }
    return QQuickPaintedItem::eventFilter(watched, event);
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
    return QQuickPaintedItem::event(event);
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
        m_fling.stop();
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
    if (realMouse(e))
        setHover(e->position(), m_tool == Tool::Eraser);
}

void NoteView::hoverLeaveEvent(QHoverEvent *) { setHover(m_hover, false); }

void NoteView::wheelEvent(QWheelEvent *e) {
    e->accept();
    m_fling.stop();
    emit interacted();
    if (e->modifiers() & Qt::ControlModifier) {
        const double steps = e->angleDelta().y() / 120.0;
        zoomTo(m_view.zoom() * std::pow(1.15, steps), e->position());
        return;
    }
    QPointF d = !e->pixelDelta().isNull() ? QPointF(e->pixelDelta()) : QPointF(e->angleDelta()) / 120.0 * 60.0;
    if ((e->modifiers() & Qt::ShiftModifier) && d.x() == 0)
        d = {d.y(), 0};
    m_view.panBy(d.x(), d.y());
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
            m_fling.stop();
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
