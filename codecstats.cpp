#include "codecstats.h"
#include "track.h"

#include "atom.h"

#include <map>
#include <iomanip>
#include <iostream>

#include "log.h"
using namespace std;

void CodecStats::init(Track &track, BufferedAtom *mdat) {
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

	float step = 1e20/track.chunks.size();

	int current_sample = 0;
	for(Track::Chunk &chunk: track.chunks) {

		int64_t offset = chunk.offset - mdat->content_start;
		for(int s = 0; s < chunk.nsamples; s++) {
			unsigned char *start = mdat->getFragment(offset, 8);// &(mdat->content[offset]);
			int64_t begin64 = readBE<int64_t>(start);
			int32_t begin32 = readBE<int32_t>(start);

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

