#include "codecstats.h"
#include "track.h"

#include "atom.h"

#include <map>
#include <iomanip>
#include <iostream>

#include <math.h>
#include "log.h"
using namespace std;


int GCD(int a, int b) {
	if (a == 0)
		return b;
	if(b == 0)
		return a;
	if (a > b)
		return GCD(a-b, b);
	return GCD(a, b-a);
}


void CodecStats::init(Track &track, BufferedAtom *mdat) {

	variance = 0;

	if(track.default_time) {
		average_time = min_time = max_time = track.default_time;
		average_time = 0;
	} else {
		for(int t: track.times) {
			average_time += t;
			min_time = std::min(min_time, t);
			max_time = std::max(max_time, t);
		}
		average_time /= (double)track.times.size();
		for(int t: track.times) {
			variance += pow(t - average_time, 2.0);
		}
		variance /= (double)track.times.size();
		variance = sqrt(variance);
	}
	std::vector<Track::Chunk> &chunks = track.chunks;
	if(!chunks.size())
		return;
	fixed_size = chunks[0].size;
	//last chunk is skipped as in pcm could be a bit smaller.
	for(size_t i = 0; i < chunks.size()-1; i++) {
		auto &c = chunks[i];
		if(c.size != fixed_size) {
			fixed_size = 0;
			break;
		}
	}

	//it might happens that chunks get just grouped, find GCD
	if(!fixed_size && track.codec.pcm) {
		int gcd = 0;
		for(size_t i = 0; i < chunks.size()-1; i++) {
			gcd = GCD(gcd, chunks[i].size);
		}
		if(gcd >= 128)
			fixed_size = gcd;
	}

	float step = (1<<20)/track.chunks.size();

	int current_sample = 0;
	for(Track::Chunk &chunk: track.chunks) {

		int64_t offset = chunk.offset - mdat->content_start;
		for(int s = 0; s < chunk.nsamples; s++) {

			int size = track.getSize(s);
			largestSample = std::max(size, largestSample);
			smallestSample = std::min(size, smallestSample);

			unsigned char *start = mdat->getFragment(offset, 8);// &(mdat->content[offset]);
			int64_t begin64 = readBE<int64_t>(start);
			int32_t begin32 = readBE<int32_t>(start);
			if(track.codec.name == "avc1") {
				begin64 = readBE<int32_t>(start + 4);
				begin32 = readBE<int32_t>(start + 4) && 0x0000ffff;
			}

			if(!beginnings64.count(begin64)) {
				beginnings64[begin64] = 1;
			} else {
				beginnings64[begin64]+= step;
			}
			if(!beginnings32.count(begin32)) {
				beginnings32[begin32] = step;
			} else {
				beginnings32[begin32]+= step;
			}
			if(!track.default_size) { //pcm codecs with small samples.
				offset += track.sample_sizes[current_sample];
				current_sample++;
			} else if(track.default_size > 80) {
				offset += track.default_size;
				current_sample++;
			} else
				break;
		}
	}
	Log::debug << "Beginnings: \n";
	int maxcount = 20;
	for(auto &c: beginnings64) {
		if(!maxcount--) break;
		Log::debug << hex << setw(116) << c.first << ": " << c.second << dec << endl;
	}
}

