QT += core gui quick testlib dbus
CONFIG += c++17 testcase link_pkgconfig
PKGCONFIG += poppler-qt6 libqpdf
TARGET = omanote_tests
TEMPLATE = app

INCLUDEPATH += ../src
DEFINES += SRCDIR=\\\"$$PWD/../src\\\"

HEADERS += ../src/document.h ../src/ink.h ../src/pdfstore.h ../src/theme.h ../src/translator.h ../src/viewport.h
SOURCES += omanote_tests.cpp ../src/document.cpp ../src/ink.cpp ../src/pdfstore.cpp ../src/theme.cpp \
    ../src/translator.cpp ../src/viewport.cpp
