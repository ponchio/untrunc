#include "codec.h"
#include "log.h"

////it might be encode each sample (1-4) bytes as a packet (AARGH!)


Match Codec::pcmMatch(const unsigned char *start, int maxlength) {

	Match match;
	if(stats.fixed_size) {
		match.length = stats.fixed_size;
//For unknown reason the lenght of the pcm sometimes does not equal sample size *Nchannels*nsamples
//and has to be determined manually (in an example nsample was reported as 24024!)
//		match.length = 24064*12
	}
	match.chances = 2.0f; //we really have no idea
	return match;
}
