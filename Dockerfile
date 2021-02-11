# pull base image
FROM ubuntu:bionic as build

# install packaged dependencies
RUN apt-get update && \
    apt-get -y install \
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
RUN ./configure && make -j

# build untrunc
WORKDIR /untrunc
ADD . .
RUN /usr/bin/g++ -o untrunc \
    -I./libav-12.3 file.cpp main.cpp track.cpp atom.cpp mp4.cpp \
    -L./libav-12.3/libavformat -lavformat \
    -L./libav-12.3/libavcodec -lavcodec \
    -L./libav-12.3/libavresample -lavresample \
    -L./libav-12.3/libavutil -lavutil \
    -lpthread -lz

# Have a small deliverable Docker image
FROM ubuntu:bionic
COPY --from=build /untrunc/untrunc /untrunc

# execution
ENTRYPOINT ["/untrunc"]
