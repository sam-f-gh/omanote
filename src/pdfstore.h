#pragma once

#include <QByteArray>
#include <QList>
#include <QSizeF>
#include <QString>

#include "ink.h"

#include <memory>

class QFile;

// The PDF a document came from, as an open file handle rather than bytes in
// RAM. Held open, it pins that exact file even after a save replaces the
// path (the old data stays readable until the document closes), and readers
// (qpdf, each poppler worker) go through /proc/self/fd, so the kernel's page
// cache serves them and the app keeps no copy of the file.
class PdfSource {
public:
    ~PdfSource();
    static std::shared_ptr<const PdfSource> open(const QString &path, QString &error);
    // Spills bytes to an unnamed temporary file (rare fallbacks, tests).
    static std::shared_ptr<const PdfSource> fromBytes(const QByteArray &bytes);
    QString path() const;
    QByteArray readAll() const;

private:
    std::unique_ptr<QFile> m_file;
};
using SourcePtr = std::shared_ptr<const PdfSource>;

// Non-destructive PDF persistence through qpdf. Original page content is
// kept as is; Omanote adds, per page, one tagged Form XObject (in an "Omanote
// Ink" optional-content layer) that any viewer draws, plus versioned stroke
// data in the catalog's /PieceInfo. Every save strips the previous ink first,
// so re-saving replaces it instead of stacking ghost copies on top.
namespace PdfStore {
constexpr int FormatVersion = 2;  // 2: samples as compact varint data.
constexpr char LayerName[] = "Omanote Ink";

struct PageData {
    QSizeF size;               // Displayed size in points (crop box, rotated).
    int source = -1;           // Page index in the source PDF, -1 for new paper.
    bool note = true;          // Omanote's own blank paper.
    QList<StrokePtr> strokes;
};

struct Loaded {
    // The file as is. Old Omanote ink in it sits in the "Omanote Ink" layer,
    // which the renderer hides and every save strips, so nothing is
    // rewritten just to open a file.
    SourcePtr source;
    QList<PageData> pages;
    bool omanote = false;      // Carried Omanote stroke data.
    QString origin = "external";
};

bool load(const QString &path, Loaded &out, QString &error);

// Builds the PDF (pages in order, ink on top, metadata embedded) and writes
// it straight into a temporary file beside the target, then renames it into
// place: atomic, one write of the file, no in-memory copy.
bool save(const SourcePtr &source, const QList<PageData> &pages, const QString &origin, const QString &path,
          QString &error);

// Reads the raw stroke JSON from a PDF, for tests and tooling.
QByteArray metadata(const QByteArray &pdf);
}
