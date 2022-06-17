#include "codec.h"
#include "log.h"

////it might be encode each sample (1-4) bytes as a packet (AARGH!)


Match Codec::pcmMatch(const unsigned char *start, int maxlength) {

	Match match;
	if(stats.fixed_size)
		match.length = stats.fixed_size;
	match.chances = 2.0f; //we really have no idea
	return match;
}
