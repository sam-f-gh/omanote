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
    void redo(Document &doc) override { page->strokes.append(stroke); doc.touch(page, stroke->bounds()); }
    void undo(Document &doc) override { page->strokes.removeOne(stroke); doc.touch(page, stroke->bounds()); }
    PagePtr page;
    StrokePtr stroke;
};

class EraseStrokes : public Command {
public:
    struct Item { PagePtr page; StrokePtr stroke; int index; };
    void redo(Document &doc) override {
        for (const auto &i : items) { i.page->strokes.removeOne(i.stroke); doc.touch(i.page, i.stroke->bounds()); }
    }
    void undo(Document &doc) override {
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            it->page->strokes.insert(std::min<int>(it->index, it->page->strokes.size()), it->stroke);
            doc.touch(it->page, it->stroke->bounds());
        }
    }
    std::vector<Item> items;
};

class ClearPage : public Command {
public:
    ClearPage(PagePtr p) : page(std::move(p)), strokes(page->strokes) {}
    void redo(Document &doc) override { page->strokes.clear(); doc.touch(page, area()); }
    void undo(Document &doc) override { page->strokes = strokes; doc.touch(page, area()); }
    QRectF area() const { return QRectF(QPointF(0, 0), page->size); }
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
    return fromLoaded(path, loaded, parent);
}

Document *Document::fromLoaded(const QString &path, const PdfStore::Loaded &loaded, QObject *parent) {
    auto *doc = new Document(parent);
    doc->m_source = loaded.pdf;
    doc->m_filePath = QFileInfo(path).absoluteFilePath();
    doc->m_fromOmanote = loaded.omanote;
    doc->m_external = loaded.origin != "note";
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

SaveJob Document::prepareSave() const {
    SaveJob job;
    const auto [first, last] = saveRange();
    job.source = m_source;
    job.origin = m_external ? "external" : "note";
    for (int i = first; i <= last; ++i)
        job.pages.append({m_pages[i]->size, m_pages[i]->source, m_pages[i]->note, m_pages[i]->strokes});
    for (int i = 0; i < first; ++i)
        job.leadingBlanks.append(m_pages[i]);
    job.historyIndex = m_index;
    job.generation = m_generation;
    return job;
}

// Runs on any thread: the job holds its own copies and committed strokes
// are immutable, so the GUI can keep drawing meanwhile.
bool Document::writeJob(const SaveJob &job, const QString &path, QString *error) {
    QString message;
    if (!PdfStore::save(job.source, job.pages, job.origin, path, message)) {
        if (error) *error = message;
        return false;
    }
    return true;
}

void Document::finishSave(const SaveJob &job, const QString &path) {
    m_filePath = QFileInfo(path).absoluteFilePath();
    m_fromOmanote = true;
    // Leading blanks are gone from the file, so drop them here too (unless
    // the user wrote on one meanwhile). Their history could resurrect ink on
    // pages that no longer exist, so history starts fresh from the file.
    bool trimmed = false;
    for (const auto &page : job.leadingBlanks) {
        if (!page->hasInk() && m_pages.size() > 1 && m_pages.removeOne(page))
            trimmed = true;
    }
    if (trimmed) {
        m_history.clear();
        m_index = 0;
        m_pendingErase.reset();
        ++m_generation;
        m_savedIndex = 0;
        emit pagesChanged();
    } else if (job.generation == m_generation) {
        m_savedIndex = job.historyIndex; // Ink added during the save stays dirty.
    }
    emit historyChanged();
}

bool Document::save(const QString &path, QString *error) {
    const SaveJob job = prepareSave();
    if (!writeJob(job, path, error))
        return false;
    finishSave(job, path);
    return true;
}

void Document::touch(const PagePtr &page, const QRectF &region) {
    ++page->revision;
    emit pageChanged(page->id, region);
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
    // Bounded history: old steps (and the strokes they hold) let go.
    if (int(m_history.size()) > MaxHistory) {
        m_history.erase(m_history.begin());
        --m_index;
        m_savedIndex = m_savedIndex > 0 ? m_savedIndex - 1 : -1;
    }
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
    stroke->finalize();
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
    touch(page, stroke->bounds());
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
