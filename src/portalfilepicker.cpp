// The portal file picker follows Omacut (MIT, David Heinemeier Hansson).
#include "portalfilepicker.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QUrl>

namespace {
struct Rule { uint type; QString pattern; };
struct Filter { QString name; QList<Rule> rules; };

QDBusArgument &operator<<(QDBusArgument &a, const Rule &r) {
    a.beginStructure(); a << r.type << r.pattern; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, Rule &r) {
    a.beginStructure(); a >> r.type >> r.pattern; a.endStructure(); return a;
}
QDBusArgument &operator<<(QDBusArgument &a, const Filter &f) {
    a.beginStructure(); a << f.name << f.rules; a.endStructure(); return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, Filter &f) {
    a.beginStructure(); a >> f.name >> f.rules; a.endStructure(); return a;
}
}
Q_DECLARE_METATYPE(Rule)
Q_DECLARE_METATYPE(Filter)

namespace {
void registerTypes() {
    static const bool done = [] {
        qDBusRegisterMetaType<Rule>();
        qDBusRegisterMetaType<QList<Rule>>();
        qDBusRegisterMetaType<Filter>();
        qDBusRegisterMetaType<QList<Filter>>();
        return true;
    }();
    Q_UNUSED(done);
}

// The portal wants folders as NUL-terminated byte strings.
QByteArray folderBytes(const QString &path) { return QFile::encodeName(path) + '\0'; }
}

PortalFilePicker::PortalFilePicker(QObject *parent) : FilePicker(parent) { registerTypes(); }

void PortalFilePicker::openPdf() {
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    request("OpenFile", tr("Open PDF"), {{"current_folder", folderBytes(docs)}}, Action::Open);
}

void PortalFilePicker::savePdf(const QString &suggestedPath) {
    QFileInfo info(suggestedPath);
    QString folder = info.absolutePath();
    if (suggestedPath.isEmpty() || !QFileInfo(folder).isDir())
        folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString name = info.fileName().isEmpty() ? tr("Untitled Note") + ".pdf" : info.fileName();
    request("SaveFile", tr("Save PDF"),
            {{"current_name", name}, {"current_folder", folderBytes(folder)}}, Action::Save);
}

void PortalFilePicker::request(const QString &method, const QString &title, QVariantMap options, Action action) {
    if (m_action != Action::None)
        return;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit failed(tr("No session bus: xdg-desktop-portal is unavailable"));
        return;
    }
    const Filter pdf{tr("PDF documents"), {{1, "application/pdf"}, {0, "*.pdf"}, {0, "*.PDF"}}};
    options.insert("filters", QVariant::fromValue(QList<Filter>{pdf}));
    options.insert("current_filter", QVariant::fromValue(pdf));
    options.insert("modal", true);

    // Subscribe to the predicted request path before calling, so a fast
    // response can't slip past us.
    const QString token = QString("omanote%1").arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService().mid(1).replace('.', '_');
    m_requestPath = QString("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    options.insert("handle_token", token);
    bus.connect("org.freedesktop.portal.Desktop", m_requestPath, "org.freedesktop.portal.Request", "Response",
                this, SLOT(handleResponse(uint, QVariantMap)));
    m_action = action;

    auto msg = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                              "org.freedesktop.portal.FileChooser", method);
    msg << QString() << title << options;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QDBusObjectPath> reply = *w;
        w->deleteLater();
        if (reply.isError()) {
            QDBusConnection::sessionBus().disconnect("org.freedesktop.portal.Desktop", m_requestPath,
                "org.freedesktop.portal.Request", "Response", this, SLOT(handleResponse(uint, QVariantMap)));
            m_action = Action::None;
            emit failed(tr("File dialog unavailable: %1").arg(reply.error().message()));
            return;
        }
        // Older portals return a different handle; follow it.
        const QString path = reply.value().path();
        if (path != m_requestPath) {
            auto bus = QDBusConnection::sessionBus();
            bus.disconnect("org.freedesktop.portal.Desktop", m_requestPath, "org.freedesktop.portal.Request",
                           "Response", this, SLOT(handleResponse(uint, QVariantMap)));
            m_requestPath = path;
            bus.connect("org.freedesktop.portal.Desktop", path, "org.freedesktop.portal.Request", "Response",
                        this, SLOT(handleResponse(uint, QVariantMap)));
        }
    });
}

void PortalFilePicker::handleResponse(uint response, const QVariantMap &results) {
    QDBusConnection::sessionBus().disconnect("org.freedesktop.portal.Desktop", m_requestPath,
        "org.freedesktop.portal.Request", "Response", this, SLOT(handleResponse(uint, QVariantMap)));
    const Action action = m_action;
    m_action = Action::None;
    const QStringList uris = results.value("uris").toStringList();
    if (response != 0 || uris.isEmpty()) {
        emit cancelled();
        return;
    }
    QString path = QUrl(uris.first()).toLocalFile();
    if (action == Action::Save) {
        if (!path.endsWith(".pdf", Qt::CaseInsensitive))
            path += ".pdf";
        emit saved(path);
    } else {
        emit opened(path);
    }
}
