#-------------------------------------------------
#
# Project created by QtCreator 2012-10-28T12:50:54
#
#-------------------------------------------------

QT       -= core
QT       -= gui

TARGET = untrunc
CONFIG   += console
CONFIG   -= -qt app_bundle


TEMPLATE = app


SOURCES += main.cpp \
    atom.cpp \
    mp4.cpp \
    file.cpp \
    track.cpp

HEADERS += \
    atom.h \
    mp4.h \
    file.h \
    track.h

CONFIG(system_libav) {
    message(Building with system libav.)
    #PKG_CONFIG = PKG_CONFIG_PATH=/usr/lib64/ffmpeg-compat/pkgconfig pkg-config
    CONFIG += link_pkgconfig
    PKGCONFIG += libavformat libavcodec libavutil

    # without pkg-config
    #INCLUDEPATH += -I/usr/local/lib
    #LIBS += -L/usr/local/lib -lavformat -lavcodec -lavutil
} else {
    message(Building with static libav.)
    LIBAV_SOURCE_PATH = ../libav-0.8.7
    LIBAV_BUILD_PATH = $$LIBAV_SOURCE_PATH
    LIBAV_HEADER_PATH = $$LIBAV_SOURCE_PATH
    INCLUDEPATH += -I$$LIBAV_HEADER_PATH/libavformat -I$$LIBAV_HEADER_PATH/libavcodec -I$$LIBAV_HEADER_PATH/libavutil
    LIBS += $$LIBAV_BUILD_PATH/libavformat/libavformat.a $$LIBAV_BUILD_PATH/libavcodec/libavcodec.a $$LIBAV_BUILD_PATH/libavutil/libavutil.a
}

DEFINES += _FILE_OFFSET_BITS=64 VERBOSE VERBOSE1
LIBS += -lz

#QMAKE_LFLAGS += -static
#LIBS += /usr/lib/x86_64-linux-gnu/libavcodec.a \
#        /usr/lib/x86_64-linux-gnu/libavformat.a \
#        /usr/lib/x86_64-linux-gnu/libavutil.a
