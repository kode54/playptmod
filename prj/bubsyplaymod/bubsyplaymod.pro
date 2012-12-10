TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += \
    ../../bubsyplaymod.c \
    ../../playptmod.c \
    ../../blip_buf.c

HEADERS += \
    ../../playptmod.h \
    ../../blip_buf.h

unix|win32: LIBS += -lao
