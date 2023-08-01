#include "codec.h"
#include "log.h"

////it might be encode each sample (1-4) bytes as a packet (AARGH!)


Match Codec::unknownMatch(const unsigned char *start, int maxlength) {
	Match match;

	if(name == "rtmd") {
		int32_t begin32 = readBE<int32_t>(start);

		if(stats.beginnings32.count(begin32))
			match.chances = 10*stats.beginnings32[begin32];
		match.length = 1024;
//		match.length = 5120; sometimes found with this size (use stats!)
		return match;
	}
	//probably no samples
	if(!stats.fixed_size)
		return match;

	if(stats.fixed_size)
		match.length = stats.fixed_size;

	if(name == "samr") {
		//samr special thing might be 160... or different values.

		static const uint8_t amrnb_packed_size[16] = {
			13, 14, 16, 18, 20, 21, 27, 32, 6, 1, 1, 1, 1, 1, 1, 1
		};
		static const uint8_t amrwb_packed_size[16] = {
			18, 24, 33, 37, 41, 47, 51, 59, 61, 6, 1, 1, 1, 1, 1, 1
		};
		int mode = (start[0] >> 3) & 0xf;
		if (mode > 9 || (start[0] & 0x4) != 0x4) {
			match.chances = 0;
			return match;
		}
		match.length = amrnb_packed_size[mode];
		match.chances = 4;
	}




	int64_t begin64 = readBE<int64_t>(start);
	int32_t begin32 = readBE<int32_t>(start);

	/*if(stats.fixed_begin64) {
		if(stats.fixed_begin64 == begin64)
			match.chances = 1e40f;
		else
			match.chances = 0.0f;
		return match;
	}

	if(stats.fixed_begin32) {
		if(stats.fixed_begin32 == begin32)
			match.chances = 1e20f;
		else
			match.chances = 0.0f;
		return match;
	}*/

	//if we have a probability with the beginnig we can do something otherwise just lower it below 2 (for pcm)
	//we will have to look up for patterns, and when chances are really low, use search!
	if(stats.beginnings32.count(begin32))
		match.chances = stats.beginnings32[begin32];
	if(stats.beginnings64.count(begin64))
		match.chances = stats.beginnings64[begin64];

	//match.chances = 0.0f; //we really have no idea

	return match;
}
