#include "codec.h"
#include "log.h"
#include "avlog.h"

extern "C" {
	#include <libavcodec/mpegaudiodecheader.h>
}
using namespace std;

//orrid.
Match Codec::mp4aSearch(const unsigned char *start, int maxlength, int maxskip) {
	for(int i = 0; i < maxskip; i++) {
		Match m = mp4aMatch(start + i, maxlength - i);
		if(m.chances > 0) {
			m.offset = i;
			return m;
		}
	}
	return Match();
}

Match Codec::mp4aMatch(const unsigned char *start, int maxlength) {


	Match match;

	if(!context)
		return match;

	if(strcmp(context->codec->name, "mp3") == 0) {
		MPADecodeHeader *s = (MPADecodeHeader *)context->priv_data;
		uint32_t header = AV_RB32(start);

		int ret = avpriv_mpegaudio_decode_header(s, header);
		if (ret < 0) { //wrong header
			return match;
		} else if(ret == 1) { //no way to compute size (that I know of)
			return match;
		}
		match.length = s->frame_size;
		match.chances = 100;
		return match;
	}


	// XXX Horrible Hack: These values might need to be changed depending on the file. XXX
	if((start[4] == 0xee && start[5] == 0x1b) ||
			(start[4] == 0x3e && start[5] == 0x64) ) {
		match.chances = 32000;
		Log::debug << "mp4a: Success because of horrible hack.\n";
	}

	int32_t begin32 = readBE<int32_t>(start);
	if(start[0] == 0 && begin32 < 1000000)
		return match;

	uint32_t duration = 0;

	int consumed = -1;
	{
		AvLog useAvLog();
		av_log_set_level(0);
		AVFrame *frame = av_frame_alloc();
		if(!frame)
			throw string("Could not create AVFrame");
		AVPacket avp;
		av_init_packet(&avp);
		avp.data = (uint8_t *)start;
		avp.size = maxlength;
		int got_frame = 0;
		consumed = avcodec_decode_audio4(context, frame, &got_frame, &avp);

		if(consumed < 0)
			return match;

		if(consumed <= 4) {
			avp.data += consumed;
			consumed = avcodec_decode_audio4(context, frame, &got_frame, &avp);
		}

		int frame_size =  *(int *)context->priv_data;

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
		av_frame_free(&frame);
	}

	if(consumed == maxlength) {
		Log::debug << "Codec can't determine length of the packet.";
		match.chances = 4;
		consumed = 0; //unknown length
	} else if(consumed < 0) {
		match.chances = 0.0f;
		consumed = 0;
	} else if(consumed == 6) {
		match.chances = 32000;
	} else if (consumed < 6) {
		return match;
	}

//actually below 200 is pretty common! l
//TODO use actual length of the good packets to assess a probability
	else if(consumed < 400)
		match.chances = 50;
	else
		match.chances = 100;

	match.duration = duration;
	match.length = consumed;

	/*
	if(match.chances > 0) {
		//count zeros.
		int zeros = 0;
		for(int i = 0; i < match.length; i++) {
			if(start[i] == 0)
				zeros++;
		}
		if(zeros*2 > match.length)
			cout << "ZEROS! " << zeros << " / " << match.length << endl;
		else
			cout << "UNOS!" << endl;
	}*/
	return match;
}


#if 0 // THIS is true for mp3...
	// From: MP3'Tech Programmer's corner <http://www.mp3-tech.org/>.
	// MPEG Audio Layer I/II/III frame header (MSB->LSB):
	// BitPos   Length Use
	//  31-21  11 bits Frame sync [all bits are 1],
	//  20-19   2 bits MPEG Audio version Id [11=v1, 10=v2, 01=reserved, 00=v2.5+],
	//  18-17   2 bits Layer [11=I, 10=II, 01=III, 00=reserved],
	//     16   1 bit  Protection [1=unprotected, 0=16-bit CRC after this header],
	//  15-12   4 bits Bitrate index (variable for VBR) [0000=free, .., 1111=bad],
	//  11-10   2 bits Sampling rate frequency index [.., 11=reserved],
	//      9   1 bit  Padding (might be variable) [0=unpadded, 1=extra 32bit (L1) or 8bit (L2/L3)],
	//      8   1 bit  Private (???) [=0 ???],
	//   7- 6   2 bits Channel mode (constant) [00=Stereo, 01=Joint Stereo, 10=Dual Mono, 11=Single Mono],
	//   5- 4   2 bits Mode extension for Joint Stereo (variable),
	//      3   1 bit  Copyright [0=Not-Copyrighted, 1=Copyrighted],
	//      2   1 bit  Original [0=Copy, 1=Original Media],
	//   1- 0   2 bits Emphasis (variable) [00=None, 01=50/15 ms, 10=reserved, 11=CCIT J.17].
	// In practice we have:
	//  mask = 11111111111 11 11 1 0000 11 0 0 11 11 1 1 11
	//       = 1111 1111 1111 1111 0000 1100 1111 1111
	//       = 0xFFFF0CFF
	reverse(s);
	if(s & 0xFFE00000 != 0xFFE00000) // Check frame sync.
		return false;
	return true;
#endif


