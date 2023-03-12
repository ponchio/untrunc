#include "codec.h"
#include "log.h"

#include <string.h>

/* codec text for quicktime chapters.
 * https://developer.apple.com/standards/qtff-2001.pdf text sample data
 * atom structure 16 bit for size
 * the end might be is (always?) 00 00 00 0C 'encd' 00 00 01 00
 * but it's not documented!!
 * idea is to follow the atom structure up to maxsize, if we find an encd atom we are done.
 *
 * encoding in movenc.c only saves text and encd atom.
 */

Match Codec::textMatch(const unsigned char *start, int maxlength) {
	Match match;

	int32_t begin = readBE<int32_t>(start);
	if(stats.fixed_size && stats.beginnings32.size() == 1) {
		if(stats.beginnings32.count(begin)) {
			match.chances = 1<<20;
			match.length = stats.fixed_size;
		}
		return match;
	}

	//TODO find more sensible values for these maxes
	uint32_t max_text_length = std::min(maxlength, 4096);
	uint32_t max_atom_length = 4096; //?
	uint32_t offset = 0.0f;


	uint16_t size;
	readBE<uint16_t>(size, start + offset);

	//probably there is a max size here!
	if(size > max_text_length)
		return match;

	//expect the last byte of the text to be zero? ffmpeg does not but sound only samples that does that.
	if(start[offset + size + 1] != 0)
		match.chances = 256.0f;
		//return match;

	offset += size + 2; //length does not include size word


	//TODO if text is ascii changes might be significantly higher!
	//small text or already seen beginnings.
	if(size < 128 || stats.beginnings32.count(begin)) {
		match.chances = stats.beginnings32[begin];
		match.length = size+2;
		return match;
	}


	const char *atoms[9] = { "encd", "styl", "ftab", "hlit", "hclr", "drpo", "drpt", "imag", "metr" };
	while(offset < maxlength - 10) { //shortest atom has length(4) name(4) shadow transparency(2)
		uint32_t length;
		readBE<uint32_t>(length, start + offset);
		if(length > max_atom_length)
			return match;

		const char *atom = (char *)(start + offset + 4);

		bool atom_found = false;
		for(int i = 0; i < 9; i++) {
			if(!strncmp(atoms[i], atom, 4)) {
				match.chances = 1<<20;
				match.length = offset + length ;
				atom_found = true;
			}
		}
		if(!atom_found)
			break;

		offset += length;
	}
	return match;

}
