#include "viewport.h"

#include <algorithm>
#include <cmath>

void Viewport::setPages(const QList<QSizeF> &sizes) {
    m_sizes = sizes;
    m_tops.clear();
    m_docWidth = 0;
    double y = 0;
    for (const auto &s : sizes) {
        m_tops.append(y);
        y += s.height() + Gap;
        m_docWidth = std::max(m_docWidth, s.width());
    }
    clamp();
}

void Viewport::setViewSize(QSizeF size) {
    m_view = size;
    clamp();
}

double Viewport::docHeight() const {
    return m_sizes.isEmpty() ? 0 : m_tops.last() + m_sizes.last().height();
}

double Viewport::maxPanX() const { return std::max(0.0, (m_docWidth * m_zoom - m_view.width()) / 2); }

double Viewport::maxScrollY() const { return std::max(0.0, docHeight() * m_zoom + 2 * Pad - m_view.height()); }

QRectF Viewport::pageDocRect(int i) const {
    if (i < 0 || i >= m_sizes.size())
        return {};
    return {-m_sizes[i].width() / 2, m_tops[i], m_sizes[i].width(), m_sizes[i].height()};
}

QPointF Viewport::toScreen(QPointF d) const {
    return {m_view.width() / 2 + m_panX + d.x() * m_zoom, Pad + d.y() * m_zoom - m_scrollY};
}

QPointF Viewport::toDoc(QPointF s) const {
    return {(s.x() - m_view.width() / 2 - m_panX) / m_zoom, (s.y() - Pad + m_scrollY) / m_zoom};
}

QRectF Viewport::pageScreenRect(int i) const {
    const QRectF r = pageDocRect(i);
    return {toScreen(r.topLeft()), r.size() * m_zoom};
}

QPointF Viewport::toPage(int i, QPointF screen) const { return toDoc(screen) - pageDocRect(i).topLeft(); }

int Viewport::pageAt(QPointF screen, QPointF *local) const {
    const QPointF d = toDoc(screen);
    // Binary search over tops; pages are few enough that this is plenty.
    auto it = std::upper_bound(m_tops.begin(), m_tops.end(), d.y());
    const int i = int(it - m_tops.begin()) - 1;
    if (i < 0 || !pageDocRect(i).contains(d))
        return -1;
    if (local)
        *local = d - pageDocRect(i).topLeft();
    return i;
}

int Viewport::currentPage() const {
    if (m_sizes.isEmpty())
        return -1;
    // Scrolled to the very end: the last page is the current one.
    if (maxScrollY() > 0 && m_scrollY >= maxScrollY() - 0.5)
        return int(m_sizes.size()) - 1;
    // Otherwise the page crossing a line a third of the way down, so a page
    // reached by "next" (its top at the top) is the current one.
    const double y = toDoc({0, std::min(m_view.height() / 3, Pad + 40)}).y();
    auto it = std::upper_bound(m_tops.begin(), m_tops.end(), y);
    return std::clamp(int(it - m_tops.begin()) - 1, 0, int(m_sizes.size()) - 1);
}

QList<int> Viewport::visiblePages(double margin) const {
    QList<int> out;
    const double top = toDoc({0, -margin}).y(), bottom = toDoc({0, m_view.height() + margin}).y();
    auto it = std::upper_bound(m_tops.begin(), m_tops.end(), top);
    for (int i = std::max(0, int(it - m_tops.begin()) - 1); i < m_sizes.size() && m_tops[i] <= bottom; ++i)
        if (m_tops[i] + m_sizes[i].height() >= top)
            out.append(i);
    return out;
}

void Viewport::zoomAt(double zoom, QPointF focus) {
    const QPointF d = toDoc(focus);
    m_zoom = std::clamp(zoom, MinZoom, MaxZoom);
    m_panX = focus.x() - m_view.width() / 2 - d.x() * m_zoom;
    m_scrollY = Pad + d.y() * m_zoom - focus.y();
    clamp();
}

void Viewport::panBy(double dx, double dy) {
    m_panX += dx;
    m_scrollY -= dy;
    clamp();
}

void Viewport::setScrollY(double y) {
    m_scrollY = y;
    clamp();
}

void Viewport::scrollToPage(int i) {
    if (i < 0 || i >= m_sizes.size())
        return;
    m_scrollY = m_tops[i] * m_zoom;
    clamp();
}

double Viewport::fitWidthZoom() const {
    if (m_docWidth <= 0)
        return 1;
    return std::clamp((m_view.width() - 32) / m_docWidth, MinZoom, MaxZoom);
}

// Narrow documents stay centered; wide ones pan only until an edge meets the
// viewport edge, so zooming out glides back to center rather than sticking.
void Viewport::clamp() {
    if (horizontallyLocked())
        m_panX = 0;
    else
        m_panX = std::clamp(m_panX, -maxPanX(), maxPanX());
    m_scrollY = std::clamp(m_scrollY, minScrollY(), maxScrollY());
}
