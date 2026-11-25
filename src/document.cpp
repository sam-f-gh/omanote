#include "document.h"

#include <QFileInfo>
#include <algorithm>
#include <QRandomGenerator>

#include "pdfstore.h"

class Command {
public:
    virtual ~Command() = default;
    virtual void redo(Document &doc) = 0;
    virtual void undo(Document &doc) = 0;
};

namespace {
class AddStroke : public Command {
public:
    AddStroke(PagePtr p, StrokePtr s) : page(std::move(p)), stroke(std::move(s)) {}
    void redo(Document &doc) override { page->strokes.append(stroke); doc.touch(page); }
    void undo(Document &doc) override { page->strokes.removeOne(stroke); doc.touch(page); }
    PagePtr page;
    StrokePtr stroke;
};

class EraseStrokes : public Command {
public:
    struct Item { PagePtr page; StrokePtr stroke; int index; };
    void redo(Document &doc) override {
        for (const auto &i : items) { i.page->strokes.removeOne(i.stroke); doc.touch(i.page); }
    }
    void undo(Document &doc) override {
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            it->page->strokes.insert(std::min<int>(it->index, it->page->strokes.size()), it->stroke);
            doc.touch(it->page);
        }
    }
    std::vector<Item> items;
};

class ClearPage : public Command {
public:
    ClearPage(PagePtr p) : page(std::move(p)), strokes(page->strokes) {}
    void redo(Document &doc) override { page->strokes.clear(); doc.touch(page); }
    void undo(Document &doc) override { page->strokes = strokes; doc.touch(page); }
    PagePtr page;
    QList<StrokePtr> strokes;
};

class AddPage : public Command {
public:
    AddPage(PagePtr p, int i) : page(std::move(p)), index(i) {}
    void redo(Document &doc) override { doc.insertPageRaw(index, page); }
    void undo(Document &doc) override { doc.removePageRaw(page); }
    PagePtr page;
    int index;
};

PagePtr blankPage(QSizeF size) {
    auto p = std::make_shared<Page>();
    p->id = Document::newId();
    p->size = size;
    return p;
}
}

Document::Document(QObject *parent) : QObject(parent) {}
Document::~Document() = default;

quint64 Document::newId() {
    // 53 bits so ids survive a round trip through JSON numbers in any tool.
    return QRandomGenerator::global()->generate64() & ((quint64(1) << 53) - 1);
}

Document *Document::createNote(QObject *parent) {
    auto *doc = new Document(parent);
    doc->m_pages.append(blankPage({A4Width, A4Height}));
    return doc;
}

Document *Document::open(const QString &path, QString *error, QObject *parent) {
    PdfStore::Loaded loaded;
    QString message;
    if (!PdfStore::load(path, loaded, message)) {
        if (error) *error = message;
        return nullptr;
    }
    auto *doc = new Document(parent);
    doc->m_source = loaded.pdf;
    doc->m_filePath = QFileInfo(path).absoluteFilePath();
    doc->m_fromOmanote = loaded.omanote;
    doc->m_external = loaded.origin != "note";
    // A file Omanote already wrote is ours to overwrite; anyone else's PDF
    // gets Save As first so the original is never replaced silently.
    doc->m_savedHere = loaded.omanote;
    for (const auto &data : loaded.pages) {
        auto p = std::make_shared<Page>();
        p->id = newId();
        p->size = data.size;
        p->source = data.source;
        p->note = data.note;
        p->strokes = data.strokes;
        doc->m_pages.append(p);
    }
    if (doc->m_pages.isEmpty())
        doc->m_pages.append(blankPage({A4Width, A4Height}));
    doc->ensureTrailingPage();
    return doc;
}

std::pair<int, int> Document::saveRange() const {
    int first = -1, last = -1;
    for (int i = 0; i < m_pages.size(); ++i) {
        if (m_pages[i]->isContent()) {
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first < 0)
        return {0, 0}; // Nothing at all: one blank page, predictably.
    return {first, last};
}

bool Document::save(const QString &path, QString *error) {
    const auto [first, last] = saveRange();
    QList<PdfStore::PageData> out;
    for (int i = first; i <= last; ++i) {
        const auto &p = m_pages[i];
        out.append({p->size, p->source, p->note, p->strokes});
    }
    QString message;
    if (!PdfStore::save(m_source, out, m_external ? "external" : "note", path, message)) {
        if (error) *error = message;
        return false;
    }
    m_filePath = QFileInfo(path).absoluteFilePath();
    m_savedHere = true;
    m_fromOmanote = true;
    if (first > 0) {
        // Leading blanks are gone from the file, so drop them here too. Their
        // history could resurrect ink on pages that no longer exist, so start
        // a fresh history (the file is the new baseline anyway).
        m_pages.erase(m_pages.begin(), m_pages.begin() + first);
        m_history.clear();
        m_index = 0;
        m_pendingErase.reset();
        emit pagesChanged();
    }
    m_savedIndex = m_index;
    emit historyChanged();
    return true;
}

void Document::touch(const PagePtr &page) {
    ++page->revision;
    emit pageChanged(page->id);
}

void Document::insertPageRaw(int index, const PagePtr &page) {
    m_pages.insert(std::clamp<int>(index, 0, m_pages.size()), page);
    emit pagesChanged();
}

void Document::removePageRaw(const PagePtr &page) {
    if (m_pages.size() > 1 && m_pages.removeOne(page))
        emit pagesChanged();
}

void Document::push(std::unique_ptr<Command> command) {
    if (m_index < int(m_history.size())) {
        m_history.resize(m_index);
        if (m_savedIndex > m_index)
            m_savedIndex = -1; // The saved state can no longer be reached.
    }
    m_history.push_back(std::move(command));
    ++m_index;
    ensureTrailingPage();
    emit historyChanged();
}

// Infinite paper: whenever the last sheet has ink, a fresh one waits below.
void Document::ensureTrailingPage() {
    if (!m_pages.isEmpty() && m_pages.last()->hasInk()) {
        m_pages.append(blankPage(m_pages.last()->size));
        emit pagesChanged();
    }
}

void Document::addStroke(const PagePtr &page, const StrokePtr &stroke) {
    if (!page || !stroke || stroke->samples.isEmpty())
        return;
    auto cmd = std::make_unique<AddStroke>(page, stroke);
    cmd->redo(*this);
    push(std::move(cmd));
}

void Document::beginErase() { m_pendingErase = std::make_unique<EraseStrokes>(); }

void Document::eraseLive(const PagePtr &page, const StrokePtr &stroke) {
    if (!m_pendingErase)
        beginErase();
    const int index = page->strokes.indexOf(stroke);
    if (index < 0)
        return;
    page->strokes.removeAt(index);
    static_cast<EraseStrokes *>(m_pendingErase.get())->items.push_back({page, stroke, index});
    touch(page);
}

void Document::endErase() {
    if (m_pendingErase && !static_cast<EraseStrokes *>(m_pendingErase.get())->items.empty())
        push(std::move(m_pendingErase));
    m_pendingErase.reset();
}

bool Document::clearPage(const PagePtr &page) {
    if (!page || page->strokes.isEmpty())
        return false;
    auto cmd = std::make_unique<ClearPage>(page);
    cmd->redo(*this);
    push(std::move(cmd));
    return true;
}

PagePtr Document::insertPage(int index) {
    const QSizeF size = m_pages.isEmpty() ? QSizeF(A4Width, A4Height)
                                          : m_pages[std::clamp<int>(index - 1, 0, m_pages.size() - 1)]->size;
    auto page = blankPage(size);
    auto cmd = std::make_unique<AddPage>(page, index);
    cmd->redo(*this);
    push(std::move(cmd));
    return page;
}

void Document::undo() {
    endErase();
    if (!canUndo())
        return;
    m_history[--m_index]->undo(*this);
    emit historyChanged();
}

void Document::redo() {
    if (!canRedo())
        return;
    m_history[m_index++]->redo(*this);
    ensureTrailingPage();
    emit historyChanged();
}
