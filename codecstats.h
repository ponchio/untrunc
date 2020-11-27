#ifndef CODECSTATS_H
#define CODECSTATS_H

#include <vector>
#include <map>
#include <stdint.h>

class Track;
class BufferedAtom;

class CodecStats {
public:
	void init(Track &track, BufferedAtom *mdat);

	//this is most useful for pcm, assuming we are lucky.
	int fixed_size = 0; //zero we don't know
	//if we don't know how to parse a codec, but we know the begin we can still ideintify it.
	int64_t fixed_begin64 = 0;
	int32_t fixed_begin32 = 0;

	std::map<int32_t, float> beginnings32;
	std::map<int64_t, float> beginnings64;
};

#endif // CODECSTATS_H
