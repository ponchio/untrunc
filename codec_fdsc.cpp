#include "codec.h"

/* proprietary gopro codec.
 * Contains:
 * 8	FirmwareVersion
	23	SerialNumber
	87	OtherSerialNumber
	102	Model

	always starts with GP, but I have no idea how to guess the length of the packet.
	maybe follows a pattern? 264, 152 16 16 16...

	https://github.com/stilldavid/gopro-utils can parse the gmpd codec.
	*/

Match Codec::fdscMatch(const unsigned char *start, int maxlength) {

	Match match;
	if(start[0] != 'G' || start[1] != 'P')
		return match;
	match.chances = 1<<16;
	match.length = 0;

	static int idx = -1;
	idx++;
	if (idx == 0) {
		for(auto pos = start + 4; maxlength > 0; pos += 4, maxlength -= 4) {
			if (pos[0] == 'G' && pos[1] == 'P') {
				match.length = pos - start;
				break;
			}
		}
	} else if (idx == 1) {
		match.length = 152;
	} else
		match.length = 16;

	return match;
}
