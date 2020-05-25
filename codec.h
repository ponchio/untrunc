#ifndef CODEC_H
#define CODEC_H

#include <string>
#include <vector>

#include "codecstats.h"

extern "C" {
#include <stdint.h>
}

struct AVCodecContext;
struct AVCodec;
class Atom;

struct Match {
	uint32_t id = 0;
	uint32_t length = 0;
	uint32_t duration = 0; //audio often provide a duration for the packet.
	float chances = 0.0f; //1/chances is the probability to NOT be a match
	bool keyframe = false;
	bool operator<(const Match &b) { return chances < b.chances; }
};

//the last one is the chosen candidate.
struct MatchGroup: public std::vector<Match> {
	int64_t offset;
};

class Codec {
public:
	std::string     name;
	AVCodecContext *context;
	AVCodec        *codec;

	/* Codec properties */
	uint32_t pcm_bytes_per_sample = 0; //sample size.
	bool pcm = false;
//	bool knows_start = false;
//	bool guess_start = false;
//	bool knows_length = false;
	CodecStats stats;

	Codec();

	bool parse(Atom *trak);
	void clear();

	Match match(const unsigned char *start, int maxlength);
	Match search(const unsigned char *start, int maxlength);

private:
	Match rtpMatch(const unsigned char *start, int maxlength);
	Match avc1Match(const unsigned char *start, int maxlength);
	Match mp4aMatch(const unsigned char *start, int maxlength);
	Match alacMatch(const unsigned char *start, int maxlength);
	Match mbexMatch(const unsigned char *start, int maxlength);
	Match pcmMatch(const unsigned char *start, int maxlength);
	Match textMatch(const unsigned char *start, int maxlength);

	//we just hope statistics on beginning and lenght is enough
	Match unknownMatch(const unsigned char *start, int maxlength);

	int avc1Search(const unsigned char *start, int maxlength);



	// Used by mp4a.
	int mask1;
	int mask0;
};


// Read an unaligned, big-endian value.
// A compiler will optimize this (at -O2) to a single instruction if possible.
template<class T>
static inline T readBE(const uint8_t *p, size_t i = 0) {
	return (i >= sizeof(T)) ? T(0) :
			(T(*p) << ((sizeof(T) - 1 - i) * 8)) | readBE<T>(p + 1, i + 1);
};

template<class T>
static inline void readBE(T &result, const uint8_t *p) { result = readBE<T>(p); };

// Write an unaligned, big-endian value.
template<class T>
static inline void writeBE(uint8_t *p, T value, size_t i = 0) {
	(i >= sizeof(T)) ? void(0) :
		(*p = ((value >> ((sizeof(T) - 1 - i) * 8)) & 0xFF) , writeBE(p + 1, value, i + 1));
};


#endif // CODEC_H
