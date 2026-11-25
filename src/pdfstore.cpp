#include "pdfstore.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryFile>
#include <cstdio>
#include <unistd.h>
#include <algorithm>

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

PdfSource::~PdfSource() = default;

SourcePtr PdfSource::open(const QString &path, QString &error) {
    auto file = std::make_unique<QFile>(path);
    if (!file->exists()) {
        error = QObject::tr("File not found: %1").arg(path);
        return nullptr;
    }
    if (!file->open(QIODevice::ReadOnly)) {
        error = QObject::tr("Can't read %1: %2").arg(path, file->errorString());
        return nullptr;
    }
    auto source = std::shared_ptr<PdfSource>(new PdfSource);
    source->m_file = std::move(file);
    return source;
}

SourcePtr PdfSource::fromBytes(const QByteArray &bytes) {
    auto file = std::make_unique<QTemporaryFile>();
    if (!file->open() || file->write(bytes) != bytes.size() || !file->flush())
        return nullptr;
    auto source = std::shared_ptr<PdfSource>(new PdfSource);
    source->m_file = std::move(file);
    return source;
}

QString PdfSource::path() const { return QStringLiteral("/proc/self/fd/%1").arg(m_file->handle()); }

QByteArray PdfSource::readAll() const {
    QFile f(path());
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

namespace PdfStore {
namespace {
using QOH = QPDFObjectHandle;

// Every object Omanote adds carries this key, so stripping finds exactly ours.
const std::string Marker = "/Omanote";
const std::string FormName = "/OmanoteInk";

bool ours(QOH o) {
    if (o.isStream())
        return o.getDict().hasKey(Marker);
    return o.isDictionary() && o.hasKey(Marker);
}

QOH real(double v) { return QOH::newReal(v, 4, true); }

QOH rectangle(double a, double b, double c, double d) {
    QOH r = QOH::newArray();
    r.appendItem(real(a)); r.appendItem(real(b)); r.appendItem(real(c)); r.appendItem(real(d));
    return r;
}

// How a page is shown: its visible box and rotation, as the matrix taking
// displayed coordinates (top-left origin, y down, rotated like the viewer
// shows it) into PDF user space.
struct Geometry {
    QSizeF size;
    double m[6];
};

QPDFObjectHandle::Rectangle box(QOH o) {
    if (o.isArray() && o.getArrayNItems() == 4) {
        auto r = o.getArrayAsRectangle();
        return {std::min(r.llx, r.urx), std::min(r.lly, r.ury), std::max(r.llx, r.urx), std::max(r.lly, r.ury)};
    }
    return {0, 0, 612, 792};
}

Geometry geometry(QPDFPageObjectHelper &page) {
    const auto media = box(page.getMediaBox());
    auto crop = box(page.getCropBox(false, false));
    // Viewers show the crop box clipped to the media box.
    crop = {std::max(crop.llx, media.llx), std::max(crop.lly, media.lly), std::min(crop.urx, media.urx),
            std::min(crop.ury, media.ury)};
    if (crop.urx <= crop.llx || crop.ury <= crop.lly)
        crop = media;
    int rotate = 0;
    QOH r = page.getAttribute("/Rotate", false);
    if (r.isInteger())
        rotate = ((r.getIntValueAsInt() % 360) + 360) % 360 / 90 * 90;
    const double x0 = crop.llx, y0 = crop.lly, x1 = crop.urx, y1 = crop.ury, w = x1 - x0, h = y1 - y0;
    Geometry g{rotate % 180 ? QSizeF(h, w) : QSizeF(w, h), {}};
    const double m0[] = {1, 0, 0, -1, x0, y1}, m90[] = {0, 1, 1, 0, x0, y0}, m180[] = {-1, 0, 0, 1, x1, y0},
                 m270[] = {0, -1, -1, 0, x1, y1};
    const double *m = rotate == 90 ? m90 : rotate == 180 ? m180 : rotate == 270 ? m270 : m0;
    std::copy(m, m + 6, g.m);
    return g;
}

QByteArray streamBytes(QOH stream) {
    auto buf = stream.getStreamData(qpdf_dl_generalized);
    return buf ? QByteArray(reinterpret_cast<const char *>(buf->getBuffer()), qsizetype(buf->getSize())) : QByteArray();
}

QOH ourInfo(QPDF &q) {
    QOH piece = q.getRoot().getKey("/PieceInfo");
    return piece.isDictionary() ? piece.getKey(Marker) : QOH::newNull();
}

QByteArray readMeta(QPDF &q) {
    QOH info = ourInfo(q);
    if (!info.isDictionary())
        return {};
    QOH data = info.getKey("/Private");
    return data.isStream() ? streamBytes(data) : QByteArray();
}

void removeOurs(QOH array) {
    if (!array.isArray())
        return;
    for (int i = array.getArrayNItems() - 1; i >= 0; --i)
        if (ours(array.getArrayItem(i)))
            array.eraseItem(i);
}

// Removes every trace of Omanote ink and metadata, leaving the PDF as it was
// before Omanote first touched it.
void strip(QPDF &q) {
    QPDFPageDocumentHelper pages(q);
    for (auto &page : pages.getAllPages()) {
        QOH dict = page.getObjectHandle();
        QOH contents = dict.getKey("/Contents");
        if (contents.isArray()) {
            std::vector<QOH> kept;
            for (auto &item : contents.getArrayAsVector())
                if (!ours(item))
                    kept.push_back(item);
            dict.replaceKey("/Contents", QOH::newArray(kept));
        } else if (ours(contents)) {
            dict.replaceKey("/Contents", q.newStream(std::string()));
        }
        QOH resources = dict.getKey("/Resources");
        if (resources.isDictionary()) {
            QOH xobjects = resources.getKey("/XObject");
            if (xobjects.isDictionary() && xobjects.hasKey(FormName))
                xobjects.removeKey(FormName);
        }
    }

    QOH root = q.getRoot();
    QOH oc = root.getKey("/OCProperties");
    if (oc.isDictionary()) {
        removeOurs(oc.getKey("/OCGs"));
        QOH d = oc.getKey("/D");
        if (d.isDictionary())
            for (const char *key : {"/ON", "/OFF", "/Order", "/Locked"})
                removeOurs(d.getKey(key));
        QOH ocgs = oc.getKey("/OCGs");
        if (oc.hasKey(Marker) && (!ocgs.isArray() || ocgs.getArrayNItems() == 0))
            root.removeKey("/OCProperties");
    }
    QOH piece = root.getKey("/PieceInfo");
    if (piece.isDictionary()) {
        piece.removeKey(Marker);
        if (piece.getKeys().empty())
            root.removeKey("/PieceInfo");
    }
}

// Viewers (and poppler's layer model) list layers from /D /Order.
bool layerToggleable(QPDF &q) {
    QOH oc = q.getRoot().getKey("/OCProperties");
    if (!oc.isDictionary() || !oc.getKey("/D").isDictionary())
        return false;
    QOH order = oc.getKey("/D").getKey("/Order");
    if (!order.isArray())
        return false;
    for (auto &item : order.getArrayAsVector())
        if (ours(item))
            return true;
    return false;
}

QByteArray writeMemory(QPDF &q) {
    QPDFWriter writer(q);
    writer.setOutputMemory();
    // Object streams pack the many small dictionaries tightly.
    writer.setObjectStreamMode(qpdf_o_generate);
    writer.write();
    auto buf = writer.getBufferSharedPointer();
    return QByteArray(reinterpret_cast<const char *>(buf->getBuffer()), qsizetype(buf->getSize()));
}

QByteArray rgb(const QColor &c) {
    return QByteArray::number(c.redF(), 'f', 3) + ' ' + QByteArray::number(c.greenF(), 'f', 3) + ' ' +
           QByteArray::number(c.blueF(), 'f', 3) + " rg\n";
}

// Registers the "Omanote Ink" layer so viewers can toggle it.
QOH layer(QPDF &q) {
    QOH ocg = QOH::newDictionary();
    ocg.replaceKey("/Type", QOH::newName("/OCG"));
    ocg.replaceKey("/Name", QOH::newUnicodeString(LayerName));
    ocg.replaceKey(Marker, QOH::newBool(true));
    ocg = q.makeIndirectObject(ocg);

    QOH root = q.getRoot();
    QOH oc = root.getKey("/OCProperties");
    if (!oc.isDictionary()) {
        oc = QOH::newDictionary();
        oc.replaceKey(Marker, QOH::newBool(true));
        root.replaceKey("/OCProperties", oc);
    }
    if (!oc.getKey("/OCGs").isArray())
        oc.replaceKey("/OCGs", QOH::newArray());
    oc.getKey("/OCGs").appendItem(ocg);
    if (!oc.getKey("/D").isDictionary())
        oc.replaceKey("/D", QOH::newDictionary());
    QOH d = oc.getKey("/D");
    if (d.getKey("/BaseState").isName() && d.getKey("/BaseState").getName() == "/OFF") {
        if (!d.getKey("/ON").isArray())
            d.replaceKey("/ON", QOH::newArray());
        d.getKey("/ON").appendItem(ocg);
    }
    if (d.getKey("/Order").isArray())
        d.getKey("/Order").appendItem(ocg);
    else if (!d.hasKey("/Order"))
        d.replaceKey("/Order", QOH::newArray(std::vector<QOH>{ocg}));
    return ocg;
}

void addInk(QPDF &q, QPDFPageObjectHelper &page, const QList<StrokePtr> &strokes, QOH ocg) {
    const Geometry g = geometry(page);
    const double w = g.size.width(), h = g.size.height();

    // Same outlines the screen fills, highlighters first with multiply so
    // text beneath stays crisp, pens on top. Clipped to the page.
    QByteArray content = "q\n0 0 " + QByteArray::number(w, 'f', 2) + ' ' + QByteArray::number(h, 'f', 2) +
                         " re W n\n";
    for (const auto &s : strokes)
        if (s->tool == Tool::Highlighter)
            content += "q /OmHL gs\n" + rgb(s->color) + Ink::pdfPath(*s->path()) + "f\nQ\n";
    for (const auto &s : strokes)
        if (s->tool != Tool::Highlighter)
            content += rgb(s->color) + Ink::pdfPath(*s->path()) + "f\n";
    content += "Q\n";

    QOH form = q.newStream(content.toStdString());
    QOH fd = form.getDict();
    fd.replaceKey("/Type", QOH::newName("/XObject"));
    fd.replaceKey("/Subtype", QOH::newName("/Form"));
    fd.replaceKey("/BBox", rectangle(0, 0, w, h));
    QOH matrix = QOH::newArray();
    for (double v : g.m)
        matrix.appendItem(real(v));
    fd.replaceKey("/Matrix", matrix);
    QOH gs = QOH::newDictionary();
    gs.replaceKey("/Type", QOH::newName("/ExtGState"));
    gs.replaceKey("/BM", QOH::newName("/Multiply"));
    gs.replaceKey("/ca", real(Ink::HighlightAlpha));
    QOH gsDict = QOH::newDictionary();
    gsDict.replaceKey("/OmHL", gs);
    QOH res = QOH::newDictionary();
    res.replaceKey("/ExtGState", gsDict);
    fd.replaceKey("/Resources", res);
    fd.replaceKey("/OC", ocg);
    fd.replaceKey(Marker, QOH::newBool(true));

    // Resources are often shared between pages; copy before adding ours so
    // one page's ink never shows up on another.
    QOH dict = page.getObjectHandle();
    QOH resources = dict.getKey("/Resources");
    resources = resources.isDictionary() ? resources.shallowCopy() : QOH::newDictionary();
    QOH xobjects = resources.getKey("/XObject");
    xobjects = xobjects.isDictionary() ? xobjects.shallowCopy() : QOH::newDictionary();
    xobjects.replaceKey(FormName, form);
    resources.replaceKey("/XObject", xobjects);
    dict.replaceKey("/Resources", resources);

    // Wrap the original content in q/Q so whatever state it leaves behind
    // can't skew the ink, then draw the ink form last.
    QOH head = q.newStream("q\n");
    head.getDict().replaceKey(Marker, QOH::newBool(true));
    QOH tail = q.newStream("Q\nq " + FormName + " Do Q\n");
    tail.getDict().replaceKey(Marker, QOH::newBool(true));
    page.addPageContents(head, true);
    page.addPageContents(tail, false);
}

QPDFPageObjectHelper newPage(QPDF &q, QSizeF size) {
    QOH page = QOH::newDictionary();
    page.replaceKey("/Type", QOH::newName("/Page"));
    page.replaceKey("/MediaBox", rectangle(0, 0, size.width(), size.height()));
    page.replaceKey("/Resources", QOH::newDictionary());
    page.replaceKey("/Contents", q.newStream(std::string()));
    return QPDFPageObjectHelper(q.makeIndirectObject(page));
}

QString describe(const std::exception &e) {
    const QString what = QString::fromLocal8Bit(e.what());
    if (what.contains("password", Qt::CaseInsensitive))
        return QObject::tr("This PDF is password protected");
    return QObject::tr("Not a readable PDF (%1)").arg(what);
}
}

bool load(const QString &path, Loaded &out, QString &error) {
    const SourcePtr source = PdfSource::open(path, error);
    if (!source)
        return false;
    try {
        // qpdf reads lazily from the file: only the parts it needs.
        QPDF q;
        q.setSuppressWarnings(true);
        q.processFile(source->path().toLocal8Bit().constData());
        QPDFPageDocumentHelper helper(q);
        auto pages = helper.getAllPages();
        if (pages.empty()) {
            error = QObject::tr("The PDF has no pages");
            return false;
        }

        QJsonArray metaPages;
        const QJsonObject meta = QJsonDocument::fromJson(readMeta(q)).object();
        const bool omanote = meta.value("format").toString() == "omanote" &&
                             meta.value("version").toInt() >= 1 && meta.value("version").toInt() <= FormatVersion;
        if (omanote)
            metaPages = meta.value("pages").toArray();
        // Ink only maps back if the page structure is the one we wrote; if
        // another tool reshuffled pages, keep the PDF and drop stale strokes.
        const bool mapped = omanote && metaPages.size() == int(pages.size());

        out = Loaded{};
        out.omanote = omanote;
        out.origin = omanote ? meta.value("origin").toString("external") : "external";
        for (size_t i = 0; i < pages.size(); ++i) {
            PageData data;
            data.size = geometry(pages[i]).size;
            data.source = int(i);
            data.note = false;
            if (mapped) {
                const QJsonObject p = metaPages[int(i)].toObject();
                data.note = p.value("note").toBool();
                for (const auto &s : p.value("strokes").toArray())
                    if (auto stroke = Ink::fromJson(s.toObject()))
                        data.strokes.append(stroke);
            }
            out.pages.append(data);
        }
        // Old ink is hidden at render time through its layer. Only if a
        // foreign tool broke the layer wiring is a stripped copy needed.
        if ((omanote || ourInfo(q).isDictionary()) && !layerToggleable(q)) {
            strip(q);
            out.source = PdfSource::fromBytes(writeMemory(q));
        } else {
            out.source = source;
        }
        return true;
    } catch (const std::exception &e) {
        error = describe(e);
        return false;
    }
}

bool save(const SourcePtr &source, const QList<PageData> &pages, const QString &origin, const QString &path,
          QString &error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = QObject::tr("Couldn't save %1: %2").arg(path, file.errorString());
        return false;
    }
    try {
        QPDF q;
        q.setSuppressWarnings(true);
        if (source)
            q.processFile(source->path().toLocal8Bit().constData());
        else
            q.emptyPDF();
        strip(q);

        QPDFPageDocumentHelper helper(q);
        helper.pushInheritedAttributesToPage();
        auto originals = helper.getAllPages();

        QOH ocg;
        bool hasInk = false;
        for (const auto &p : pages)
            hasInk = hasInk || !p.strokes.isEmpty();
        if (hasInk)
            ocg = layer(q);

        QJsonArray metaPages;
        std::vector<QPDFPageObjectHelper> result;
        for (const auto &p : pages) {
            QPDFPageObjectHelper page = p.source >= 0 && p.source < int(originals.size())
                                            ? originals[size_t(p.source)] : newPage(q, p.size);
            if (!p.strokes.isEmpty())
                addInk(q, page, p.strokes, ocg);
            result.push_back(page);

            QJsonArray strokes;
            for (const auto &s : p.strokes)
                strokes.append(Ink::toJson(*s));
            metaPages.append(QJsonObject{{"note", p.note}, {"strokes", strokes}});
        }
        for (auto &page : originals)
            helper.removePage(page);
        for (auto &page : result)
            helper.addPage(page, false);

        const QJsonObject meta{{"format", "omanote"},
                               {"version", FormatVersion},
                               {"origin", origin},
                               {"pages", metaPages}};
        QOH data = q.newStream(QJsonDocument(meta).toJson(QJsonDocument::Compact).toStdString());
        const QString stamp = "D:" + QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmss") + "Z";
        QOH info = QOH::newDictionary();
        info.replaceKey("/LastModified", QOH::newString(stamp.toStdString()));
        info.replaceKey("/Private", data);
        QOH root = q.getRoot();
        QOH piece = root.getKey("/PieceInfo");
        if (!piece.isDictionary()) {
            piece = QOH::newDictionary();
            root.replaceKey("/PieceInfo", piece);
        }
        piece.replaceKey(Marker, info);
        // PDF requires /LastModified beside /PieceInfo.
        root.replaceKey("/LastModified", QOH::newString(stamp.toStdString()));

        // Stream straight into QSaveFile's temporary file through a second
        // descriptor on it; commit() then renames it over the target.
        FILE *out = fdopen(dup(file.handle()), "wb");
        if (!out)
            throw std::runtime_error("can't open the temporary file");
        QPDFWriter writer(q);
        writer.setOutputFile(path.toLocal8Bit().constData(), out, true);
        writer.setObjectStreamMode(qpdf_o_generate);
        writer.write();
    } catch (const std::exception &e) {
        file.cancelWriting();
        error = QObject::tr("Couldn't build the PDF (%1)").arg(QString::fromLocal8Bit(e.what()));
        return false;
    }
    if (!file.commit()) {
        error = QObject::tr("Couldn't save %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

QByteArray metadata(const QByteArray &pdf) {
    try {
        QPDF q;
        q.setSuppressWarnings(true);
        q.processMemoryFile("document.pdf", pdf.constData(), size_t(pdf.size()));
        return readMeta(q);
    } catch (const std::exception &) {
        return {};
    }
}
}
