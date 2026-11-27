#pragma once

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <atomic>
#include <memory>

class QPainter;

// Strokes live in page coordinates: PDF points, top-left origin, y down, in
// the page's displayed (rotated) orientation. Screen and PDF both draw the
// outline that Ink::outline() builds, so they can't disagree.
enum class Tool { Pen = 0, Highlighter = 1, Eraser = 2 };

struct Sample {
    float x = 0, y = 0, p = 0.5f;
};

// A cached outline that render threads may read while the GUI drops it.
// Copies start empty: the outline is cheap to rebuild and copies are rare.
class PathCache {
public:
    PathCache() = default;
    PathCache(const PathCache &) {}
    PathCache &operator=(const PathCache &) { store(nullptr); return *this; }
    std::shared_ptr<const QPainterPath> load() const { return m_value.load(); }
    void store(std::shared_ptr<const QPainterPath> p) const { m_value.store(std::move(p)); }

private:
    mutable std::atomic<std::shared_ptr<const QPainterPath>> m_value;
};

struct Stroke {
    quint64 id = 0;
    Tool tool = Tool::Pen;
    QColor color{0x18, 0x18, 0x1c};
    float width = 2.5f;
    QVector<Sample> samples;

    QPointF point(int i) const { return {samples[i].x, samples[i].y}; }
    float radius(int i) const;
    // The filled outline. Safe to call from render threads: the cache is
    // swapped atomically, and it can be dropped to save memory any time.
    // Without cache, a missing outline is built but not kept: saving walks
    // every stroke in the document and mustn't pin all their outlines.
    std::shared_ptr<const QPainterPath> path(bool cache = true) const;
    bool hasCachedPath() const { return bool(m_path.load()); }
    void releasePath() const;
    // Covers the outline; cheap (from samples), so hit tests and tile
    // culling never need the path. Precomputed by finalize().
    QRectF bounds() const;
    // Called once a stroke is committed: from then on it is immutable and
    // may be shared with render threads.
    void finalize();

private:
    PathCache m_path;
    QRectF m_bounds;
    bool m_finalized = false;
};

using StrokePtr = std::shared_ptr<Stroke>;

namespace Ink {
constexpr double HighlightAlpha = 0.4;
constexpr double EraserRadius = 5.0;

// Pressure 0.5 (the mouse default) gives exactly the nominal width.
float radiusFor(Tool tool, float width, float pressure);

// Drops jitter and near-duplicate samples so committed strokes stay small in
// the PDF without visibly changing shape (Ramer–Douglas–Peucker on x, y, r).
QVector<Sample> simplify(const QVector<Sample> &samples, Tool tool, float width, double tolerance = 0.12);

// Union of round-capped, variable-radius capsules along a Catmull-Rom spline
// through the samples. Filled with the nonzero rule, overlaps never double up,
// which keeps translucent highlighters even.
QPainterPath outline(const Stroke &stroke);

// Highlighters first (multiply, translucent), then pens on top.
void paint(QPainter &painter, const QList<StrokePtr> &strokes);

// Does the eraser, swept from a to b with radius r, touch the stroke?
bool hits(const Stroke &stroke, QPointF a, QPointF b, double r = EraserRadius);

QJsonObject toJson(const Stroke &stroke);
StrokePtr fromJson(const QJsonObject &object);
// Samples as delta-coded varints in base64: ~4 bytes a sample instead of ~20.
QString encodeSamples(const QVector<Sample> &samples);
QVector<Sample> decodeSamples(const QString &data);

// PDF content-stream operators for a path (m/l/c/h), two decimals.
QByteArray pdfPath(const QPainterPath &path);
}
