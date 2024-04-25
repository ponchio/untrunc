# pull base image
FROM ubuntu:bionic as build

# install packaged dependencies
RUN apt-get update && apt-get upgrade -y && \
    apt-get -y install \
    fonts-dejavu-core \
    libavformat-dev \
    libavcodec-dev \
    libavutil-dev \
    unzip \
    g++ \
    wget \
    make \
    nasm \
    zlib1g-dev

# download and extract
WORKDIR /untrunc
RUN wget https://github.com/libav/libav/archive/v12.3.zip && \
    unzip v12.3.zip && \
    rm v12.3.zip

# build libav
WORKDIR /untrunc/libav-12.3/
RUN ./configure && make -j8

# build untrunc
WORKDIR /untrunc
ADD . .
RUN /usr/bin/g++ -o untrunc \
    main.cpp \
    atom.cpp \
    mp4.cpp \
    file.cpp \
    track.cpp \
    log.cpp \
    codec.cpp \
    codec_rtp.cpp \
    codec_avc1.cpp \
    codec_hev1.cpp \
    codec_mp4a.cpp \
    codec_mp4v.cpp \
    codec_pcm.cpp \
    codec_mbex.cpp \
    codec_alac.cpp \
    codecstats.cpp \
    codec_unknown.cpp \
    codec_text.cpp \
    codec_tmcd.cpp \
    codec_gpmd.cpp \
    codec_camm.cpp \
    codec_fdsc.cpp \
    codec_apch.cpp \
    codec_mijd.cpp \
    -I./libav-12.3 \
    -L./libav-12.3/libavformat -lavformat \
    -L./libav-12.3/libavcodec -lavcodec \
    -L./libav-12.3/libavresample -lavresample \
    -L./libav-12.3/libavutil -lavutil \
    -lpthread -lz

# Have a small deliverable Docker image
FROM ubuntu:bionic
COPY --from=build /untrunc/untrunc /untrunc

# user
RUN useradd untrunc
USER untrunc

# execution
ENTRYPOINT ["/untrunc"]
