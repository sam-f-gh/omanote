#pragma once

#include <QObject>
#include <QPointer>
#include <QSettings>

#include "document.h"
#include "filepicker.h"

// The session: which document is open, the chosen tools, and the open/save
// flows (Save As on first save of new or foreign files, overwrite after).
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(Document *document READ document NOTIFY documentChanged)
    Q_PROPERTY(bool hasDocument READ hasDocument NOTIFY documentChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged)
    Q_PROPERTY(QString penColor READ penColor WRITE setPenColor NOTIFY toolChanged)
    Q_PROPERTY(int penSize READ penSize WRITE setPenSize NOTIFY toolChanged)
    Q_PROPERTY(QString highlightColor READ highlightColor WRITE setHighlightColor NOTIFY toolChanged)
    Q_PROPERTY(int highlightSize READ highlightSize WRITE setHighlightSize NOTIFY toolChanged)
    Q_PROPERTY(double penWidth READ penWidth NOTIFY toolChanged)
    Q_PROPERTY(double highlightWidth READ highlightWidth NOTIFY toolChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit Backend(FilePicker *picker, QObject *parent = nullptr);

    Document *document() const { return m_doc; }
    bool hasDocument() const { return m_doc; }
    QString title() const;
    QString error() const { return m_error; }
    bool busy() const { return m_busy; }

    int tool() const { return m_tool; }
    void setTool(int tool);
    QString penColor() const { return m_penColor; }
    void setPenColor(const QString &color);
    int penSize() const { return m_penSize; }
    void setPenSize(int size);
    QString highlightColor() const { return m_highlightColor; }
    void setHighlightColor(const QString &color);
    int highlightSize() const { return m_highlightSize; }
    void setHighlightSize(int size);
    double penWidth() const;
    double highlightWidth() const;

    Q_INVOKABLE void newNote();
    Q_INVOKABLE void openDialog();
    void openFile(const QString &pathOrUrl);
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAs();
    bool saveTo(const QString &path);
    Q_INVOKABLE void closeDocument();

    static const QList<double> &penWidths();
    static const QList<double> &highlightWidths();

signals:
    void documentChanged();
    void titleChanged();
    void errorChanged();
    void toolChanged();
    void busyChanged();
    void status(const QString &message);
    void saved();
    void saveCancelled();

private:
    void setDocument(Document *doc);
    void setError(const QString &message);
    void reportError(const QString &message);
    void cancelAwaitingSave();
    void setBusy(bool busy);
    void store();

    FilePicker *m_picker;
    QPointer<Document> m_doc;
    QString m_error;
    bool m_busy = false;
    bool m_awaitingSave = false;
    bool m_saving = false;
    int m_openTicket = 0;
    QSettings m_settings;
    int m_tool = 0;
    QString m_penColor = "#18181c";
    int m_penSize = 1;
    QString m_highlightColor = "#ffd60a";
    int m_highlightSize = 1;
};
