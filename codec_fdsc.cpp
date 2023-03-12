#include "codec.h"

/* proprietary gopro codec.
 * Contains:
 * 8	FirmwareVersion
	23	SerialNumber
	87	OtherSerialNumber
	102	Model

	always starts with GP, but I have no idea how to guess the length of the packet.
	maybe follows a pattern? 264, 152 16 16 16...

	https://github.com/stilldavid/gopro-utils can parse the gpmd codec.
	*/

#include <iostream>
using namespace std;

Match Codec::fdscSearch(const unsigned char *start, int maxlength, int maxskip) {
	Match match;
	const unsigned char *end = start + maxskip;
	const  unsigned char *current = start;

	while(current < end) {
		if(current[0] == 'G' && current[1] == 'P') {
			match.chances = 1<<20;
			match.offset = current - start;
			return match;
		}
		current++;
	}
	return match;
}


Match Codec::fdscMatch(const unsigned char *start, int maxlength) {

	Match match;
	if(start[0] != 'G' || start[1] != 'P')
		return match;
	match.chances = 1<<16;
	match.length = 0;



	if(start[2] == 'R' && start[3] == 'O') {
		match.length = *(uint16_t *)(start + 4);
		return match;
	}

	uint8_t type = start[2]; //assuming it's a single byte and actualy is a type.
	//type 00 seems to be followed always by 03

	match.length = 16;
	if(type == 0) {
		//this seems to be a marker for the next sample.
		uint32_t next_sample_length = readBE<int>(start +4);
	}

	if(type == 3) { //it seems this is a thumbnail type pointing to the first frame of the video, similar to type 0, but longer.
		//match.length = 16;
		//match.length += 4 + readBE<int>(start +16); //maybe here is a lenght, but it's not the last one. and padding is possible,

		match.length = 152; //found 152 in the single sample I got.
	}

	if(type == 4 || type == 5) { //contains some data.
		//a length is int32 at byte 4, don't kwnow which data.
		match.length = 16;
	}
	if(type == 15) //another wild guess.
		match.length = 220;
	/*
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
		match.length = 16; */

	return match;
}
