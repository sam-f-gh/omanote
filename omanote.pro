QT += core gui qml quick quickcontrols2 dbus concurrent network waylandclient

CONFIG += c++20 release link_pkgconfig wayland-scanner
PKGCONFIG += poppler-qt6 wayland-client
# qpdf's headers need C++20 and its pkg-config cflags say so with -std=c++20;
# keep that flag on the C++ side only (the generated Wayland code is C).
QMAKE_CXXFLAGS += $$system(pkg-config --cflags libqpdf)
LIBS += $$system(pkg-config --libs libqpdf)
TARGET = omanote
VERSION = 0.1.0
# The one place the version lives; main.cpp reports it for --version.
DEFINES += OMANOTE_VERSION=\\\"$$VERSION\\\"
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/document.h \
    src/filepicker.h \
    src/ink.h \
    src/noteview.h \
    src/tilerenderer.h \
    src/touchpadhold.h \
    src/pdfstore.h \
    src/portalfilepicker.h \
    src/theme.h \
    src/translator.h \
    src/viewport.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/document.cpp \
    src/ink.cpp \
    src/noteview.cpp \
    src/tilerenderer.cpp \
    src/touchpadhold.cpp \
    src/pdfstore.cpp \
    src/portalfilepicker.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/viewport.cpp

RESOURCES += src/resources.qrc

# Touchpad hold gesture: Qt's wayland-scanner feature generates the protocol
# code and C++ wrappers from the system's wayland-protocols at build time.
WAYLANDCLIENTSOURCES += $$system(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml
