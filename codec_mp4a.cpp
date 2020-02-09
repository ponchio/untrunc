#include "codec.h"
#include "log.h"

using namespace std;

Match Codec::mp4aMatch(const unsigned char *start, int maxlength) {

	Match match;

	//leftover!
	/*if(s > 1000000) {
		Log::debug << "mp4a: Success because of large s value.\n";
		return true;
	}*/
	// XXX Horrible Hack: These values might need to be changed depending on the file. XXX
	if((start[4] == 0xee && start[5] == 0x1b) ||
			(start[4] == 0x3e && start[5] == 0x64) )
	{
		Log::debug << "mp4a: Success because of horrible hack.\n";
		return true;
	}

	if(start[0] == 0) {
		Log::debug << "mp4a: Failure because of NULL header.\n";
		return false;
	}
	Log::debug << "mp4a: Success for no particular reason....\n";
	return true;



	if(!context)
		return -1;

	int consumed = -1;
	{
		AvLog useAvLog();
		AVFrame *frame = av_frame_alloc();
		if(!frame)
			throw string("Could not create AVFrame");
		AVPacket avp;
		av_init_packet(&avp);
		avp.data = start;
		avp.size = maxlength;
		int got_frame = 0;
		consumed = avcodec_decode_audio4(context, frame, &got_frame, &avp);
		if(consumed >= 0) {
			if(frame->nb_samples > 0)
				duration = frame->nb_samples;
			// Flush decoder to receive buffered packets.
			if(consumed <= 0 || duration <= 0) {
				Log::debug << "Flush " << name << " decoder.\n";
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
	Log::debug << "Duration: " << duration << '\n';
	match.length = consumed;
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


