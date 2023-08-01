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

	//keep track of min and max timing, useful for variable timing.
	int min_time = 0xffffff;
	int max_time = 0;
	double average_time = 0;
	double variance = 0;

	//this is most useful for pcm, assuming we are lucky.
	int fixed_size = 0; //zero we don't know
	//if we don't know how to parse a codec, but we know the begin we can still ideintify it.
	int64_t fixed_begin64 = 0;
	int32_t fixed_begin32 = 0;
	int32_t largestSample = 0;
	int32_t smallestSample = (1<<20);

	std::map<int32_t, float> beginnings32;
	std::map<int64_t, float> beginnings64;
};

#endif // CODECSTATS_H
