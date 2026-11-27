// Omanote — minimalist PDF notes and annotation for Omarchy, pen first.

#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "backend.h"
#include "noteview.h"
#include "portalfilepicker.h"
#include "theme.h"
#include "translator.h"

namespace {
// The desktop's UI font size (the 11 of "Adwaita Sans 11") through the
// settings portal: one D-Bus read. Older portals only have Read, which wraps
// the value twice. Returns 0 when the portal doesn't answer.
double desktopFontSize() {
    for (const char *method : {"ReadOne", "Read"}) {
        QDBusMessage call = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
            "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings", method);
        call << QString("org.gnome.desktop.interface") << QString("font-name");
        const QDBusMessage reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 300);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            continue;
        QVariant value = reply.arguments().first();
        while (value.canConvert<QDBusVariant>() && value.userType() == qMetaTypeId<QDBusVariant>())
            value = value.value<QDBusVariant>().variant();
        const QString spec = value.toString().trimmed();
        const int split = spec.lastIndexOf(' ');
        bool ok = false;
        const double size = split > 0 ? spec.mid(split + 1).toDouble(&ok) : 0;
        return ok && size > 0 ? size : 0;
    }
    return 0;
}

// Omarchy's font is fontconfig's "monospace" alias, which `omarchy font set`
// points at the chosen family; the shell, the terminals and Omarchy's other
// apps all draw with it. Asking for the alias rather than a resolved name
// keeps Omanote on whatever the user picks.
void applyOmarchyFont(QGuiApplication &app, double pointSize) {
    QFont font = app.font();
    font.setFamilies({QStringLiteral("monospace")});
    font.setStyleHint(QFont::Monospace);
    if (pointSize > 0)
        font.setPointSizeF(pointSize);
    app.setFont(font);
}
}

int main(int argc, char *argv[]) {
    // Omarchy points Qt at the GTK platform theme, which loads all of GTK
    // (~28 ms of startup plus its memory) only to learn the UI font. The
    // portal theme gives the same light/dark hints without GTK, and the font
    // size is one D-Bus read below.
    const bool gtkTheme = qgetenv("QT_QPA_PLATFORMTHEME") == "gtk3";
    if (gtkTheme)
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    QGuiApplication app(argc, argv);
    applyOmarchyFont(app, gtkTheme ? desktopFontSize() : 0);
    app.setApplicationName("omanote");
    app.setApplicationVersion(QStringLiteral(OMANOTE_VERSION));

    // Matches the window to omanote.desktop, so the compositor (Wayland
    // app_id) and the launcher pick up the installed icon.
    app.setDesktopFileName("omanote");
    app.setWindowIcon(QIcon::fromTheme("omanote"));

    QCommandLineParser parser;
    parser.setApplicationDescription("Minimalist PDF notes and annotation");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "PDF to open", "[file.pdf]");
    parser.process(app);

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    qmlRegisterType<NoteView>("Omanote", 1, 0, "NoteView");
    qmlRegisterUncreatableType<Document>("Omanote", 1, 0, "Document", "Owned by the backend");

    Theme theme;
    PortalFilePicker picker;
    Backend backend(&picker);
    if (!parser.positionalArguments().isEmpty())
        backend.openFile(parser.positionalArguments().first());

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
