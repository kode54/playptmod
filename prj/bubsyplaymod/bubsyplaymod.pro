TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

SOURCES += \
    ../../bubsyplaymod.c \
    ../../playptmod.c \
    ../../pt_blep.c

HEADERS += \
    ../../playptmod.h \
    ../../pt_blep.h

unix|win32: LIBS += -lao
