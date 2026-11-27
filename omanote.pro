QT += core gui qml quick quickcontrols2 dbus concurrent network

CONFIG += c++17 release link_pkgconfig
PKGCONFIG += poppler-qt6 libqpdf wayland-client
TARGET = omanote
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

# Wayland protocol code for the touchpad hold gesture, generated at build
# time from the system's wayland-protocols.
WAYLAND_PROTOCOLS = $$system(pkg-config --variable=pkgdatadir wayland-protocols)
WAYLAND_GESTURES = $$WAYLAND_PROTOCOLS/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml
wlheader.input = WAYLAND_GESTURES
wlheader.output = ${QMAKE_FILE_BASE}-client-protocol.h
wlheader.commands = wayland-scanner client-header ${QMAKE_FILE_IN} ${QMAKE_FILE_OUT}
wlheader.variable_out = HEADERS
wlheader.CONFIG += target_predeps no_link
wlcode.input = WAYLAND_GESTURES
wlcode.output = ${QMAKE_FILE_BASE}-protocol.c
wlcode.commands = wayland-scanner private-code ${QMAKE_FILE_IN} ${QMAKE_FILE_OUT}
wlcode.variable_out = SOURCES
QMAKE_EXTRA_COMPILERS += wlheader wlcode
INCLUDEPATH += $$OUT_PWD
