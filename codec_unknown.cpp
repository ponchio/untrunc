#include "codec.h"
#include "log.h"

////it might be encode each sample (1-4) bytes as a packet (AARGH!)


Match Codec::unknownMatch(const unsigned char *start, int maxlength) {
	Match match;

	//probably no samples
	if(!stats.fixed_size && !stats.fixed_begin32 && !stats.fixed_begin64)
		return match;

	if(stats.fixed_size)
		match.length = stats.fixed_size;


	int64_t begin64 = readBE<int64_t>(start);

	if(stats.fixed_begin64) {
		if(stats.fixed_begin64 == begin64)
			match.chances = 1e40f;
		else
			match.chances = 0.0f;
		return match;
	}

	int32_t begin32 = readBE<int32_t>(start);
	if(stats.fixed_begin32) {
		if(stats.fixed_begin32 == begin32)
			match.chances = 1e20f;
		else
			match.chances = 0.0f;
		return match;
	}

	//if we have a probability with the beginnig we can do something otherwise just lower it below 2 (for pcm)
	//we will have to look up for patterns, and when chances are really low, use search!
	if(stats.beginnings32.count(begin32))
		match.chances = stats.beginnings32[begin32];
	if(stats.beginnings64.count(begin64))
		match.chances = stats.beginnings64[begin64];

	//match.chances = 0.0f; //we really have no idea

	return match;
}
