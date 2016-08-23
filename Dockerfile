FROM ubuntu:14.04

RUN apt-get update
RUN apt-get install -y libavformat-dev libavcodec-dev libavutil-dev
ADD . /untrunc
WORKDIR /untrunc
RUN apt-get install -y g++
RUN g++ -o untrunc file.cpp main.cpp track.cpp atom.cpp mp4.cpp -L/usr/local/lib -lavformat -lavcodec -lavutil

ENTRYPOINT bash
