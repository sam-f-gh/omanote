QT += core gui qml quick quickcontrols2 dbus concurrent

CONFIG += c++17 release link_pkgconfig
PKGCONFIG += poppler-qt6 libqpdf
TARGET = omanote
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/document.h \
    src/filepicker.h \
    src/ink.h \
    src/noteview.h \
    src/pagerenderer.h \
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
    src/pagerenderer.cpp \
    src/pdfstore.cpp \
    src/portalfilepicker.cpp \
    src/theme.cpp \
    src/translator.cpp \
    src/viewport.cpp

RESOURCES += src/resources.qrc
