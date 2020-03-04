#include "codec.h"
#include "log.h"

////it might be encode each sample (1-4) bytes as a packet (AARGH!)


Match Codec::pcmMatch(const unsigned char *start, int maxlength) {

	Match match;
	match.length = pcm_bytes_per_sample;
	match.chances = 2; //we really have no idea
	return match;
}
