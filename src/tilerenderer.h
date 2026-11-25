#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSizeF>
#include <QWaitCondition>
#include <atomic>
#include <memory>
#include <vector>

#include "ink.h"
#include "pdfstore.h"

class QThread;
namespace Poppler { class Document; }

// Renders pages in tiles (PDF background + committed ink) on worker threads.
// Tiles bound memory at any zoom, and an ink change only re-renders the
// tiles it touches, never the whole page and never on the GUI thread.
class TileRenderer : public QObject {
    Q_OBJECT

public:
    static constexpr int TileSize = 512; // Device pixels.

    struct Job {
        quint64 page = 0;
        int source = -1;           // PDF page index, -1 for blank paper.
        QSizeF pageSize;
        double scale = 1;          // Device pixels per point.
        int tx = 0, ty = 0;
        int revision = 0;          // Page ink revision this renders.
        QList<StrokePtr> strokes;  // Committed, immutable; copied on the GUI thread.
    };

    explicit TileRenderer(int threads = 2, QObject *parent = nullptr);
    ~TileRenderer() override;

    void setPdf(const SourcePtr &source);
    // Replaces the whole wish list, most urgent first. In-flight renders no
    // longer wanted are aborted.
    void setJobs(const QList<Job> &jobs);
    int generation() const { return m_generation; }

    // Tile geometry: grid cell (tx, ty) at a scale, in device pixels of the
    // page image and in page points.
    static QRect tilePixels(QSizeF pageSize, double scale, int tx, int ty);
    static QRectF tilePoints(QSizeF pageSize, double scale, int tx, int ty);
    static QSize gridSize(QSizeF pageSize, double scale);
    // Hides earlier Omanote ink (its own layer) so only live strokes show.
    static void hideInk(Poppler::Document &doc);

signals:
    void rendered(quint64 page, double scale, int tx, int ty, int revision, const QImage &image, int generation);

private:
    struct Worker;
    struct BgKey {
        int source, tx, ty;
        qint64 scale;
        bool operator==(const BgKey &o) const { return source == o.source && tx == o.tx && ty == o.ty && scale == o.scale; }
    };
    friend size_t qHash(const BgKey &k, size_t seed) { return qHashMulti(seed, k.source, k.tx, k.ty, k.scale); }

    void run(Worker *worker);
    QImage render(Worker &worker, const Job &job);
    bool wanted(const Job &job);
    QImage cachedBackground(const BgKey &key);
    void storeBackground(const BgKey &key, const QImage &image);

    QMutex m_mutex;
    QWaitCondition m_wake;
    QList<Job> m_jobs;
    QList<Job> m_wanted;
    SourcePtr m_pdf; // Held here too, so the file stays open while workers read it.
    std::atomic<int> m_generation{0};
    bool m_quit = false;
    std::vector<std::unique_ptr<Worker>> m_workers;

    // Rendered PDF backgrounds, so ink edits don't re-run poppler.
    QMutex m_bgMutex;
    QHash<BgKey, QImage> m_bg;
    QList<BgKey> m_bgOrder;
    qint64 m_bgBytes = 0;
};
