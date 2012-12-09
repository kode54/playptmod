TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += \
    ../../bubsyplaymod.c \
    ../../playptmod.c

HEADERS += \
    ../../playptmod.h

unix|win32: LIBS += -lao
