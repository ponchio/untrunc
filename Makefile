.cpp.o:*.h
	g++ -g -c $< -I./libav 

all:file.o main.o track.o atom.o codec_*.o codecstats.o codec.o mp4.o log.o codec_alac.o codec_apch.o codec_avc1.o codec_fdsc.o codec_gpmd.o codec_hev1.o codec_mbex.o codec_mijd.o codec_mp4a.o codec_mp4v.o codec_pcm.o codec_rtp.o codec_text.o codec_tmcd.o codec_unknown.o
	g++ -g -o untrunc -I./libav main.o file.o track.o atom.o codec_*.o codecstats.o codec.o mp4.o log.o -L./libav/libavformat -lavformat -L./libav/libavcodec -lavcodec -L./libav/libavresample -lavresample -L./libav/libavutil -lavutil -lpthread -lz -lX11 -lvdpau
