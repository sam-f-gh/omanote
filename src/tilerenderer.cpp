#include "tilerenderer.h"

#include <QPainter>
#include <QVariant>
#include <cmath>
#include <functional>
#include <poppler-optcontent.h>
#include <poppler-qt6.h>
#include <thread>

#include "pdfstore.h"

namespace {
constexpr qint64 BackgroundBudget = 96ll << 20; // Bytes of cached PDF tiles.

bool sameTile(const TileRenderer::Job &a, const TileRenderer::Job &b) {
    return a.page == b.page && a.tx == b.tx && a.ty == b.ty && a.scale == b.scale;
}
}

struct TileRenderer::Worker {
    std::thread thread;
    std::unique_ptr<Poppler::Document> doc;
    int docGeneration = -1;
    bool busy = false;
    Job current;
};

TileRenderer::TileRenderer(int threads, QObject *parent) : QObject(parent) {
    for (int i = 0; i < std::max(1, threads); ++i) {
        auto worker = std::make_unique<Worker>();
        Worker *w = worker.get();
        m_workers.push_back(std::move(worker));
        w->thread = std::thread([this, w] { run(w); });
    }
}

TileRenderer::~TileRenderer() {
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
        ++m_generation; // Aborts renders in flight.
        m_wake.wakeAll();
    }
    for (auto &w : m_workers)
        w->thread.join();
}

QSize TileRenderer::gridSize(QSizeF pageSize, double scale) {
    const int w = int(std::ceil(pageSize.width() * scale)), h = int(std::ceil(pageSize.height() * scale));
    return {(w + TileSize - 1) / TileSize, (h + TileSize - 1) / TileSize};
}

QRect TileRenderer::tilePixels(QSizeF pageSize, double scale, int tx, int ty) {
    const int w = int(std::ceil(pageSize.width() * scale)), h = int(std::ceil(pageSize.height() * scale));
    const int x = tx * TileSize, y = ty * TileSize;
    return {x, y, std::max(0, std::min(TileSize, w - x)), std::max(0, std::min(TileSize, h - y))};
}

QRectF TileRenderer::tilePoints(QSizeF pageSize, double scale, int tx, int ty) {
    const QRect px = tilePixels(pageSize, scale, tx, ty);
    return {px.x() / scale, px.y() / scale, px.width() / scale, px.height() / scale};
}

void TileRenderer::hideInk(Poppler::Document &doc) {
    // Poppler crashes building a layer model for a PDF without layers.
    if (!doc.hasOptionalContent())
        return;
    Poppler::OptContentModel *model = doc.optionalContentModel();
    if (!model)
        return;
    std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
        for (int r = 0; r < model->rowCount(parent); ++r) {
            const QModelIndex i = model->index(r, 0, parent);
            if (model->data(i, Qt::DisplayRole).toString() == QLatin1String(PdfStore::LayerName))
                model->setData(i, Qt::Unchecked, Qt::CheckStateRole);
            walk(i);
        }
    };
    walk({});
}

void TileRenderer::setPdf(const QByteArray &pdf) {
    QMutexLocker lock(&m_mutex);
    m_pdf = pdf;
    m_jobs.clear();
    ++m_generation;
    QMutexLocker bg(&m_bgMutex);
    m_bg.clear();
    m_bgOrder.clear();
    m_bgBytes = 0;
}

void TileRenderer::setJobs(const QList<Job> &jobs) {
    QMutexLocker lock(&m_mutex);
    m_jobs.clear();
    for (const auto &job : jobs) {
        bool inFlight = false;
        for (const auto &w : m_workers)
            if (w->busy && sameTile(w->current, job) && w->current.revision == job.revision)
                inFlight = true;
        if (!inFlight)
            m_jobs.append(job);
    }
    // Workers check wanted() while poppler renders, so dropping a job from
    // the list is enough to abort it.
    m_wanted = jobs;
    m_wake.wakeAll();
}

bool TileRenderer::wanted(const Job &job) {
    QMutexLocker lock(&m_mutex);
    if (m_quit)
        return false;
    for (const auto &j : m_wanted)
        if (sameTile(j, job))
            return true;
    return false;
}

QImage TileRenderer::cachedBackground(const BgKey &key) {
    QMutexLocker lock(&m_bgMutex);
    auto it = m_bg.find(key);
    if (it == m_bg.end())
        return {};
    m_bgOrder.removeOne(key);
    m_bgOrder.append(key);
    return *it;
}

void TileRenderer::storeBackground(const BgKey &key, const QImage &image) {
    QMutexLocker lock(&m_bgMutex);
    if (m_bg.contains(key))
        return;
    m_bg.insert(key, image);
    m_bgOrder.append(key);
    m_bgBytes += image.sizeInBytes();
    while (m_bgBytes > BackgroundBudget && !m_bgOrder.isEmpty()) {
        const BgKey old = m_bgOrder.takeFirst();
        m_bgBytes -= m_bg.take(old).sizeInBytes();
    }
}

void TileRenderer::run(Worker *worker) {
    for (;;) {
        Job job;
        QByteArray pdf;
        int generation;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_jobs.isEmpty())
                m_wake.wait(&m_mutex);
            if (m_quit)
                return;
            job = m_jobs.takeFirst();
            worker->current = job;
            worker->busy = true;
            generation = m_generation;
            pdf = m_pdf;
        }
        if (worker->docGeneration != generation) {
            worker->doc = pdf.isEmpty() ? nullptr : Poppler::Document::loadFromData(pdf);
            if (worker->doc) {
                worker->doc->setRenderHint(Poppler::Document::Antialiasing);
                worker->doc->setRenderHint(Poppler::Document::TextAntialiasing);
                worker->doc->setRenderHint(Poppler::Document::TextHinting);
                worker->doc->setPaperColor(Qt::white);
                hideInk(*worker->doc);
            }
            worker->docGeneration = generation;
        }
        const QImage image = render(*worker, job);
        {
            QMutexLocker lock(&m_mutex);
            worker->busy = false;
        }
        if (!image.isNull() && generation == m_generation)
            emit rendered(job.page, job.scale, job.tx, job.ty, job.revision, image, generation);
    }
}

QImage TileRenderer::render(Worker &worker, const Job &job) {
    const QRect px = tilePixels(job.pageSize, job.scale, job.tx, job.ty);
    if (px.isEmpty())
        return {};
    QImage image;
    if (job.source >= 0 && worker.doc && job.source < worker.doc->numPages()) {
        const BgKey key{job.source, job.tx, job.ty, qint64(std::llround(job.scale * 1e4))};
        image = cachedBackground(key);
        if (image.isNull()) {
            std::unique_ptr<Poppler::Page> page = worker.doc->page(job.source);
            if (!page)
                return {};
            struct Context { TileRenderer *self; const Job *job; int generation; } ctx{this, &job, m_generation};
            auto abort = [](const QVariant &payload) {
                auto *c = reinterpret_cast<Context *>(payload.value<quintptr>());
                return c->generation != c->self->m_generation || !c->self->wanted(*c->job);
            };
            const double dpi = 72.0 * job.scale;
            image = page->renderToImage(dpi, dpi, px.x(), px.y(), px.width(), px.height(), Poppler::Page::Rotate0,
                                        nullptr, nullptr, abort, QVariant::fromValue(quintptr(&ctx)));
            if (image.isNull() || abort(QVariant::fromValue(quintptr(&ctx))))
                return {};
            image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            storeBackground(key, image);
        }
    } else {
        image = QImage(px.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
    }

    const QRectF area = tilePoints(job.pageSize, job.scale, job.tx, job.ty);
    QList<StrokePtr> strokes;
    for (const auto &s : job.strokes)
        if (s->bounds().intersects(area))
            strokes.append(s);
    if (!strokes.isEmpty()) {
        QPainter p(&image);
        p.translate(-px.x(), -px.y());
        p.scale(job.scale, job.scale);
        p.setClipRect(QRectF(QPointF(0, 0), job.pageSize));
        Ink::paint(p, strokes);
    }
    return image;
}
