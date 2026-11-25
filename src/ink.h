#pragma once

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <memory>

class QPainter;

// Strokes live in page coordinates: PDF points, top-left origin, y down, in
// the page's displayed (rotated) orientation. Screen and PDF both draw the
// outline that Ink::outline() builds, so they can't disagree.
enum class Tool { Pen = 0, Highlighter = 1, Eraser = 2 };

struct Sample {
    float x = 0, y = 0, p = 0.5f;
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
    std::shared_ptr<const QPainterPath> path() const;
    void releasePath() const;
    // Covers the outline; cheap (from samples), so hit tests and tile
    // culling never need the path. Precomputed by finalize().
    QRectF bounds() const;
    // Called once a stroke is committed: from then on it is immutable and
    // may be shared with render threads.
    void finalize();
    void invalidate();

private:
    mutable std::shared_ptr<const QPainterPath> m_path;
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
void paintOne(QPainter &painter, const Stroke &stroke);

// Does the eraser, swept from a to b with radius r, touch the stroke?
bool hits(const Stroke &stroke, QPointF a, QPointF b, double r = EraserRadius);
double segmentDistance(QPointF a, QPointF b, QPointF c, QPointF d);

QJsonObject toJson(const Stroke &stroke);
StrokePtr fromJson(const QJsonObject &object);
// Samples as delta-coded varints in base64: ~4 bytes a sample instead of ~20.
QString encodeSamples(const QVector<Sample> &samples);
QVector<Sample> decodeSamples(const QString &data);

// PDF content-stream operators for a path (m/l/c/h), two decimals.
QByteArray pdfPath(const QPainterPath &path);
}
