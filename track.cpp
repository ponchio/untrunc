//==================================================================//
/*
	Untrunc - track.cpp

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

#include <vector>
#include <string>
#include <iostream>


#include "track.h"
#include "atom.h"
#include "log.h"
#include "avlog.h"


using namespace std;


// Track.
Track::Track() : trak(nullptr), timescale(0), duration(0) { }

void Track::cleanUp() {
	trak      = NULL;
	timescale = 0;
	duration  = 0;
	offsets.clear();
	chunks.clear();
	sample_sizes.clear();
	keyframes.clear();
	times.clear();
	codec.clear();
}

bool Track::parse(Atom *t) {
	cleanUp();

	if(!t) {
		Log::error << "Missing 'Container for an individual Track or stream' atom (trak).\n";
		return false;
	}
	trak = t;
	Atom *tkhd = trak->atomByName("tkhd");
	id = tkhd->readInt(12);

	Atom *mdhd = t->atomByName("mdhd");
	if(!mdhd)
		throw string("Missing 'Media Header' atom (mdhd): Unknown duration and timescale");

	//required before getting other properties (it reads stsd)
	codec.parse(trak);

	timescale = mdhd->readInt(12);
	duration  = mdhd->readInt(16);

	getSampleTimes(t);
	getKeyframes  (t);
	getSampleSizes(t);

	if(default_size) {
		Log::debug << "Default size: " << default_size << endl;
	}

	getChunkOffsets(t);
	getSampleToChunk(t);

	if(!default_time && !default_size && times.size() != sample_sizes.size()) {
		Log::info << "Mismatch between time offsets and size offsets.\n";
		Log::debug << "Time offsets: " << times.size() << " Size offsets: " << sample_sizes.size() << '\n';
	}
	//assert(times.size() == sizes.size());
/*	if(!default_time && times.size() != sample_to_chunk.size()) {
		Log::info << "Mismatch between time offsets and sample_to_chunk offsets.\n";
		Log::debug << "Time offsets: " << times.size() << " Chunk offsets: " << sample_to_chunk.size() << '\n';
	} */
	// Compute actual offsets.
/*	int old_chunk = -1;
	int offset = -1;
	if(sizes.size()) {
		for(unsigned int i = 0; i < sizes.size(); i++) {
			int chunk = sample_to_chunk[i];
			int size = sizes[i];
			if(chunk != old_chunk) {
				offset = chunk_offsets[chunk];
				old_chunk= chunk;
			}
			offsets.push_back(offset);
			offset += size;
		}
	} else {
		for(unsigned int i = 0; i < sample_to_chunk.size(); i++) {
			int chunk = sample_to_chunk[i];
			if(chunk != old_chunk) {
				offset = chunk_offsets[chunk];
				old_chunk= chunk;
			}
			offsets.push_back(offset);
			offset += default_size;
		}
	} */

	// Move this stuff into track!
	Atom *hdlr = trak->atomByName("hdlr");
	if(!hdlr) {
		Log::error << "Missing 'Handler' atom (hdlr).\n";
		return false;
	}
	hdlr->readChar(type, 8, 4);

	Log::debug << "Track type: " << type << "\n";

	if(type != string("soun") && type != string("vide")) {
		Log::info << "Found '" << type << "' track. Might be not supported.\n";
	}

	if(type == string("hint")) {
		Atom *hint =  trak->atomByName("hint");
		hinted_id = hint->readInt(0);
		hint_track = true;
		Log::info << "Found a hint track for track: " << hinted_id << endl;
	}

	if(!codec.context)
		throw string("No codec context.");
	{
		AvLog useAvLog();
		codec.codec = avcodec_find_decoder(codec.context->codec_id);
		if(!codec.codec) {
			Log::info <<  "No codec found for track of type: " << type << "\n";
			return true;
		}
		if(avcodec_open2(codec.context, codec.codec, NULL) < 0) {
			throw string("Could not open codec: ")
					+ ((codec.context->codec && codec.context->codec->name)? codec.context->codec->name : "???");
		}
	}

#if 0
	if(!mdat)
		throw string("Missing 'Media Data container' atom (mdat)");

	// Print sizes and offsets.
	clog << "Track codec: " << codec.name << '\n';
	clog << "Sizes      : " << sizes.size() << '\n';
	for(unsigned int i = 0; i < 10 && i < sizes.size(); i++) {
		int64_t offset = offsets[i] - (mdat->start + 8);
		int64_t begin  = mdat->readInt(offset);
		int64_t next   = mdat->readInt(offset + 4);
		int64_t end    = mdat->readInt(offset + sizes[i] - 4);
		// Use <iomanip> for layout.
		clog << setw(8) << i
			 << " Size: " << setw(6) << sizes[i]
				<< " offset " << setw(10) << offsets[i]
				   << "  begin: " << hex << setw(5) << begin << ' ' << setw(8) << next
				   << " end: " << setw(8) << end << dec << '\n';
	}
	if(sizes.size() > 10)
		clog << "...\n";
	clog << endl;
#endif
	return true;
}

void Track::writeToAtoms() {
	if(!trak)
		return;

	if(keyframes.empty())
		trak->prune("stss");

	saveSampleTimes();
	saveKeyframes();
	saveSampleSizes();
	saveSampleToChunk();
	saveChunkOffsets();

	Atom *mdhd = trak->atomByName("mdhd");
	if(!mdhd)
		Log::error << "Missing 'Media Header' atom (mdhd).\n";
	else
		mdhd->writeInt(duration, 16);

	// Avc1 codec writes something inside stsd.
	// In particular the picture parameter set (PPS) in avcC (after the sequence parameter set).
	// For avcC see: <http://jaadec.sourceforge.net/specs/ISO_14496-15_AVCFF.pdf>.
	// For PPS  see: <https://www.iitk.ac.in/mwn/vaibhav/Vaibhav%20Sharma_files/h.264_standard_document.pdf>.
	// For PPS  see: <http://last.hit.bme.hu/download/firtha/video/h264/Iain%20E.%20Richardson%20-%20H264%20(2nd%20edition).pdf>.
	// I have found out in shane,banana,bruno and nawfel that pic_init_qp_minus26 is different
	//  even for consecutive videos of the same camera.
	// The only thing to do then is to test possible values,
	//  to do so remove 28 (the NAL header) follow the golomb stuff.
	// This test could be done automatically when decoding fails..
	//#define SHANE 6
#ifdef SHANE
	int pps[15] = {
		0x28ee3880,
		0x28ee1620,
		0x28ee1e20,
		0x28ee0988,
		0x28ee0b88,
		0x28ee0d88,
		0x28ee0f88,
		0x28ee0462,
		0x28ee04e2,
		0x28ee0562,
		0x28ee05e2,
		0x28ee0662,
		0x28ee06e2,
		0x28ee0762,
		0x28ee07e2
	};
	if(codec.name == "avc1") {
		Atom *stsd = trak->atomByName("stsd");
		if(stsd)
			stsd->writeInt(pps[SHANE], 122); //a bit complicated to find.... find avcC... follow first link.
	}
#endif

}

void Track::clear() {
	nsamples = 0;
	offsets.clear();
	sample_sizes.clear();
	keyframes.clear();
	//times.clear();
}

void Track::fixTimes() {
	if(codec.name == "samr") {
		times.clear();
		default_time = 160;
	}
	if(default_time || times.size() == 0) {
		duration = default_time * nsamples;
	} else {

		assert(times.size() > 0);
		while(times.size() < offsets.size())
			times.insert(times.end(), times.begin(), times.end());
		times.resize(offsets.size());

		duration = 0;
		for(unsigned int i = 0; i < times.size(); i++)
			duration += times[i];
	}
}

void Track::getSampleTimes(Atom *t) {
	assert(t != NULL);
	times.clear();
	// Chunk offsets.
	Atom *stts = t->atomByName("stts");
	if(!stts)
		throw string("Missing 'Sync Sample Table' atom (stts)");

	int32_t entries = stts->readInt(4);

	for(int i = 0; i < entries; i++) {
		int32_t nsamples = stts->readInt( 8 + 8*i);
		int32_t time     = stts->readInt(12 + 8*i);
		if(entries == 1) {
			default_time = time;
			break;
		}
		for(int i = 0; i < nsamples; i++) {
			times.push_back(time);
		}
	}
	//check if times are always the same
	if(entries != 1) {
		int common_time = times[0];
		for(int t: times) {
			if(t != common_time) {
				common_time = 0;
				break;
			}
		}
		if(common_time) {
			default_time = common_time;
			times.clear();
		}
	}

}

void Track::getKeyframes(Atom *t) {
	assert(t != NULL);
	vector<int> keyframes;
	// Chunk offsets.
	Atom *stss = t->atomByName("stss");
	if(!stss)
		return;

	int32_t entries = stss->readInt(4);
	for(int i = 0; i < entries; i++)
		keyframes.push_back(stss->readInt(8 + 4*i) - 1);
	return;
}

void Track::getSampleSizes(Atom *t) {
	assert(t != NULL);
	sample_sizes.clear();
	// Chunk offsets.
	Atom *stsz = t->atomByName("stsz");
	if(!stsz)
		throw string("Missing 'Sample Sizes' atom (stsz)");

	nsamples      = stsz->readInt(8);
	default_size = stsz->readInt(4); 

	if(default_size == 0) {
		for(int i = 0; i < nsamples; i++)
			sample_sizes.push_back(stsz->readInt(12 + 4*i));
	}
}

void Track::getChunkOffsets(Atom *t) {
	assert(t != NULL);

	int32_t nchunks = 0;

	Atom *stco = t->atomByName("stco");
	if(stco)
		nchunks = stco->readInt(4);

	Atom *co64 = t->atomByName("co64");
	if(co64)
		nchunks = co64->readInt(4);

	if(nchunks == 0)
		Log::debug << "Missing both 'Chunk Offset' atoms (stco & co64) or no chunks!";

	chunks.resize(nchunks);
	if(stco) {
		for(int i = 0; i < nchunks; i++)
			chunks[i].offset = stco->readUInt(8 + i*4);
	} else {
		for(int i = 0; i < nchunks; i++)
			chunks[i].offset = co64->readInt64(8 + i*8);
	}
	return;
}

void Track::getSampleToChunk(Atom *t){
	assert(t != NULL);

	Atom *stsc = t->atomByName("stsc");
	if(!stsc)
		throw string("Missing 'Sample to Chunk' atom (stsc)");

	vector<int> first_chunks;
	int32_t entries = stsc->readInt(4);
	for(int i = 0; i < entries; i++)
		first_chunks.push_back(stsc->readInt(8 + 12*i)-1);

	//assert(first_chunks.back() == chunks.size());

	default_chunk_nsamples = -1;

	int32_t count = 0;
	for(int i = 0; i < entries; i++) {
		int first_chunk  = first_chunks[i];
		int last_chunk =  (i == entries-1)? chunks.size() : first_chunks[i+1];
		for(int k = first_chunk; k < last_chunk; k++) {
			chunks[k].first_sample = count;
			chunks[k].nsamples = stsc->readInt(12 + 12*i);
			if(default_chunk_nsamples == -1)
				default_chunk_nsamples = chunks[k].nsamples;
			else if(default_chunk_nsamples != chunks[k].nsamples)
				default_chunk_nsamples = 0;

			if(default_size) {
				//assert(codec.pcm_bytes_per_sample > 0);
				chunks[k].size = chunks[k].nsamples * codec.pcm_bytes_per_sample;
				count += chunks[k].nsamples ;

			} else {
				uint64_t offset = chunks[k].offset;
				for(int s = 0; s < chunks[k].nsamples; s++) {
					int32_t size = sample_sizes[count++];
					chunks[k].size += size;
					offsets.push_back(offset);
					offset += size;
				}
			}
		}
		first_chunk = last_chunk;
	}
	//assert(count == nsamples);

	return;

}


void Track::saveSampleTimes() {
	if(!trak)
		return;
	Atom *stts = trak->atomByName("stts");
	assert(stts);
	if(!stts)
		return;
	int nentries = default_time? 1 : times.size();
	stts->content.resize(4 +                //version
						 4 +                //entries
						 8*nentries);   //time table
	stts->writeInt(nentries, 4);
	if(default_time) {
		//TODO
		stts->writeInt(nsamples, 8);
		stts->writeInt(default_time, 12);
	} else {
		for(unsigned int i = 0; i < times.size(); i++) {
			stts->writeInt(1, 8 + 8*i);
			stts->writeInt(times[i], 12 + 8*i);
		}
	}
}

void Track::saveKeyframes() {
	if(!trak)
		return;
	Atom *stss = trak->atomByName("stss");
	if(!stss)
		return;
	assert(keyframes.size() > 0);
	if(keyframes.empty())
		return;

	stss->content.resize(4 +                  //version
						 4 +                  //entries
						 4*keyframes.size()); //time table
	stss->writeInt(keyframes.size(), 4);
	for(unsigned int i = 0; i < keyframes.size(); i++)
		stss->writeInt(keyframes[i] + 1, 8 + 4*i);
}

void Track::saveSampleSizes() {
	if(!trak)
		return;
	Atom *stsz = trak->atomByName("stsz");
	assert(stsz);
	if(!stsz)
		return;

	stsz->content.resize(4 +                //version
						 4 +                //default size
						 4 +                //entries
						 4*sample_sizes.size());   //size table
	stsz->writeInt(0, 4);
	stsz->writeInt(default_size, 4);
	if(default_size) {
		stsz->writeInt(nsamples, 8);
	} else {
		stsz->writeInt(sample_sizes.size(), 8);
		for(unsigned int i = 0; i < sample_sizes.size(); i++)
			stsz->writeInt(sample_sizes[i], 12 + 4*i);
	}
}

void Track::saveSampleToChunk() {
	if(!trak)
		return;
	Atom *stsc = trak->atomByName("stsc");
	assert(stsc);
	if(!stsc)
		return;
	//for non pcm codecs we just assume 1
	if(!codec.pcm && default_chunk_nsamples == 0) {
		default_chunk_nsamples = 1;
		Log::debug <<  "Don;t know how to deal with variable sample size and variable number of samples per chunk. Trying with 1.\n";
	}

	if(default_size == 0) { //video might put more samples in the same chunk, even a constant number, default_chunk_nsamples might be != 0
		//TODO we should distinguish those cases.
		//so if we don;t have a default number of samples per chunk we save one sample per chunk.
		stsc->content.resize(4 +                //version
							 4 +                //number of entries
							 12);               //one sample per chunk.
		stsc->writeInt(1,  4);
		stsc->writeInt(1,  8);                  //first chunk (1 based)
		stsc->writeInt(1, 12);                  //one sample per chunk
		stsc->writeInt(1, 16);                  //id 1 (WHAT IS THIS!)
	} else if(default_chunk_nsamples != 0) {
		stsc->content.resize(4 +                //version
							 4 +                //number of entries
							 12);               //one sample per chunk.
		stsc->writeInt(1,  4);
		stsc->writeInt(1,  8);                  //first chunk (1 based)
		stsc->writeInt(default_chunk_nsamples, 12);                  //one sample per chunk
		stsc->writeInt(1, 16);                  //id 1 (WHAT IS THIS!)

	} else { //default size but not default chunk nsamples
		stsc->content.resize(4 +                //version
							 4 +                //number of entries
							 12*chunk_sizes.size());               //one sample per chunk.
		stsc->writeInt(chunk_sizes.size(),  4);
		for(int i = 0; i < chunk_sizes.size(); i++) {
			stsc->writeInt(i+1,  8 + 12*i);                  //first chunk (1 based)
			int chunk_nsamples;
			if(codec.pcm)
				chunk_nsamples = chunk_sizes[i]/ codec.pcm_bytes_per_sample;
			else
				chunk_nsamples = chunk_sizes[i]/ default_size;
			stsc->writeInt(chunk_nsamples, 12 + 12*i);
			stsc->writeInt(1, 16 + 12*i);                  //id 1 (WHAT IS THIS!)
		}
	}
}

void Track::saveChunkOffsets() {
	if(!trak)
		return;
	Atom *stco = trak->atomByName("stco");
	if(stco) {
		trak->prune("stco");
		Atom *stbl = trak->atomByName("stbl");
		if(stbl) {
			Atom *new_co64 = new Atom;
			memcpy(new_co64->name, "co64", min(sizeof("co64"), sizeof(new_co64->name)-1));
			stbl->children.push_back(new_co64);
		}
	}
	Atom *co64 = trak->atomByName("co64");
	assert(co64);

	co64->content.resize(4 +                //version
						 4 +                //number of entries
						 8*offsets.size());
	co64->writeInt(0, 4);
	co64->writeInt(offsets.size(), 4);
	for(unsigned int i = 0; i < offsets.size(); i++)
		co64->writeInt64(offsets[i], 8 + 8*i);
}

// vim:set ts=4 sw=4 sts=4 noet:
