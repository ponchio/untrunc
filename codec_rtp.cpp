#include "codec.h"
#include "log.h"
#include <cmath>
#include <limits>

//  https://developer.apple.com/standards/qtff-2001.pdf page 153
//  https://en.wikipedia.org/wiki/Real-time_Transport_Protocol#:~:text=The%20RTP%20header%20has%20a,the%20version%20of%20the%20protocol.

Match Codec::rtpMatch(const unsigned char *start, int maxlength) {
	//TODO need to code a stream where we check for maxlength.
	Match match;
	const unsigned char *begin = start;
	uint32_t hinted_size = 0;
	uint32_t hinted_packet = 0;
	uint16_t nentries = readBE<uint16_t>(start);
	uint16_t zeroes = readBE<uint16_t>(start+2);
	if(zeroes != 0)
		return match;
	match.chances = 1;
	start += 4;
	int32_t additional_data_size = 0;
	for(int i = 0; i < nentries; i++) {
		int32_t timestamp = readBE<int32_t>(start);
		// relative timestamp  it should not be too big(but how much?!)
		start += 4;
		uint16_t info = readBE<uint16_t>(start);
		start += 2;
		//bytes 01 is version and is supposed to be 2.
		if(info >> 14 != 2) {
			match.chances = 0;
			return match;
		}
		match.chances *= 4;
		//expect P and X bits to be zero (might not be true)
		if(info & 0x0300) {
			match.chances = 0;
			return match;
		}
		match.chances *= 4;

		//tp payload (https://www.iana.org/assignments/rtp-parameters/rtp-parameters.xhtml)
		//most probably expect 96 or 97.
		uint16_t payload_type= info & 0x007f;
		if(payload_type == 96 || payload_type == 97)
			match.chances *= 64;
		//sequence is per payload_type! and might not start from 0.
		uint16_t sequence = readBE<uint16_t>(start);
		start += 2;

		uint16_t flags = readBE<uint16_t>(start);
		start += 2;
		if(flags > 7) { //only last 3 bits
			match.chances = 0;
			return match;
		}
		bool extraTLV = flags & 0x4;


		uint16_t entries = readBE<uint16_t>(start);
		start += 2;

		if(extraTLV) {
			uint32_t tlv_size = readBE<uint32_t>(start);
			//TODO what is a reasonable max size for tlv?
			if(tlv_size > 1000 ||  start + tlv_size > begin + maxlength) {
				match.chances = 0;
				return match;
			}
			start += tlv_size;
		}

		for(int i = 0; i < entries; i++) {
			uint8_t mode = start[0];
			switch(mode) {

			case 0: //ignore everything
				break;
			case 1: { //immediate mode
				uint8_t length = start[1];
				if (length > 14) {
					match.chances = 0;
					return match;
				}
			}
				break;

			case 2: { //sample mode
				uint8_t track_id = start[1];
				uint16_t length = readBE<uint16_t>(start + 2);
				hinted_size += length;
				int32_t sample_number = readBE<int16_t>(start + 4);
				hinted_packet = sample_number;
				int32_t offset = readBE<int16_t>(start + 8);
				uint16_t byte_per_compression_block = readBE<uint16_t>(start + 12);
				uint16_t samples_per_compression_block = readBE<uint16_t>(start + 14);
				if(track_id == 255) {
					additional_data_size = std::max(additional_data_size, offset + length);

					Log::debug << "Autohint! length: " << length << " offset: " << offset << "\n";
				}

				}
				break;
			case 3: {
				uint8_t track_id = start[1];
				uint16_t length = readBE<uint16_t>(start + 2);
				hinted_size += length;
				int32_t sample_description_number = readBE<int16_t>(start + 4);
				hinted_packet = sample_description_number;
				int32_t offset = readBE<int16_t>(start + 8);
				if(track_id == 255){
					additional_data_size = std::max(additional_data_size, offset + length);
					Log::debug << "Autohint! length: " << length << " offset: " << offset << "\n";
				}
			}
			default:
				match.chances = 0;
				return match;
			}
			start += 16;
			match.chances *= 64.0f;

		}
	}
	//Hinted size is 5 less then the length of the samples! (check track id).
	Log::debug << "Hinted size: " << hinted_size << "\n";
	if(std::isinf(match.chances))
		match.chances = std::numeric_limits<float>::max();

	match.length = start - begin + additional_data_size;
	return match;
}
