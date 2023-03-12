#include "codec.h"
#include "log.h"
#include <string.h>

using namespace std;
/*  MIJD is a proprietary codec...
 *  it looks like it contains a couple of jpegs,
 * at offset 28 is the relative begin of the first image
 * at 32 the length of the first image
 * at 34 and 36 width and height of the image
 * at 44 relative begin of the second image
 * at 48 length of the seocnd image
 * at 52 - 54 width and height of the second image.
 *
*/

Match Codec::mijdMatch(const unsigned char *start, int maxlength) {

	Match match;

	if(!strncmp((char *)start, "mijd", 4)) {
		uint32_t off = *(uint32_t *)(start + 44);
		uint32_t length = *(uint32_t *)(start + 48);
		match.length = off + length;
		match.chances  = 1e30;
		return match;
	}
	uint32_t word = *(uint32_t *)start;
	if(word == 0x3030f800) {
		match.length = 250;
		match.chances  = 1e30;
		return match;
	}

	return match;
}

Match Codec::mijdSearch(const unsigned char *start, int maxlength, int maxskip) {
	Match match;
	const unsigned char *end = start + maxskip;
	const  unsigned char *current = start;
	while(current < end) {
		uint32_t word = *(uint32_t *)start;
		if(!strncmp((char *)current, "mijd", 4) || word == 0x3030f800) {
			match.offset = current - start;
			match.chances  = 1e30;
			return match;
		}

		current++;
	}
	return match;
}

