#pragma once

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSizeF>

// Page layout and pan/zoom math, kept free of Qt Quick so it can be tested.
// Document space is points: pages stack vertically, centered on x = 0, with
// a gap between them. Screen space is the view's logical pixels.
class Viewport {
public:
    static constexpr double Gap = 18;      // Points between pages.
    static constexpr double Pad = 24;      // Pixels above the first and below the last page.
    static constexpr double MinZoom = 0.25, MaxZoom = 4.0;

    void setPages(const QList<QSizeF> &sizes);
    void setViewSize(QSizeF size);
    int pageCount() const { return m_sizes.size(); }

    double zoom() const { return m_zoom; }
    double panX() const { return m_panX; }
    double scrollY() const { return m_scrollY; }

    double docHeight() const;
    double maxPanX() const;
    double maxScrollY() const;
    // A whole first page's worth of room above the first page, so a fresh
    // sheet can sit as low on the screen as the hand likes.
    double minScrollY() const { return m_sizes.isEmpty() ? 0 : -m_sizes.first().height() * m_zoom; }
    bool horizontallyLocked() const { return m_docWidth * m_zoom <= m_view.width(); }

    QRectF pageDocRect(int index) const;
    QRectF pageScreenRect(int index) const;
    QPointF toScreen(QPointF doc) const;
    QPointF toDoc(QPointF screen) const;
    // Page under a screen point and the point in that page's coordinates.
    int pageAt(QPointF screen, QPointF *local = nullptr) const;
    QPointF toPage(int index, QPointF screen) const;
    int currentPage() const;
    QList<int> visiblePages(double margin = 0) const;

    // Zoom about a fixed screen point (pinch centroid or cursor).
    void zoomAt(double zoom, QPointF focus);
    void panBy(double dx, double dy);
    void setScrollY(double y);
    void scrollToPage(int index);
    double fitWidthZoom() const;
    void clamp();

private:
    QList<QSizeF> m_sizes;
    QList<double> m_tops;
    QSizeF m_view{800, 600};
    double m_docWidth = 0;
    double m_zoom = 1;
    double m_panX = 0;
    double m_scrollY = 0;
};
