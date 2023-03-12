#include "codec.h"
#include "atom.h"
#include "log.h"

#include <cstring>
#include <cassert>

#include <bitset>





using namespace std;


// Codec.
Codec::Codec() : context(NULL), codec(NULL), mask1(0), mask0(0) { }

void Codec::clear() {
	name.clear();
	// Do not remove the context, as it will be re-used!
	codec   = NULL;
	mask1   = 0;
	mask0   = 0;
}

bool Codec::parse(Atom *trak) {
	Atom *stsd = trak->atomByName("stsd");
	if(!stsd) {
		Log::error << "Missing 'Sample Descriptions' atom (stsd).\n";
		return false;
	}
	int32_t entries = stsd->readInt(4);
	if(entries != 1)
		throw string("Multiplexed streams not supported");

	char codec_name[5];
	stsd->readChar(codec_name, 12, 4);
	name = codec_name;

	if(name == "raw " || //unsigned, linear PCM. 8-bit data
		name == "twos" || //signed (i.e. twos-complement) linear PCM. 16-bit data is stored in big endian format.
		name == "sowt" || //signed linear PCM. However, 16-bit data is stored in little endian format.
		name == "twos" ||
		name == "in24" || //24-bit, big endian, linear PCM.
		name == "in32" || //32-bit, big endian, linear PCM.
		name == "fl32" || //32-bit floating point PCM. (Presumably IEEE 32-bit; byte order?)
		name == "fl64" || //64-bit floating point PCM. (Presumably IEEE 64-bit; byte order?)
		name == "alaw" || //A-law logarithmic PCM.
		name == "lpcm" ||
		name == "ulaw") {
			pcm = true;
	}

	if(pcm) {
		//first entry 8 is length 12 is codec
		//then 16 data format and 22 for refence index
		int16_t version = stsd->readInt16(24);
		int16_t pcm_channels = stsd->readInt16(32);
		int16_t pcm_channel_bits = stsd->readInt16(34);
		if(version == 0) {
			pcm_bytes_per_sample = pcm_channels * pcm_channel_bits/8;
		} else if(version == 1) {
			//int32_t bytes_per_packet = stsd->readInt(44);
			//int32_t bytes_per_frame = stsd->readInt(48);
			pcm_bytes_per_sample = stsd->readInt(52);
		} else if(version == 2) {
			//TODO verify this number!
			pcm_bytes_per_sample = stsd->readInt(24 + 13*4);
			assert(pcm_bytes_per_sample < 128);
		}

	}
	return true;
}

//Match rtpMatch(const unsigned char *start, int maxlength);


Match Codec::match(const unsigned char *start, int maxlength) {

	if(name == "rtp ") {
		return rtpMatch(start, maxlength);
	} else if(name == "avc1") {
		return avc1Match(start, maxlength);

	} else if(name == "mp4a") {
		return mp4aMatch(start, maxlength);
	} else if(name == "mp4v") {
		return mp4vMatch(start, maxlength);

	} else if(name == "hev1" || name == "hvc1") {
		return hev1Match(start, maxlength);

	} else if(name == "alac") {
		return alacMatch(start, maxlength);
	} else if(name == "mebx") {
		return mbexMatch(start, maxlength);
	} else if(name == "text") {
		return textMatch(start, maxlength);
	} else if(name == "apch") {
		return apchMatch(start, maxlength);
	} else if(name == "tmcd") {
		return tmcdMatch(start, maxlength);		
	} else if(name == "gpmd") {
		return gpmdMatch(start, maxlength);
	} else if(name == "camm") {
		return cammMatch(start, maxlength);


	} else if(name == "fdsc") {
		return fdscMatch(start, maxlength);
	} else if(name == "priv") {
		return mijdMatch(start, maxlength);
	} else if(pcm) {
		return pcmMatch(start, maxlength);
	} else { //rtmd
		return unknownMatch(start, maxlength);
	}


	Log::error << "Unsupported codec: " << name << "\n";
	return Match();
	//throw "Usnupported codec\n";
}

Match Codec::search(const unsigned char *start, int maxlength, int maxskip) {
	if(name == "apch") {
		return apchSearch(start, maxlength, maxskip);
	} else if(name == "avc1") {
		return avc1Search(start, maxlength, maxskip);
	} else if(name == "mp4a") {
		return mp4aSearch(start, maxlength, maxskip);
	} else if(name == "mp4v") {
		return mp4vSearch(start, maxlength, maxskip);
	} else if(name == "gpmd") {
		return gpmdSearch(start, maxlength, maxskip);
	} else if(name == "camm") {
		return gpmdSearch(start, maxlength, maxskip);
	} else if(name == "fdsc") {
		return fdscSearch(start, maxlength, maxskip);
	}

	Match match;
	return match;

	int count = 0;
	for(int offset = 8; offset < maxlength - 8; offset++) {
		if(count > maxlength)
			throw "Something fishy";
		int64_t begin64 = readBE<int64_t>(start+offset);
		int32_t begin32 = readBE<int32_t>(start+offset);
		int32_t next32 = readBE<int32_t>(start+offset+4);

		//skip zeros
		if(begin32 == 0) {
			continue;
		}

		//skip atoms such as free/etc.
		if(next32 == *(int32_t *)"free" || next32 == *(int32_t *)"moov" || next32 == *(int32_t *)"hoov"
				|| next32 == *(int32_t *)"moof" || next32 == *(int32_t *)"wide") {
			if(begin32 < maxlength && begin32 > 8)
				offset += begin32 -1;
			continue;
		}

		if(stats.beginnings64.count(begin64)) {
			match = Codec::match(start + offset, maxlength - offset);
			match.offset  = offset;
			if(match.chances > 0) return match;
		}
		//if(name == "avc1") {

		//} else {
		if(stats.beginnings64.size() < 10)
			continue;
		if(stats.beginnings32.count(begin32)) {
			match = Codec::match(start + offset, maxlength - offset);
			match.offset  = offset;
			if(match.chances > 0) return match;
		}
		//}
		count++;
	}
	return Match();
}


/*

bool Codec::matchSample(const unsigned char *start, int maxlength) {
	int32_t s = readBE<int32_t>(start);

	if(name == "rtp ") {
		//return rtpMatch(start, maxlength) > 0;
	} if(name == "avc1") {


	} else if(name == "mp4a") {
		if(s > 1000000) {
			Log::debug << "mp4a: Success because of large s value.\n";
			return true;
		}
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

	} else if(name == "mp4v") {
		// As far as I know, keyframes are 1b3 and frames are 1b6 (ISO/IEC 14496-2, 6.3.4 6.3.5).
		if(s == 0x1b3 || s == 0x1b6)
			return true;
		return false;

	} else if(name == "alac") {
		int32_t t = readBE<int32_t>(start + 4);
		t &= 0xffff0000;

		if(s == 0      && t == 0x00130000) return true;
		if(s == 0x1000 && t == 0x001a0000) return true;
		return false;

	} else if(name == "samr") {
		return start[0] == 0x3c;

	} else if(name == "twos") {
		// Weird audio codec: each packet is 2 signed 16b integers.
		Log::error << "The Twos audio codec is EVIL, there is no hope to guess it.\n";
		throw "Encountered an EVIL audio codec";
		return true;

	} else if(name == "apcn") {
		return memcmp(start, "icpf", 4) == 0;

	} else if(name == "lpcm") {
		// This is not trivial to detect, because it is just
		// the audio waveform encoded as signed 16-bit integers.
		// For now, just test that it is not "apcn" video.
		return memcmp(start, "icpf", 4) != 0;

	} else if(name == "in24") {
		// It's a codec id, in a case I found a pcm_s24le (little endian 24 bit).
		// No way to know it's length.
		return true;

	} else if(name == "sowt") {
		Log::error << "Sowt is just raw data, no way to guess length (unless reliably detecting the other codec start).\n";
		return false;
	}

	return false;
}


int Codec::getLength(unsigned char *start, int maxlength, int &duration) {
	if(name == "rtp ") {
		return rtpMatch(start, maxlength);
	} else if(name == "mp4a") {


	} else if(name == "mp4v") {
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
			consumed = avcodec_decode_video2(context, frame, &got_frame, &avp);
			if(consumed == 0) {
				// Flush decoder to receive buffered packets.
				Log::debug << "Flush " << name << " decoder.\n";
				got_frame = 0;
				av_packet_unref(&avp);
				av_frame_unref(frame);
				int consumed2 = avcodec_decode_video2(context, frame, &got_frame, &avp);
				if(consumed2 >= 0)
					consumed = consumed2;
			}
			av_packet_unref(&avp);
			av_frame_free(&frame);
		}
		return consumed;

	} else if(name == "avc1") {


	} else if(name == "samr") {
		// Lenght is a multiple of 32, we split packets.
		return 32;

	} else if(name == "twos") {
		// Lenght is a multiple of 32, we split packets.
		return 4;

	} else if(name == "apcn") {
		return readBE<int32_t>(start);

	} else if(name == "lpcm") {
		// Use hard-coded values for now....
		const int num_samples      = 4096; // Empirical
		const int num_channels     =    2; // Stereo
		const int bytes_per_sample =    2; // 16-bit
		return num_samples * num_channels * bytes_per_sample;

	} else if(name == "in24") {
		return -1;

	} else
		return -1;
}

bool Codec::isKeyframe(const unsigned char *start, int maxlength) {
	if(name == "avc1") {
		// First byte of the NAL, the last 5 bits determine type
		//   (usually 5 for keyframe, 1 for intra frame).
		return (start[4] & 0x1F) == 5;
	} else
		return false;
} */
