// Omanote — minimalist PDF notes and annotation for Omarchy, pen first.

#include <QCommandLineParser>
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

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("omanote");
    app.setApplicationVersion("0.1.0");

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
