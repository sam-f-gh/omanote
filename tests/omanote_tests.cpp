#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QtTest>
#include <poppler-qt6.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include "document.h"
#include "ink.h"
#include "pdfstore.h"
#include "theme.h"
#include "tilerenderer.h"
#include "translator.h"
#include "viewport.h"

namespace {
StrokePtr line(QPointF a, QPointF b, Tool tool = Tool::Pen, float pressure = 0.5f) {
    auto s = std::make_shared<Stroke>();
    s->id = Document::newId();
    s->tool = tool;
    s->width = tool == Tool::Highlighter ? 14 : 2.5f;
    for (int i = 0; i <= 20; ++i) {
        const QPointF p = a + (b - a) * (i / 20.0);
        s->samples.append({float(p.x()), float(p.y()), pressure + 0.02f * (i % 5)});
    }
    return s;
}

// A two-page "foreign" PDF with real text, the second page left blank.
QString externalPdf(const QString &dir) {
    const QString path = dir + "/external.pdf";
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setResolution(72);
    writer.setCreator("tests");
    QPainter p(&writer);
    QFont font("Sans");
    font.setPixelSize(24);
    p.setFont(font);
    p.drawText(QPointF(72, 100), "Hello original text");
    p.drawRect(QRectF(72, 200, 200, 100));
    writer.newPage();
    p.end();
    return path;
}

QString pageText(const QString &path, int page) {
    auto doc = Poppler::Document::load(path);
    if (!doc || page >= doc->numPages())
        return {};
    return doc->page(page)->text(QRectF());
}

int popplerPages(const QString &path) {
    auto doc = Poppler::Document::load(path);
    return doc ? doc->numPages() : -1;
}

QColor pixel(const QString &path, int page, QPointF pt) {
    auto doc = Poppler::Document::load(path);
    const QImage img = doc->page(page)->renderToImage(72, 72);
    return img.pixelColor(pt.toPoint());
}

void write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(contents);
}
}

class OmanoteTests : public QObject {
    Q_OBJECT

private slots:
    // --- Ink -------------------------------------------------------------
    void pressureModulatesWidth() {
        QCOMPARE(Ink::radiusFor(Tool::Pen, 2.5f, 0.5f), 1.25f);
        QVERIFY(Ink::radiusFor(Tool::Pen, 2.5f, 1.0f) > Ink::radiusFor(Tool::Pen, 2.5f, 0.2f));
        QCOMPARE(Ink::radiusFor(Tool::Highlighter, 14, 0.1f), 7.f);
    }

    void outlineCoversStrokeWithoutHoles() {
        auto s = line({10, 10}, {100, 60});
        const QPainterPath path = *s->path();
        QVERIFY(path.contains(QPointF(55, 35)));
        QVERIFY(!path.contains(QPointF(55, 50)));
        // A sharp hairpin must still fill solid (no winding cancellation).
        Stroke v;
        for (int i = 0; i <= 40; ++i)
            v.samples.append({float(i < 20 ? i * 2.5 : (40 - i) * 2.5), float(i < 20 ? i * 2.5 : 50 + (i - 20) * 0.05), .5f});
        for (int i = 0; i < v.samples.size(); ++i)
            QVERIFY(v.path()->contains(v.point(i)));
    }

    void eraserHitsWholeStroke() {
        auto s = line({10, 10}, {100, 10});
        QVERIFY(Ink::hits(*s, {50, 0}, {50, 20}));
        QVERIFY(Ink::hits(*s, {50, 16}, {50, 16})); // 5 pt eraser + 1.25 half width
        QVERIFY(!Ink::hits(*s, {50, 20}, {60, 20}));
    }

    void simplifyKeepsShape() {
        auto s = line({0, 0}, {100, 0});
        for (auto &p : s->samples) p.p = 0.5f;
        const auto simple = Ink::simplify(s->samples, Tool::Pen, 2.5f);
        QCOMPARE(simple.size(), 2);
    }

    void strokeJsonRoundTrip() {
        auto s = line({1.234, 5.678}, {50, 60}, Tool::Highlighter, 0.7f);
        s->color = QColor("#ffd60a");
        auto back = Ink::fromJson(Ink::toJson(*s));
        QVERIFY(back);
        QCOMPARE(back->id, s->id);
        QCOMPARE(back->tool, Tool::Highlighter);
        QCOMPARE(back->color, s->color);
        QCOMPARE(back->samples.size(), s->samples.size());
        QCOMPARE(back->samples[0].x, 1.23f);
        QVERIFY(qAbs(back->samples[3].p - s->samples[3].p) < 0.001f);
    }

    // --- Document & history ---------------------------------------------
    void newNoteKeepsTrailingBlank() {
        std::unique_ptr<Document> doc(Document::createNote());
        QCOMPARE(doc->pageCount(), 1);
        QVERIFY(doc->needsSaveAs()); // Only a never-saved note asks where.
        doc->addStroke(doc->page(0), line({10, 10}, {50, 50}));
        QCOMPARE(doc->pageCount(), 2);
        QVERIFY(!doc->page(1)->hasInk());
        QVERIFY(doc->dirty());
    }

    void undoRedoTracksDirty() {
        std::unique_ptr<Document> doc(Document::createNote());
        auto page = doc->page(0);
        auto s = line({10, 10}, {50, 50});
        doc->addStroke(page, s);
        doc->beginErase();
        doc->eraseLive(page, s);
        doc->endErase();
        QVERIFY(page->strokes.isEmpty());
        doc->undo();
        QCOMPARE(page->strokes.size(), 1);
        doc->undo();
        QVERIFY(!doc->dirty());
        QVERIFY(!doc->canUndo());
        doc->redo();
        QCOMPARE(page->strokes.size(), 1);
        QVERIFY(doc->clearPage(page));
        QVERIFY(page->strokes.isEmpty());
        doc->undo();
        QCOMPARE(page->strokes.size(), 1);
        const int before = doc->pageCount();
        doc->insertPage(1);
        QCOMPARE(doc->pageCount(), before + 1);
        doc->undo();
        QCOMPARE(doc->pageCount(), before);
    }

    void undoRedoReachableFromQml() {
        // The toolbar and Ctrl+Z call these from QML.
        const QMetaObject *mo = &Document::staticMetaObject;
        QVERIFY(mo->indexOfMethod("undo()") >= 0);
        QVERIFY(mo->indexOfMethod("redo()") >= 0);
        std::unique_ptr<Document> doc(Document::createNote());
        doc->addStroke(doc->page(0), line({10, 10}, {50, 50}));
        QVERIFY(QMetaObject::invokeMethod(doc.get(), "undo"));
        QVERIFY(doc->page(0)->strokes.isEmpty());
        QVERIFY(QMetaObject::invokeMethod(doc.get(), "redo"));
        QCOMPARE(doc->page(0)->strokes.size(), 1);
    }

    void appendPaperStaysOutOfHistory() {
        std::unique_ptr<Document> doc(Document::createNote());
        doc->appendPaper();
        QCOMPARE(doc->pageCount(), 2);
        QVERIFY(!doc->dirty());
        QVERIFY(!doc->canUndo());
        QCOMPARE(doc->saveRange(), std::make_pair(0, 0)); // Trimmed on save.
    }

    void unusedPaperIsTrimmedFromTheEnd() {
        std::unique_ptr<Document> doc(Document::createNote());
        for (int i = 0; i < 4; ++i)
            doc->appendPaper();
        doc->addStroke(doc->page(2), line({10, 10}, {50, 50})); // Ink on sheet 3.
        // Sheets 4..5 (plus the trailing one) are blank paper at the end.
        QVERIFY(doc->trimPaperAfter(0));
        QCOMPARE(doc->pageCount(), 3);       // Stops at the inked sheet.
        QVERIFY(doc->page(2)->hasInk());
        QVERIFY(!doc->trimPaperAfter(0));    // Blank sheets before ink stay.
        std::unique_ptr<Document> fresh(Document::createNote());
        fresh->appendPaper();
        QVERIFY(fresh->trimPaperAfter(0));
        QCOMPARE(fresh->pageCount(), 1);     // Never below one page.
        QVERIFY(!fresh->trimPaperAfter(0));
    }

    // --- Trimming ---------------------------------------------------------
    void trimsLeadingAndTrailingBlanks() {
        QTemporaryDir dir;
        std::unique_ptr<Document> doc(Document::createNote());
        while (doc->pageCount() < 6)
            doc->insertPage(doc->pageCount());
        doc->addStroke(doc->page(2), line({10, 10}, {50, 50}));
        doc->addStroke(doc->page(4), line({10, 10}, {50, 50}));
        // Pages: blank blank ink blank ink blank (+ no auto page: 5 is blank).
        QCOMPARE(doc->saveRange(), std::make_pair(2, 4));
        const QString path = dir.path() + "/trim.pdf";
        QString error;
        QVERIFY2(doc->save(path, &error), qPrintable(error));
        QCOMPARE(popplerPages(path), 3);
        // Memory follows the file: leading blanks gone, view stays consistent.
        QVERIFY(doc->page(0)->hasInk());
        QVERIFY(!doc->page(1)->hasInk());
        QVERIFY(doc->page(2)->hasInk());
        QVERIFY(!doc->dirty());
    }

    void emptyNoteSavesOneBlankPage() {
        QTemporaryDir dir;
        std::unique_ptr<Document> doc(Document::createNote());
        doc->insertPage(1);
        QString error;
        QVERIFY(doc->save(dir.path() + "/empty.pdf", &error));
        QCOMPARE(popplerPages(dir.path() + "/empty.pdf"), 1);
    }

    void externalKeepsOriginalBlankPages() {
        QTemporaryDir dir;
        const QString src = externalPdf(dir.path());
        QString error;
        std::unique_ptr<Document> doc(Document::open(src, &error));
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(doc->pageCount(), 2);
        QVERIFY(!doc->needsSaveAs()); // An existing file saves in place.
        doc->addStroke(doc->page(0), line({72, 120}, {300, 120}));
        doc->insertPage(2); // blank note page at the end, trimmed on save
        QVERIFY(doc->save(dir.path() + "/out.pdf", &error));
        QCOMPARE(popplerPages(dir.path() + "/out.pdf"), 2);
    }

    // --- Round trip ------------------------------------------------------
    void roundTripRestoresAndErasesWithoutGhosts() {
        QTemporaryDir dir;
        const QString src = externalPdf(dir.path());
        const QString out = dir.path() + "/annotated.pdf";
        QString error;
        std::unique_ptr<Document> doc(Document::open(src, &error));
        auto keep = line({72, 400}, {400, 400}, Tool::Pen, 0.8f);
        auto gone = line({72, 500}, {400, 500});
        auto mark = line({72, 92}, {300, 92}, Tool::Highlighter);
        mark->color = QColor("#ffd60a");
        doc->addStroke(doc->page(0), keep);
        doc->addStroke(doc->page(0), gone);
        doc->addStroke(doc->page(0), mark);
        QVERIFY(doc->save(out, &error));

        // Ink shows in a standard viewer, original text still selectable.
        QVERIFY(pageText(out, 0).contains("Hello original text"));
        QVERIFY(pixel(out, 0, {200, 500}).value() < 80);
        const QColor hl = pixel(out, 0, {250, 90});
        QVERIFY2(hl.blue() < 190 && hl.red() > 240, qPrintable(hl.name()));

        std::unique_ptr<Document> again(Document::open(out, &error));
        QVERIFY2(again, qPrintable(error));
        QVERIFY(again->fromOmanote());
        QVERIFY(!again->needsSaveAs());
        QCOMPARE(again->pageCount(), 2);
        auto strokes = again->page(0)->strokes;
        QCOMPARE(strokes.size(), 3);
        QCOMPARE(strokes[0]->id, keep->id);
        QCOMPARE(strokes[0]->samples.size(), keep->samples.size());
        QVERIFY(qAbs(strokes[0]->samples[5].p - keep->samples[5].p) < 0.001f);
        QCOMPARE(strokes[2]->tool, Tool::Highlighter);

        // Opening rewrites nothing: the old ink is hidden through its layer.
        QFile saved(out);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(again->source()->readAll(), saved.readAll());
        auto raw = Poppler::Document::load(again->source()->path());
        QVERIFY(raw->page(0)->renderToImage(72, 72).pixelColor(200, 500).value() < 80);
        TileRenderer::hideInk(*raw);
        QVERIFY(raw->page(0)->renderToImage(72, 72).pixelColor(200, 500).value() > 240);
        QVERIFY(pageText(out, 0).contains("Hello original text"));

        again->beginErase();
        again->eraseLive(again->page(0), strokes[1]);
        again->endErase();
        QVERIFY(again->save(out, &error));
        QVERIFY(pixel(out, 0, {200, 500}).value() > 240); // no ghost
        QVERIFY(pixel(out, 0, {200, 400}).value() < 80);

        std::unique_ptr<Document> third(Document::open(out, &error));
        QCOMPARE(third->page(0)->strokes.size(), 2);
        QVERIFY(pageText(out, 0).contains("Hello original text"));
        QFile f(out);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        const QJsonObject meta = QJsonDocument::fromJson(PdfStore::metadata(bytes)).object();
        QCOMPARE(meta.value("version").toInt(), 2);
        QCOMPARE(meta.value("pages").toArray()[0].toObject().value("strokes").toArray().size(), 2);
        // Saving the same document repeatedly must not grow it (no stacked overlays).
        QVERIFY(third->save(dir.path() + "/again.pdf", &error));
        QVERIFY(QFileInfo(dir.path() + "/again.pdf").size() < bytes.size() + 512);
    }

    void noteRoundTripKeepsInfinitePaper() {
        QTemporaryDir dir;
        std::unique_ptr<Document> doc(Document::createNote());
        doc->addStroke(doc->page(0), line({50, 50}, {200, 80}));
        QString error;
        const QString path = dir.path() + "/note.pdf";
        QVERIFY(doc->save(path, &error));
        QCOMPARE(popplerPages(path), 1);
        std::unique_ptr<Document> again(Document::open(path, &error));
        QVERIFY(again);
        QVERIFY(!again->isExternal());
        QCOMPARE(again->pageCount(), 2); // ink on page 1, fresh paper below
        QVERIFY(again->page(0)->note);
    }

    void rotatedPageInkLandsWhereDrawn() {
        QTemporaryDir dir;
        // A landscape page made by rotating portrait A4 by 90°.
        std::unique_ptr<Document> base(Document::createNote());
        QString error;
        const QString plain = dir.path() + "/plain.pdf";
        QVERIFY(base->save(plain, &error));
        {
            QPDF q;
            q.processFile(plain.toLocal8Bit().constData());
            QPDFPageDocumentHelper(q).getAllPages()[0].getObjectHandle().replaceKey("/Rotate", QPDFObjectHandle::newInteger(90));
            QPDFWriter w(q, (dir.path() + "/rot.pdf").toLocal8Bit().constData());
            w.write();
        }
        std::unique_ptr<Document> doc(Document::open(dir.path() + "/rot.pdf", &error));
        QVERIFY2(doc, qPrintable(error));
        QVERIFY(doc->page(0)->size.width() > doc->page(0)->size.height());
        doc->addStroke(doc->page(0), line({100, 100}, {300, 100}));
        QVERIFY(doc->save(dir.path() + "/rot-out.pdf", &error));
        QVERIFY(pixel(dir.path() + "/rot-out.pdf", 0, {200, 100}).value() < 80);
        QVERIFY(pixel(dir.path() + "/rot-out.pdf", 0, {200, 300}).value() > 240);
    }

    void reportsUnreadableFiles() {
        QTemporaryDir dir;
        write(dir.path() + "/bad.pdf", "not a pdf");
        QString error;
        QVERIFY(!Document::open(dir.path() + "/bad.pdf", &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!Document::open(dir.path() + "/missing.pdf", &error));
    }

    void samplesCodecRoundTrips() {
        QVector<Sample> in{{0, 0, 0.5f}, {12.34f, -5.67f, 1.f}, {595.28f, 842.f, 0.f}, {595.27f, 841.99f, 0.333f}};
        const QString data = Ink::encodeSamples(in);
        const auto out = Ink::decodeSamples(data);
        QCOMPARE(out.size(), in.size());
        for (int i = 0; i < in.size(); ++i) {
            QVERIFY(qAbs(out[i].x - in[i].x) < 0.006f);
            QVERIFY(qAbs(out[i].y - in[i].y) < 0.006f);
            QVERIFY(qAbs(out[i].p - in[i].p) < 0.0006f);
        }
        // Real strokes move in small steps: ~5 bytes a sample before base64.
        QVector<Sample> dense;
        for (int i = 0; i < 200; ++i)
            dense.append({float(100 + i * 0.7), float(200 + 5 * std::sin(i * 0.2)), float(0.5 + 0.1 * std::sin(i * 0.1))});
        QVERIFY(Ink::encodeSamples(dense).size() < dense.size() * 7); // v1 JSON: ~20
        // Format 1 still loads.
        auto v1 = Ink::fromJson(QJsonObject{{"id", "7"}, {"tool", "pen"}, {"color", "#000000"}, {"width", 2.5},
                                            {"points", QJsonArray{1, 2, 0.5, 3, 4, 0.6}}});
        QVERIFY(v1);
        QCOMPARE(v1->samples.size(), 2);
    }

    void boundsCoverOutline() {
        auto s = line({10, 10}, {200, 120}, Tool::Pen, 1.0f);
        const QRectF fromPath = s->path()->boundingRect();
        s->finalize();
        QVERIFY(s->bounds().contains(fromPath));
        s->releasePath();
        QVERIFY(s->path()); // Rebuilt on demand.
    }

    void historyIsBounded() {
        std::unique_ptr<Document> doc(Document::createNote());
        for (int i = 0; i < Document::MaxHistory + 20; ++i)
            doc->addStroke(doc->page(0), line({10, 10 + i * 0.1}, {50, 50}));
        int undos = 0;
        while (doc->canUndo()) { doc->undo(); ++undos; }
        QCOMPARE(undos, Document::MaxHistory);
        QVERIFY(doc->dirty()); // The oldest strokes can't be undone, so it's still changed.
    }

    void saveJobKeepsNewInkDirty() {
        QTemporaryDir dir;
        std::unique_ptr<Document> doc(Document::createNote());
        doc->addStroke(doc->page(0), line({10, 10}, {50, 50}));
        const SaveJob job = doc->prepareSave();
        doc->addStroke(doc->page(0), line({60, 60}, {90, 90})); // Drawn while saving.
        QString error;
        QVERIFY(Document::writeJob(job, dir.path() + "/a.pdf", &error));
        doc->finishSave(job, dir.path() + "/a.pdf");
        QVERIFY(doc->dirty());
        QVERIFY(!doc->needsSaveAs());
        std::unique_ptr<Document> back(Document::open(dir.path() + "/a.pdf", &error));
        QCOMPARE(back->page(0)->strokes.size(), 1);
    }

    void savingDoesNotPinOutlines() {
        // Saving builds every stroke's outline; off-screen ones mustn't stay
        // in memory afterwards.
        QTemporaryDir dir;
        std::unique_ptr<Document> doc(Document::createNote());
        auto s = line({10, 10}, {200, 80});
        doc->addStroke(doc->page(0), s);
        s->releasePath();
        QString error;
        QVERIFY(doc->save(dir.path() + "/a.pdf", &error));
        QVERIFY(!s->hasCachedPath());
        std::unique_ptr<Document> back(Document::open(dir.path() + "/a.pdf", &error));
        QCOMPARE(back->page(0)->strokes.size(), 1);
    }

    void tileGridCoversPage() {
        const QSizeF page(595.28, 842);
        const double scale = 1.7;
        const QSize grid = TileRenderer::gridSize(page, scale);
        qint64 area = 0;
        for (int y = 0; y < grid.height(); ++y)
            for (int x = 0; x < grid.width(); ++x)
                area += qint64(TileRenderer::tilePixels(page, scale, x, y).width()) *
                        TileRenderer::tilePixels(page, scale, x, y).height();
        QCOMPARE(area, qint64(std::ceil(page.width() * scale)) * qint64(std::ceil(page.height() * scale)));
    }

    void rendererDrawsTilesOffThread() {
        TileRenderer renderer(2);
        QSignalSpy spy(&renderer, &TileRenderer::rendered);
        auto s = line({10, 10}, {200, 10}, Tool::Pen, 1.0f);
        s->finalize();
        renderer.setJobs({TileRenderer::Job{1, -1, {595.28, 842}, 1.0, 0, 0, 3, {s}},
                          TileRenderer::Job{1, -1, {595.28, 842}, 1.0, 1, 1, 3, {s}}});
        QTRY_COMPARE(spy.size(), 2);
        for (const auto &args : spy) {
            const QImage img = args.at(5).value<QImage>();
            const int tx = args.at(2).toInt();
            QCOMPARE(img.size(), TileRenderer::tilePixels({595.28, 842}, 1.0, tx, args.at(3).toInt()).size());
            if (tx == 0)
                QVERIFY(img.pixelColor(100, 10).value() < 80);
        }
    }

    void rendererReleasesCachesWhenIdle() {
        QTemporaryDir dir;
        QString error;
        const SourcePtr source = PdfSource::open(externalPdf(dir.path()), error);
        QVERIFY(source);
        TileRenderer renderer(2);
        renderer.setPdf(source);
        QSignalSpy spy(&renderer, &TileRenderer::rendered);
        const TileRenderer::Job job{1, 0, {595.28, 842}, 1.0, 0, 0, 1, {}};
        renderer.setJobs({job});
        QTRY_COMPARE(spy.size(), 1);
        QVERIFY(renderer.cachedBytes() > 0);
        renderer.releaseIdle();
        QCOMPARE(renderer.cachedBytes(), 0);
        // Cold again: the worker reopens the PDF and renders the same pixels.
        renderer.setJobs({TileRenderer::Job{1, 0, {595.28, 842}, 1.0, 0, 0, 2, {}}});
        QTRY_COMPARE(spy.size(), 2);
        QCOMPARE(spy[1].at(5).value<QImage>(), spy[0].at(5).value<QImage>());
    }

    void rendererSkipsPlainWhiteTiles() {
        QTemporaryDir dir;
        QString error;
        const SourcePtr source = PdfSource::open(externalPdf(dir.path()), error);
        QVERIFY(source);
        TileRenderer renderer(2);
        renderer.setPdf(source);
        QSignalSpy spy(&renderer, &TileRenderer::rendered);
        // The second page is blank: no pixels are worth a texture or a cache.
        renderer.setJobs({TileRenderer::Job{2, 1, {595.28, 842}, 1.0, 0, 0, 1, {}}});
        QTRY_COMPARE(spy.size(), 1);
        QVERIFY(spy[0].at(5).value<QImage>().isNull());
        QCOMPARE(renderer.cachedBytes(), 0);
        // Ink on that white paper still gets a real tile.
        auto s = line({10, 10}, {200, 10}, Tool::Pen, 1.0f);
        s->finalize();
        renderer.setJobs({TileRenderer::Job{2, 1, {595.28, 842}, 1.0, 0, 0, 2, {s}}});
        QTRY_COMPARE(spy.size(), 2);
        const QImage inked = spy[1].at(5).value<QImage>();
        QVERIFY(!inked.isNull());
        QVERIFY(inked.pixelColor(100, 10).value() < 80);
        QVERIFY(inked.pixelColor(100, 100).value() > 240);
        // A tile with PDF content is kept as before.
        renderer.setJobs({TileRenderer::Job{1, 0, {595.28, 842}, 1.0, 0, 0, 1, {}}});
        QTRY_COMPARE(spy.size(), 3);
        QVERIFY(!spy[2].at(5).value<QImage>().isNull());
        QVERIFY(renderer.cachedBytes() > 0);
    }

    // --- Viewport ----------------------------------------------------------
    void viewportLocksAndRecenters() {
        Viewport v;
        v.setViewSize({1000, 800});
        v.setPages({{595, 842}, {595, 842}});
        v.zoomAt(1.0, {500, 400});
        v.panBy(200, 0);
        QCOMPARE(v.panX(), 0.0);
        v.zoomAt(3.0, {800, 400});
        QVERIFY(!v.horizontallyLocked());
        QVERIFY(v.panX() != 0);
        QVERIFY(qAbs(v.panX()) <= v.maxPanX());
        v.panBy(10000, 0);
        QCOMPARE(v.panX(), v.maxPanX());
        v.zoomAt(2.0, {500, 400});
        QCOMPARE(v.panX(), v.maxPanX()); // clamped inward as it shrinks
        v.zoomAt(1.0, {900, 400});
        QCOMPARE(v.panX(), 0.0);
        v.panBy(0, 100000);
        // A whole first page of room above page one: its top can reach
        // the bottom of the view and beyond.
        QCOMPARE(v.scrollY(), -842 * v.zoom());
        QVERIFY(v.toScreen(v.pageDocRect(0).topLeft()).y() > 800);
        v.panBy(0, -100000);
        QCOMPARE(v.scrollY(), v.maxScrollY());
        QCOMPARE(v.zoom(), 1.0);
        v.zoomAt(100, {0, 0});
        QCOMPARE(v.zoom(), Viewport::MaxZoom);
    }

    void viewportMapsPages() {
        Viewport v;
        v.setViewSize({1000, 800});
        v.setPages({{595, 842}, {595, 842}});
        v.zoomAt(1.0, {0, 0});
        QPointF local;
        QCOMPARE(v.pageAt(v.toScreen({0, 100}), &local), 0);
        QVERIFY(qAbs(local.x() - 297.5) < 1e-6);
        QCOMPARE(v.pageAt(v.toScreen({0, 842 + Viewport::Gap / 2})), -1);
        QCOMPARE(v.pageAt(v.toScreen({0, 900})), 1);
        QCOMPARE(v.pageAt(v.toScreen({-400, 100})), -1);
    }

    // --- Theme & i18n ------------------------------------------------------
    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QVERIFY(theme.dark());
    }

    void themeFollowsColorsLive() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "accent = \"#112233\"\nbackground = \"#1a1b26\"\nforeground = \"#c0caf5\"\nmode = \"dark\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
        QVERIFY(theme.dark());
        write(dir.path() + "/theme/colors.toml",
              "accent = \"#005C32\"\nbackground = \"#DEDBC8\"\nforeground = \"#0c0c14\"\nmode = \"light\"\n");
        QTRY_VERIFY(!theme.dark());
        QCOMPARE(theme.background(), QString("#DEDBC8"));
        QVERIFY(Theme::luminance(theme.backdrop()) < Theme::luminance("#ffffff"));
    }

    void themeFollowsHyprlandRounding() {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");
        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBack() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open PDF": "Abrir PDF"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open PDF"), QString("Abrir PDF"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorPlurals() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/ru.json", R"({"%n pages": {"one": "%n страница", "few": "%n страницы", "many": "%n страниц"}})");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n pages", nullptr, 21), QString("%n страница"));
        QCOMPARE(translator.translate("Main", "%n pages", nullptr, 3), QString("%n страницы"));
        QCOMPARE(translator.translate("Main", "%n pages", nullptr, 5), QString("%n страниц"));
    }

    void shippedCatalogsAreValid() {
        QDir dir(QStringLiteral(SRCDIR) + "/i18n");
        const auto files = dir.entryList({"*.json"});
        QVERIFY(files.contains("da.json"));
        for (const auto &name : files) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::ReadOnly));
            QJsonParseError err;
            QJsonDocument::fromJson(f.readAll(), &err);
            QVERIFY2(err.error == QJsonParseError::NoError, qPrintable(name + ": " + err.errorString()));
        }
    }
};

QTEST_MAIN(OmanoteTests)
#include "omanote_tests.moc"
