CXX      = g++
CXXFLAGS = -std=c++11 -I./libav

LDFLAGS  = \
	-L./libav/libavformat  -lavformat \
	-L./libav/libavcodec   -lavcodec \
	-L./libav/libavresample -lavresample \
	-L./libav/libavutil    -lavutil \
	-lpthread -lz -lbz2 \
	-framework CoreFoundation \
	-framework VideoDecodeAcceleration \
	-framework CoreVideo

TARGET  = untrunc
SRCS    = file.cpp main.cpp track.cpp atom.cpp \
          $(wildcard codec_*.cpp) \
          codecstats.cpp codec.cpp mp4.cpp log.cpp
OBJS    = $(SRCS:.cpp=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

install: $(TARGET)
	install -vpm 755 $(TARGET) /usr/local/bin/

clean:
	rm -f $(OBJS) $(TARGET)
