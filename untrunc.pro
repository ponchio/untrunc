#-------------------------------------------------
#
# Project created by QtCreator 2012-10-28T12:50:54
#
#-------------------------------------------------

QT -= core
QT -= gui

TARGET = untrunc
CONFIG += console
CONFIG -= -qt app_bundle

QMAKE_CXXFLAGS += -std=c++17

TEMPLATE = app

SOURCES += main.cpp \
    atom.cpp \
    mp4.cpp \
    file.cpp \
    track.cpp \
    log.cpp \
    codec.cpp \
    codec_rtp.cpp \
    codec_avc1.cpp \
    codec_mp4a.cpp \
    codec_pcm.cpp \
    codec_mbex.cpp \
    codec_alac.cpp \
    codecstats.cpp \
    codec_unknown.cpp \
    codec_text.cpp \
    codec_tmcd.cpp \
    codec_fdsc.cpp \
    codec_apch.cpp \
    codec_mijd.cpp \
    codec_hev1.cpp \
    codec_mp4v.cpp \
    codec_gpmd.cpp \
    codec_camm.cpp 

HEADERS += \
    atom.h \
    mp4.h \
    file.h \
    track.h \
    AP_AtomDefinitions.h \
    log.h \
    codec.h \
    avlog.h \
    codecstats.h

INCLUDEPATH += ./libav ./libav/libavcodec
LIBS += ./libav/libavformat/libavformat.a \
./libav/libavcodec/libavcodec.a \
./libav/libavutil/libavutil.a \
./libav/libavresample/libavresample.a -lbz2


#INCLUDEPATH += -I/usr/local/lib
#LIBS += -L/usr/local/lib -lavformat -lavcodec -lavutil
DEFINES += _FILE_OFFSET_BITS=64 VERBOSE VERBOSE1

LIBS += -lz

#libbz2-dev e libz-dev for ubuntu.

