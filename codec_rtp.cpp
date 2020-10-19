#include "codec.h"
#include "log.h"

//  https://developer.apple.com/standards/qtff-2001.pdf page 153

Match Codec::rtpMatch(const unsigned char *start, int maxlength) {
	Match match;
	const unsigned char *begin = start;
	uint32_t hinted_size = 0;
	uint32_t hinted_packet = 0;
	uint16_t nentries = readBE<uint16_t>(start);
	uint16_t zeroes = readBE<uint16_t>(start+2);
	if(zeroes != 0) return match;
	match.chances *= 1<<16;
	start += 4;
	for(int i = 0; i < nentries; i++) {
		int32_t timestamp = readBE<int32_t>(start);
		// relative timestamp  it should not be too big(but how much?!)
		start += 4;
		uint16_t info = readBE<uint16_t>(start);
		start += 2;
		//bytes 01 is version and is supposed to be 2.
		if(info >> 14 != 2) {
			return match;
		}
		match.chances *= 4;
		//expect P and X bits to be zero (might not be true)
		if(info & 0x0300) {
			return match;
		}
		match.chances *= 4;

		//tp payload (https://www.iana.org/assignments/rtp-parameters/rtp-parameters.xhtml)
		//most probably expect 96 or 97.
		uint16_t payload_type= info & 0x007f;
		uint16_t sequence = readBE<uint16_t>(start);
		start += 2;

		uint16_t flags = readBE<uint16_t>(start);
		start += 2;

		uint16_t entries = readBE<uint16_t>(start);
		start += 2;

		for(int i = 0; i < entries; i++) {
			uint8_t mode = start[0];
			switch(mode) {

			case 0: //ignore everything
				break;
			case 1: { //immediate mode
				uint8_t length = start[1];
				if (length > 14)
					return match;
				}
				break;
			case 2: { //sample mode
				uint8_t track_id = start[1];
				uint16_t length = readBE<uint16_t>(start + 2);
				hinted_size += length;
				int32_t packet_number = readBE<int16_t>(start + 4);
				hinted_packet = packet_number;
				int32_t offset = readBE<int16_t>(start + 8);
				uint16_t byte_per_compression_block = readBE<uint16_t>(start + 12);
				uint16_t samples_per_compression_block = readBE<uint16_t>(start + 14);
				}
				break;
			case 3: //no idea;
				break;
			default: return match;

			}
			start += 16;
			match.chances *= 64.0f;

		}
	}
	match.length = begin - start;
	return match;
}
