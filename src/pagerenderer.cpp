#include "pagerenderer.h"

#include <poppler-qt6.h>

PageRenderer::PageRenderer(QObject *parent) : QThread(parent) { start(QThread::LowPriority); }

PageRenderer::~PageRenderer() {
    {
        QMutexLocker lock(&m_mutex);
        m_quit = true;
        m_wake.wakeAll();
    }
    wait();
}

void PageRenderer::setPdf(const QByteArray &pdf) {
    QMutexLocker lock(&m_mutex);
    m_pdf = pdf;
    m_jobs.clear();
    m_reload = true;
    ++m_generation;
    m_wake.wakeAll();
}

void PageRenderer::setJobs(const QList<Job> &jobs) {
    QMutexLocker lock(&m_mutex);
    // Don't queue again what the worker is rendering right now.
    m_jobs.clear();
    for (const auto &job : jobs)
        if (job.key != m_current.key || job.scale != m_current.scale)
            m_jobs.append(job);
    m_wake.wakeAll();
}

void PageRenderer::run() {
    std::unique_ptr<Poppler::Document> doc;
    int generation = 0;
    for (;;) {
        Job job{};
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && !m_reload && m_jobs.isEmpty())
                m_wake.wait(&m_mutex);
            if (m_quit)
                return;
            if (m_reload) {
                m_reload = false;
                generation = m_generation;
                const QByteArray pdf = m_pdf;
                lock.unlock();
                doc = pdf.isEmpty() ? nullptr : Poppler::Document::loadFromData(pdf);
                if (doc) {
                    doc->setRenderHint(Poppler::Document::Antialiasing);
                    doc->setRenderHint(Poppler::Document::TextAntialiasing);
                    doc->setRenderHint(Poppler::Document::TextHinting);
                    doc->setPaperColor(Qt::white);
                } else if (!pdf.isEmpty()) {
                    emit failed(tr("Couldn't render this PDF"));
                }
                continue;
            }
            job = m_jobs.takeFirst();
            m_current = job;
        }
        std::unique_ptr<Poppler::Page> page;
        if (doc && job.source >= 0 && job.source < doc->numPages())
            page = doc->page(job.source);
        if (!page) {
            QMutexLocker lock(&m_mutex);
            m_current = {0, -1, 0};
            continue;
        }
        const double dpi = 72.0 * job.scale;
        QImage image = page->renderToImage(dpi, dpi);
        if (!image.isNull())
            emit rendered(job.key, job.scale, image.convertToFormat(QImage::Format_ARGB32_Premultiplied), generation);
        QMutexLocker lock(&m_mutex);
        m_current = {0, -1, 0};
    }
}
