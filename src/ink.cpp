#include "ink.h"

#include <QJsonArray>
#include <QPainter>
#include <QtMath>
#include <algorithm>
#include <cmath>

float Stroke::radius(int i) const { return Ink::radiusFor(tool, width, samples[i].p); }

const QPainterPath &Stroke::path() const {
    if (!m_path)
        m_path = std::make_shared<QPainterPath>(Ink::outline(*this));
    return *m_path;
}

QRectF Stroke::bounds() const { return path().boundingRect(); }

namespace Ink {

float radiusFor(Tool tool, float width, float pressure) {
    if (tool == Tool::Highlighter)
        return width / 2;
    // A gentle curve: light touches thin out, firm ones swell, and 0.5 is
    // the nominal width so mouse strokes match the preset exactly.
    const float p = std::clamp(pressure, 0.f, 1.f);
    return width / 2 * (0.3f + 1.4f * p);
}

namespace {
struct Node {
    QPointF pt;
    double r;
    bool joint;
};

double dist3(const Sample &s, const Sample &a, const Sample &b, float ra, float rb, float rs) {
    // Distance from s to segment ab in (x, y, r) space.
    const double ax = a.x, ay = a.y, ar = ra, bx = b.x, by = b.y, br = rb;
    const double dx = bx - ax, dy = by - ay, dr = br - ar;
    const double len2 = dx * dx + dy * dy + dr * dr;
    double t = len2 > 0 ? ((s.x - ax) * dx + (s.y - ay) * dy + (rs - ar) * dr) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    const double px = ax + t * dx - s.x, py = ay + t * dy - s.y, pr = ar + t * dr - rs;
    return std::sqrt(px * px + py * py + pr * pr);
}

void rdp(const QVector<Sample> &s, const QVector<float> &r, int a, int b, double tol, QVector<bool> &keep) {
    if (b <= a + 1)
        return;
    double best = -1;
    int index = -1;
    for (int i = a + 1; i < b; ++i) {
        const double d = dist3(s[i], s[a], s[b], r[a], r[b], r[i]);
        if (d > best) { best = d; index = i; }
    }
    if (best > tol) {
        keep[index] = true;
        rdp(s, r, a, index, tol, keep);
        rdp(s, r, index, b, tol, keep);
    }
}

constexpr double Kappa = 0.5522847498;

void addCircle(QPainterPath &path, QPointF c, double r) {
    // Positive shoelace orientation in y-down space; quads below match it so
    // nonzero filling never punches holes where shapes overlap.
    const double k = Kappa * r;
    path.moveTo(c.x() + r, c.y());
    path.cubicTo(c.x() + r, c.y() + k, c.x() + k, c.y() + r, c.x(), c.y() + r);
    path.cubicTo(c.x() - k, c.y() + r, c.x() - r, c.y() + k, c.x() - r, c.y());
    path.cubicTo(c.x() - r, c.y() - k, c.x() - k, c.y() - r, c.x(), c.y() - r);
    path.cubicTo(c.x() + k, c.y() - r, c.x() + r, c.y() - k, c.x() + r, c.y());
    path.closeSubpath();
}

void addQuad(QPainterPath &path, QPointF a, QPointF b, QPointF c, QPointF d) {
    const double area = (a.x() * b.y() - b.x() * a.y()) + (b.x() * c.y() - c.x() * b.y()) +
                        (c.x() * d.y() - d.x() * c.y()) + (d.x() * a.y() - a.x() * d.y());
    if (area < 0)
        std::swap(b, d);
    path.moveTo(a);
    path.lineTo(b);
    path.lineTo(c);
    path.lineTo(d);
    path.closeSubpath();
}

// The hull of two circles: the trapezoid between their outer tangents.
void addCapsuleBody(QPainterPath &path, const Node &n0, const Node &n1) {
    const QPointF d = n1.pt - n0.pt;
    const double len = std::hypot(d.x(), d.y());
    if (len <= std::abs(n0.r - n1.r) + 1e-6)
        return; // One circle contains the other.
    const double theta = std::atan2(d.y(), d.x());
    const double alpha = std::acos(std::clamp((n0.r - n1.r) / len, -1.0, 1.0));
    const QPointF u1(std::cos(theta + alpha), std::sin(theta + alpha));
    const QPointF u2(std::cos(theta - alpha), std::sin(theta - alpha));
    addQuad(path, n0.pt + u1 * n0.r, n1.pt + u1 * n1.r, n1.pt + u2 * n1.r, n0.pt + u2 * n0.r);
}

QPointF catmull(QPointF p0, QPointF p1, QPointF p2, QPointF p3, double t) {
    const double t2 = t * t, t3 = t2 * t;
    return 0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
}

QByteArray num(double v) {
    QByteArray s = QByteArray::number(v, 'f', 2);
    if (s.contains('.')) {
        while (s.endsWith('0')) s.chop(1);
        if (s.endsWith('.')) s.chop(1);
    }
    return s == "-0" ? "0" : s;
}
}

QVector<Sample> simplify(const QVector<Sample> &samples, Tool tool, float width, double tolerance) {
    if (samples.size() <= 2)
        return samples;
    QVector<float> r(samples.size());
    for (int i = 0; i < samples.size(); ++i)
        r[i] = radiusFor(tool, width, samples[i].p);
    QVector<bool> keep(samples.size(), false);
    keep.first() = keep.last() = true;
    rdp(samples, r, 0, samples.size() - 1, tolerance, keep);
    QVector<Sample> out;
    for (int i = 0; i < samples.size(); ++i)
        if (keep[i])
            out.append(samples[i]);
    return out;
}

QPainterPath outline(const Stroke &stroke) {
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    const int n = stroke.samples.size();
    if (n == 0)
        return path;
    if (n == 1) {
        addCircle(path, stroke.point(0), stroke.radius(0));
        return path;
    }

    // Densify along a Catmull-Rom spline so curves read smooth at any zoom.
    QVector<Node> nodes;
    nodes.reserve(n * 3);
    for (int i = 0; i < n - 1; ++i) {
        const QPointF p0 = stroke.point(std::max(0, i - 1)), p1 = stroke.point(i), p2 = stroke.point(i + 1),
                      p3 = stroke.point(std::min(n - 1, i + 2));
        const double r1 = stroke.radius(i), r2 = stroke.radius(i + 1);
        const double len = QLineF(p1, p2).length();
        const int steps = std::clamp(int(std::ceil(len / 1.5)), 1, 12);
        for (int s = 0; s < steps; ++s) {
            const double t = double(s) / steps;
            nodes.append({s == 0 ? p1 : catmull(p0, p1, p2, p3, t), r1 + (r2 - r1) * t, s == 0});
        }
    }
    nodes.append({stroke.point(n - 1), stroke.radius(n - 1), true});

    // Round joints only where the input turned; between spline steps the
    // tangent change is far below a hairline.
    for (int i = 0; i < nodes.size(); ++i)
        if (nodes[i].joint)
            addCircle(path, nodes[i].pt, nodes[i].r);
    for (int i = 0; i + 1 < nodes.size(); ++i)
        addCapsuleBody(path, nodes[i], nodes[i + 1]);
    return path;
}

void paintOne(QPainter &painter, const Stroke &stroke) {
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor c = stroke.color;
    if (stroke.tool == Tool::Highlighter) {
        c.setAlphaF(HighlightAlpha);
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
    }
    painter.setBrush(c);
    painter.drawPath(stroke.path());
    painter.restore();
}

void paint(QPainter &painter, const QList<StrokePtr> &strokes) {
    for (const auto &s : strokes)
        if (s->tool == Tool::Highlighter)
            paintOne(painter, *s);
    for (const auto &s : strokes)
        if (s->tool != Tool::Highlighter)
            paintOne(painter, *s);
}

double segmentDistance(QPointF a, QPointF b, QPointF c, QPointF d) {
    auto pointSeg = [](QPointF p, QPointF a, QPointF b) {
        const QPointF ab = b - a;
        const double len2 = QPointF::dotProduct(ab, ab);
        const double t = len2 > 0 ? std::clamp(QPointF::dotProduct(p - a, ab) / len2, 0.0, 1.0) : 0.0;
        const QPointF q = a + ab * t - p;
        return std::hypot(q.x(), q.y());
    };
    auto cross = [](QPointF o, QPointF a, QPointF b) {
        return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
    };
    const double d1 = cross(c, d, a), d2 = cross(c, d, b), d3 = cross(a, b, c), d4 = cross(a, b, d);
    if (((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0)))
        return 0;
    return std::min({pointSeg(a, c, d), pointSeg(b, c, d), pointSeg(c, a, b), pointSeg(d, a, b)});
}

bool hits(const Stroke &stroke, QPointF a, QPointF b, double r) {
    const int n = stroke.samples.size();
    if (n == 0)
        return false;
    const QRectF sweep = QRectF(a, b).normalized().adjusted(-r, -r, r, r);
    if (!sweep.intersects(stroke.bounds()))
        return false;
    if (n == 1)
        return segmentDistance(a, b, stroke.point(0), stroke.point(0)) <= r + stroke.radius(0);
    for (int i = 0; i + 1 < n; ++i) {
        const double reach = r + std::max(stroke.radius(i), stroke.radius(i + 1));
        if (segmentDistance(a, b, stroke.point(i), stroke.point(i + 1)) <= reach)
            return true;
    }
    return false;
}

QJsonObject toJson(const Stroke &stroke) {
    QJsonArray points;
    for (const auto &s : stroke.samples) {
        points.append(std::round(s.x * 100) / 100);
        points.append(std::round(s.y * 100) / 100);
        points.append(std::round(s.p * 1000) / 1000);
    }
    return {{"id", QString::number(stroke.id)},
            {"tool", stroke.tool == Tool::Highlighter ? "highlighter" : "pen"},
            {"color", stroke.color.name()},
            {"width", stroke.width},
            {"points", points}};
}

StrokePtr fromJson(const QJsonObject &o) {
    auto s = std::make_shared<Stroke>();
    s->id = o.value("id").toString().toULongLong();
    s->tool = o.value("tool").toString() == "highlighter" ? Tool::Highlighter : Tool::Pen;
    s->color = QColor(o.value("color").toString());
    if (!s->color.isValid())
        s->color = QColor(0x18, 0x18, 0x1c);
    s->width = std::clamp(float(o.value("width").toDouble(2.5)), 0.1f, 100.f);
    const QJsonArray pts = o.value("points").toArray();
    for (int i = 0; i + 2 < pts.size(); i += 3)
        s->samples.append({float(pts[i].toDouble()), float(pts[i + 1].toDouble()), float(pts[i + 2].toDouble())});
    if (s->samples.isEmpty())
        return nullptr;
    return s;
}

QByteArray pdfPath(const QPainterPath &path) {
    QByteArray out;
    out.reserve(path.elementCount() * 14);
    bool open = false;
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto e = path.elementAt(i);
        switch (e.type) {
        case QPainterPath::MoveToElement:
            if (open) out += "h\n";
            out += num(e.x) + ' ' + num(e.y) + " m\n";
            open = true;
            break;
        case QPainterPath::LineToElement:
            out += num(e.x) + ' ' + num(e.y) + " l\n";
            break;
        case QPainterPath::CurveToElement: {
            const auto c2 = path.elementAt(i + 1), end = path.elementAt(i + 2);
            out += num(e.x) + ' ' + num(e.y) + ' ' + num(c2.x) + ' ' + num(c2.y) + ' ' + num(end.x) + ' ' +
                   num(end.y) + " c\n";
            i += 2;
            break;
        }
        default:
            break;
        }
    }
    if (open) out += "h\n";
    return out;
}
}
