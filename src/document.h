#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSizeF>
#include <memory>
#include <vector>

#include "ink.h"
#include "pdfstore.h"

// One sheet. Pages are shared pointers with stable ids, so undo history and
// caches survive pages being inserted or trimmed around them.
struct Page {
    quint64 id = 0;
    QSizeF size{595.28, 842};
    int source = -1;   // Page index in Document::sourcePdf, or -1 for fresh paper.
    bool note = true;  // Blank paper Omanote made (trimmable), not an original page.
    QList<StrokePtr> strokes;
    int revision = 0;  // Bumped on every ink change, for render caches.

    bool hasInk() const { return !strokes.isEmpty(); }
    bool isContent() const { return hasInk() || !note; }
};
using PagePtr = std::shared_ptr<Page>;

class Command;

// Everything a save needs, captured on the GUI thread so the PDF can be
// built on a worker while the user keeps writing.
struct SaveJob {
    QByteArray source;
    QList<PdfStore::PageData> pages;       // Copied on the GUI thread.
    QString origin;
    QList<PagePtr> leadingBlanks;          // Dropped from memory once saved.
    int historyIndex = 0;
    int generation = 0;
};
class Document : public QObject {
    Q_OBJECT
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pagesChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY historyChanged)

public:
    static constexpr double A4Width = 595.28, A4Height = 842;
    static constexpr int MaxHistory = 500;

    explicit Document(QObject *parent = nullptr);
    ~Document() override;

    // A fresh notebook: one blank A4 sheet.
    static Document *createNote(QObject *parent = nullptr);
    // nullptr and an error message when the file can't be read.
    static Document *open(const QString &path, QString *error, QObject *parent = nullptr);
    static Document *fromLoaded(const QString &path, const PdfStore::Loaded &loaded, QObject *parent = nullptr);

    // Writes first..last content page atomically, then drops leading blank
    // pages from memory so the view matches the file. Synchronous; the app
    // uses prepareSave() + PdfStore on a worker + finishSave().
    bool save(const QString &path, QString *error);
    SaveJob prepareSave() const;
    static bool writeJob(const SaveJob &job, const QString &path, QString *error);
    void finishSave(const SaveJob &job, const QString &path);

    const QList<PagePtr> &pages() const { return m_pages; }
    int pageCount() const { return m_pages.size(); }
    PagePtr page(int index) const { return index >= 0 && index < m_pages.size() ? m_pages[index] : nullptr; }
    int indexOf(const PagePtr &page) const { return m_pages.indexOf(page); }
    // The pages a save would write, by index: [first, last].
    std::pair<int, int> saveRange() const;

    QByteArray sourcePdf() const { return m_source; }
    QString filePath() const { return m_filePath; }
    bool fromOmanote() const { return m_fromOmanote; }
    bool isExternal() const { return m_external; }
    // Only a note that has never been saved asks where; any file that exists
    // on disk is saved in place.
    bool needsSaveAs() const { return m_filePath.isEmpty(); }

    bool canUndo() const { return m_index > 0; }
    bool canRedo() const { return m_index < int(m_history.size()); }
    bool dirty() const { return m_index != m_savedIndex; }

    void addStroke(const PagePtr &page, const StrokePtr &stroke);
    // Erasing one gesture is one undo step; strokes vanish as they're touched.
    void beginErase();
    void eraseLive(const PagePtr &page, const StrokePtr &stroke);
    void endErase();
    bool clearPage(const PagePtr &page);
    PagePtr insertPage(int index);
    // Fresh paper at the end, outside undo history (like the trailing sheet):
    // scrolling past the end of a notebook keeps finding paper.
    void appendPaper();
    // Drops blank sheets of paper after `index`, from the end backwards,
    // stopping at the first page with ink (or an original PDF page).
    // Outside undo history; returns whether any went.
    bool trimPaperAfter(int index);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    static quint64 newId();

    // Used by commands.
    void insertPageRaw(int index, const PagePtr &page);
    void removePageRaw(const PagePtr &page);
    void touch(const PagePtr &page, const QRectF &region);

signals:
    void pagesChanged();
    // Ink changed on a page, within region (page points).
    void pageChanged(quint64 pageId, const QRectF &region);
    void historyChanged();

private:
    void push(std::unique_ptr<Command> command);
    void ensureTrailingPage();

    QList<PagePtr> m_pages;
    QByteArray m_source;
    QString m_filePath;
    bool m_fromOmanote = false;
    bool m_external = false;
    std::vector<std::unique_ptr<Command>> m_history;
    int m_index = 0;
    int m_savedIndex = 0;
    std::unique_ptr<Command> m_pendingErase;
    int m_generation = 0; // Bumped when history is rebased (trim).
};
