#include "backend.h"

#include <QColor>
#include <QFileInfo>
#include <QUrl>
#include <algorithm>

const QList<double> &Backend::penWidths() {
    static const QList<double> w{1.5, 2.5, 5.0};
    return w;
}

const QList<double> &Backend::highlightWidths() {
    static const QList<double> w{14, 22, 32};
    return w;
}

Backend::Backend(FilePicker *picker, QObject *parent)
    : QObject(parent), m_picker(picker), m_settings("omanote", "omanote") {
    // Colors and widths persist; the tool always starts as the pen.
    m_penColor = m_settings.value("penColor", m_penColor).toString();
    m_penSize = std::clamp(m_settings.value("penSize", 1).toInt(), 0, 2);
    m_highlightColor = m_settings.value("highlightColor", m_highlightColor).toString();
    m_highlightSize = std::clamp(m_settings.value("highlightSize", 1).toInt(), 0, 2);

    connect(m_picker, &FilePicker::opened, this, [this](const QString &path) {
        setBusy(false);
        openFile(path);
    });
    connect(m_picker, &FilePicker::saved, this, [this](const QString &path) {
        setBusy(false);
        m_awaitingSave = false;
        saveTo(path);
    });
    connect(m_picker, &FilePicker::cancelled, this, [this] {
        setBusy(false);
        if (m_awaitingSave) {
            m_awaitingSave = false;
            emit saveCancelled();
        }
    });
    connect(m_picker, &FilePicker::failed, this, [this](const QString &message) {
        setBusy(false);
        if (m_awaitingSave) {
            m_awaitingSave = false;
            emit saveCancelled();
        }
        if (m_doc)
            emit status(message);
        else
            setError(message);
    });
}

QString Backend::title() const {
    if (!m_doc)
        return {};
    return m_doc->filePath().isEmpty() ? tr("Untitled Note") : QFileInfo(m_doc->filePath()).fileName();
}

void Backend::setDocument(Document *doc) {
    if (m_doc)
        m_doc->deleteLater();
    m_doc = doc;
    if (doc)
        doc->setParent(this);
    emit documentChanged();
    emit titleChanged();
}

void Backend::setError(const QString &message) {
    if (message == m_error)
        return;
    m_error = message;
    emit errorChanged();
}

void Backend::clearError() { setError({}); }

void Backend::setBusy(bool busy) {
    if (busy != m_busy) {
        m_busy = busy;
        emit busyChanged();
    }
}

void Backend::newNote() {
    clearError();
    setDocument(Document::createNote());
}

void Backend::openDialog() {
    setBusy(true);
    m_picker->openPdf();
}

bool Backend::openFile(const QString &pathOrUrl) {
    const QUrl url(pathOrUrl);
    const QString path = url.isLocalFile() ? url.toLocalFile() : pathOrUrl;
    QString message;
    Document *doc = Document::open(path, &message);
    if (!doc) {
        // Errors surface in the window: inline on the start screen, as a
        // status line over an open document.
        if (m_doc)
            emit status(message);
        else
            setError(message);
        return false;
    }
    clearError();
    setDocument(doc);
    if (doc->isExternal() && !doc->fromOmanote())
        emit status(tr("Annotating a copy: the first save asks where"));
    return true;
}

void Backend::save() {
    if (!m_doc)
        return;
    if (m_doc->needsSaveAs()) {
        saveAs();
        return;
    }
    saveTo(m_doc->filePath());
}

void Backend::saveAs() {
    if (!m_doc)
        return;
    m_awaitingSave = true;
    setBusy(true);
    QString suggestion = m_doc->filePath();
    if (!suggestion.isEmpty() && m_doc->isExternal() && !m_doc->fromOmanote()) {
        // Never suggest overwriting someone else's original.
        const QFileInfo info(suggestion);
        suggestion = info.absolutePath() + "/" + info.completeBaseName() + tr(" (notes)") + ".pdf";
    }
    m_picker->savePdf(suggestion);
}

bool Backend::saveTo(const QString &path) {
    if (!m_doc)
        return false;
    QString message;
    if (!m_doc->save(path, &message)) {
        emit status(message);
        emit saveCancelled();
        return false;
    }
    emit titleChanged();
    emit status(tr("Saved %1").arg(QFileInfo(path).fileName()));
    emit saved();
    return true;
}

void Backend::closeDocument() { setDocument(nullptr); }

void Backend::store() {
    m_settings.setValue("penColor", m_penColor);
    m_settings.setValue("penSize", m_penSize);
    m_settings.setValue("highlightColor", m_highlightColor);
    m_settings.setValue("highlightSize", m_highlightSize);
}

void Backend::setTool(int tool) {
    tool = std::clamp(tool, 0, 2);
    if (tool == m_tool) return;
    m_tool = tool;
    store();
    emit toolChanged();
}

void Backend::setPenColor(const QString &color) {
    if (color == m_penColor || !QColor::isValidColorName(color)) return;
    m_penColor = color;
    store();
    emit toolChanged();
}

void Backend::setPenSize(int size) {
    size = std::clamp(size, 0, 2);
    if (size == m_penSize) return;
    m_penSize = size;
    store();
    emit toolChanged();
}

void Backend::setHighlightColor(const QString &color) {
    if (color == m_highlightColor || !QColor::isValidColorName(color)) return;
    m_highlightColor = color;
    store();
    emit toolChanged();
}

void Backend::setHighlightSize(int size) {
    size = std::clamp(size, 0, 2);
    if (size == m_highlightSize) return;
    m_highlightSize = size;
    store();
    emit toolChanged();
}

double Backend::penWidth() const { return penWidths()[m_penSize]; }
double Backend::highlightWidth() const { return highlightWidths()[m_highlightSize]; }
