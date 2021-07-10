//==================================================================//
/*
	Untrunc - track.h

	Untrunc is GPL software; you can freely distribute,
	redistribute, modify & use under the terms of the GNU General
	Public License; either version 2 or its successor.

	Untrunc is distributed under the GPL "AS IS", without
	any warranty; without the implied warranty of merchantability
	or fitness for either an expressed or implied particular purpose.

	Please see the included GNU General Public License (GPL) for
	your rights and further details; see the file COPYING. If you
	cannot, write to the Free Software Foundation, 59 Temple Place
	Suite 330, Boston, MA 02111-1307, USA.  Or www.fsf.org

	Copyright 2010 Federico Ponchio
																	*/
//==================================================================//


#ifndef TRACK_H
#define TRACK_H

#include <vector>
#include <string>


#include "codec.h"

class Atom;

class Track {
public:
	Atom *trak;
	char    type[5];
	int   timescale;
	int   duration;
	int32_t id;

	bool hint_track = false;
	int32_t hinted_id = -1;
	Codec codec;

	int default_time = 0;
	std::vector<int> times;

	//if default size we work using chunks
	int32_t nsamples;
	int default_chunk_nsamples = 0;
	int default_size = 0;  //default SAMPLE size (number of samples!!!!)
	std::vector<int32_t> sample_sizes;   //SAMPLE sizes

	//TODO use CHUNK instead!
	std::vector<int32_t> chunk_sizes;
	std::vector<int64_t> offsets; //CHUNK offsetspopulated only if not default size

	std::vector<int> keyframes; // 0 based!

	//std::vector<int> chunk_offsets;
	//std::vector<int> sample_to_chunk;

	struct Chunk {
		uint64_t offset = 0;
		int32_t size = 0;
		int32_t nsamples = 0;
		int32_t first_sample = 0;
	};

	std::vector<Chunk> chunks;

	Track();

	bool parse(Atom *trak);
	void clear();
	void writeToAtoms();
	void fixTimes();
	int getSize(size_t i) { if(sample_sizes.size()) return sample_sizes[i]; return default_size; }
	int getTimes(size_t i) { if(times.size()) return times[i]; return default_time; }

protected:
	void cleanUp();

	void getSampleTimes  (Atom *t);
	void getSampleSizes  (Atom *t);

	void getKeyframes    (Atom *t);
	void getChunkOffsets (Atom *t);
	void getSampleToChunk(Atom *t);

	void saveSampleTimes();
	void saveKeyframes();
	void saveSampleSizes();
	void saveSampleToChunk();
	void saveChunkOffsets();
};

#endif // TRACK_H
