#include "codec.h"
#include "log.h"
#include <string.h>

using namespace std;
/*
Apple ProRes 422 High Quality: 'apch' ('hcpa' in little-endian)
Apple ProRes 422 Standard Definition: 'apcn' ('ncpa' in little-endian)
Apple ProRes 422 LT: 'apcs' ('scpa' in little-endian)
Apple ProRes 422 Proxy: 'apco' ('ocpa' in little-endian)
Apple ProRes 4444: 'ap4h' ('h4pa' in little-endian)
*/

Match Codec::apchMatch(const unsigned char *start, int maxlength) {

	Match match;
	uint32_t length = readBE<uint32_t>(start);

	if(!strncmp((char *)start + 4, "icpf", 4)) {
		match.length = length;
		match.chances  = 1e30;
		return match;
	}
	return match;
}

Match Codec::apchSearch(const unsigned char *start, int maxlength, int maxskip) {
	Match match;
	const unsigned char *end = start + maxskip;
	const  unsigned char *current = start;
	while(current < end) {
		if(!strncmp((char *)current + 4, "icpf", 4)) {
			match.offset = current - start;
			match.chances  = 1e30;
			return match;
		}
		current++;
	}
	return match;
}

