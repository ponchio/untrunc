#include "codec.h"
#include "log.h"
#include <string.h>

using namespace std;
/* mbex is a codec for some editing data I cannot find documentation for.
 * for the few examples I got they are very small packets (8, 10, 100 bytes
 * large packets included crec and cist atoms.
 * Examples:
 *
 * Length 8: 00 00 00 08   00 00 00 00   01 3E 15 29
 * Length 10: 00 00 00 0A   00 00 00 01   00 06 00 00 23 95
 * Length 100: 00 00 00 64    00 00 00 01   00 00 00 5C  63 72 65 63  00 00 00 54  63 69 74 73  00 00 00 0C  00 00 00 02  00 00 00 01
 */

Match Codec::mbexMatch(const unsigned char *start, int maxlength) {

	Match match;
	uint32_t length = readBE<uint32_t>(start);
	if(length > 200) return match;
	match.chances = 1e10/200.0f;
	match.length = length;
	if(!strncmp((char *)start + 8, "crec", 4)) {
		match.chances *= 1e10;
		return match;
	}
	return match;
}

