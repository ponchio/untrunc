#include "codec.h"
#include "log.h"
#include "avlog.h"

using namespace std;

Match Codec::mp4vSearch(const unsigned char *start, int maxlength, int maxskip) {
	Match match;
	for(int offset = 0; offset < maxskip; offset++) {
		int32_t begin32 = readBE<int32_t>(start + offset);
		if(begin32 == 0x1b3 || begin32 == 0x1b6) {
			match.offset = offset;
			match.chances = 1<<20;
			break;
		}
	}
	return match;
}

Match Codec::mp4vMatch(const unsigned char *start, int maxlength) {

	Match match;
	if(!context)
		return match;

	int32_t begin32 = readBE<int32_t>(start);
	if(begin32 != 0x1b3 && begin32 != 0x1b6)
		return match;
	match.chances = 1<<20;


	uint32_t duration = 0;

	int consumed = -1;

	{
		AvLog useAvLog();
		av_log_set_level(0);

		static AVPacket* packet = av_packet_alloc();
		static AVFrame* frame = av_frame_alloc();

		packet->data = const_cast<unsigned char*>(start);
		packet->size = maxlength;
		int got_frame = 0;

		consumed = avcodec_decode_video2(context, frame, &got_frame, packet);

//		bool keyframe = frame->key_frame;
//		not a frame? = !got_frame;

/*

		AVFrame *frame = av_frame_alloc();
		if(!frame)
			throw string("Could not create AVFrame");
		AVPacket avp;
		av_init_packet(&avp);
		avp.data = (uint8_t *)start;
		avp.size = maxlength;
		int got_frame = 0;
		consumed = avcodec_decode_audio4(context, frame, &got_frame, &avp);


		if(consumed >= 0) {
			if(frame->nb_samples > 0)
				duration = frame->nb_samples;
			// Flush decoder to receive buffered packets.
			if(consumed <= 0 || duration <= 0) {
				got_frame = 0;
				av_packet_unref(&avp);
				av_frame_unref(frame);
				int consumed2 = avcodec_decode_audio4(context, frame, &got_frame, &avp);
				if(consumed2 >= 0) {
					if(consumed <= 0)
						consumed = consumed2;
					if(duration <= 0 && frame->nb_samples > 0)
						duration = frame->nb_samples;
				}
			}
		}
		av_packet_unref(&avp);
		av_frame_free(&frame); */
	}

	if(consumed == maxlength) {
		Log::debug << "Codec can't determine length of the packet.";
		match.chances = 4;
		consumed = 0; //unknown length
	} else if(consumed < 0) {
		match.chances = 0.0f;
		consumed = 0;
	}

	match.duration = duration;
	match.length = consumed;
	return match;
}
