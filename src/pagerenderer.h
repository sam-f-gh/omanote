#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>

// Renders PDF page backgrounds with poppler on one worker thread, so opening
// or zooming a long PDF never blocks drawing. The caller hands over the whole
// wish list (visible pages first) each time; stale jobs simply get replaced.
class PageRenderer : public QThread {
    Q_OBJECT

public:
    struct Job {
        quint64 key;   // Page id, echoed back.
        int source;    // Page index in the PDF.
        double scale;  // Device pixels per point.
    };

    explicit PageRenderer(QObject *parent = nullptr);
    ~PageRenderer() override;

    void setPdf(const QByteArray &pdf);
    void setJobs(const QList<Job> &jobs);
    int generation() const { return m_generation; }

signals:
    void rendered(quint64 key, double scale, const QImage &image, int generation);
    void failed(const QString &message);

protected:
    void run() override;

private:
    QMutex m_mutex;
    QWaitCondition m_wake;
    QByteArray m_pdf;
    QList<Job> m_jobs;
    Job m_current{0, -1, 0};
    bool m_reload = false;
    bool m_quit = false;
    int m_generation = 0;
};
