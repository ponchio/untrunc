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

	return match;
}
