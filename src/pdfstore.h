#pragma once

#include <QByteArray>
#include <QList>
#include <QSizeF>
#include <QString>

#include "ink.h"

// Non-destructive PDF persistence through qpdf. Original page content is
// kept as is; Omanote adds, per page, one tagged Form XObject (in an "Omanote
// Ink" optional-content layer) that any viewer draws, plus versioned stroke
// JSON in the catalog's /PieceInfo. Loading strips both again, so re-saving
// replaces the old ink instead of stacking ghost copies on top of it.
namespace PdfStore {
constexpr int FormatVersion = 1;

struct PageData {
    QSizeF size;               // Displayed size in points (crop box, rotated).
    int source = -1;           // Page index in the source PDF, -1 for new paper.
    bool note = true;          // Omanote's own blank paper.
    QList<StrokePtr> strokes;
};

struct Loaded {
    QByteArray pdf;            // Source with Omanote ink stripped: render and re-save from this.
    QList<PageData> pages;
    bool omanote = false;      // Carried Omanote stroke data.
    QString origin = "external";
};

bool load(const QString &path, Loaded &out, QString &error);
bool loadBytes(const QByteArray &bytes, Loaded &out, QString &error);

// Builds the PDF in memory: pages in order, ink on top, metadata embedded.
bool build(const QByteArray &source, const QList<PageData> &pages, const QString &origin, QByteArray &out,
           QString &error);
// build() then an atomic write (temp file beside the target, then rename).
bool save(const QByteArray &source, const QList<PageData> &pages, const QString &origin, const QString &path,
          QString &error);

// Reads the raw stroke JSON from a PDF, for tests and tooling.
QByteArray metadata(const QByteArray &pdf);
}
