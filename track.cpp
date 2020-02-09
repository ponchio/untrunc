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
Track::Track() : trak(NULL), timescale(0), duration(0) { }

void Track::cleanUp() {
	trak      = NULL;
	timescale = 0;
	duration  = 0;
	offsets.clear();
	sizes.clear();
	keyframes.clear();
	times.clear();
	codec.clear();
}

bool Track::parse(Atom *t, Atom *mdat) {
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

	timescale = mdhd->readInt(12);
	duration  = mdhd->readInt(16);

	getSampleTimes(t);
	keyframes = getKeyframes  (t);
	getSampleSizes(t);

	vector<int> chunk_offsets   = getChunkOffsets(t);
	vector<int> sample_to_chunk = getSampleToChunk(t, chunk_offsets.size());

	if(!default_time && !default_size && times.size() != sizes.size()) {
		Log::info << "Mismatch between time offsets and size offsets.\n";
		Log::debug << "Time offsets: " << times.size() << " Size offsets: " << sizes.size() << '\n';
	}
	//assert(times.size() == sizes.size());
	if(!default_time && times.size() != sample_to_chunk.size()) {
		Log::info << "Mismatch between time offsets and sample_to_chunk offsets.\n";
		Log::debug << "Time offsets: " << times.size() << " Chunk offsets: " << sample_to_chunk.size() << '\n';
	}
	// Compute actual offsets.
	int old_chunk = -1;
	int offset = -1;
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

	// Move this stuff into track!
	Atom *hdlr = trak->atomByName("hdlr");
	if(!hdlr) {
		Log::error << "Missing 'Handler' atom (hdlr).\n";
		return false;
	}
	char type[5];
	hdlr->readChar(type, 8, 4);

	if(type != string("soun") && type != string("vide") && type != string("hint")) {
		Log::info << "Not an Audio nor Video nor Hint  track: " << type << "\n";
		return true;
	}

	Log::debug << "Track type: " << type << "\n";
	if(type == string("hint")) {
		Atom *hint =  trak->atomByName("hint");
		hinted_id = hint->readInt(0);
		hint_track = true;
		Log::info << "Found a hint track for track: " << hinted_id << endl;
		//return true;
	}

	if(type == string("hint")) {
		Log::info << "Found hint track" << "\n";
	}


	// If audio, use next?
	//bool audio = (type == string("soun"));

	// Move this to Codec.
	codec.parse(trak, offsets, mdat);
	if(type == string("hint")) //no codec for hints.
		return true;

	if(!codec.context)
		throw string("No codec context.");
	{
		AvLog useAvLog();
		codec.codec = avcodec_find_decoder(codec.context->codec_id);
		if(!codec.codec)
			throw string("No codec found!");
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
	offsets.clear();
	sizes.clear();
	keyframes.clear();
	//times.clear();
}

void Track::fixTimes() {
	if(codec.name == "samr") {
		times.clear();
		assert(default_time == 160);
		//times.resize(offsets.size(), 160);
	}
	if(default_time) {
		duration = default_time * offsets.size();
	} else {
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
		for(int i = 0; i < nsamples; i++)
			times.push_back(time);
	}
}

vector<int> Track::getKeyframes(Atom *t) {
	assert(t != NULL);
	vector<int> sample_key;
	// Chunk offsets.
	Atom *stss = t->atomByName("stss");
	if(!stss)
		return sample_key;

	int32_t entries = stss->readInt(4);
	for(int i = 0; i < entries; i++)
		sample_key.push_back(stss->readInt(8 + 4*i) - 1);
	return sample_key;
}

void Track::getSampleSizes(Atom *t) {
	assert(t != NULL);
	sizes.clear();
	// Chunk offsets.
	Atom *stsz = t->atomByName("stsz");
	if(!stsz)
		throw string("Missing 'Sample Sizes' atom (stsz)");

	int32_t entries      = stsz->readInt(8);
	int32_t default_size_t = stsz->readInt(4);

	if(default_size_t == 0) {
		for(int i = 0; i < entries; i++)
			sizes.push_back(stsz->readInt(12 + 4*i));
	} else {
		default_size = default_size_t;
		sizes.clear();
		sizes.resize(entries, default_size);
	}
}

vector<int> Track::getChunkOffsets(Atom *t) {
	assert(t != NULL);
	vector<int> chunk_offsets;
	// Chunk offsets.
	Atom *stco = t->atomByName("stco");
	if(stco) {
		int32_t nchunks = stco->readInt(4);
		for(int i = 0; i < nchunks; i++)
			chunk_offsets.push_back(stco->readInt(8 + i*4));

	} else {
		Atom *co64 = t->atomByName("co64");
		if(!co64)
			throw string("Missing both 'Chunk Offset' atoms (stco & co64)");

		int32_t nchunks = co64->readInt(4);
		for(int i = 0; i < nchunks; i++) {
			int32_t hi32 = co64->readInt( 8 + i*8); //high order 32-bits
			int32_t lo32 = co64->readInt(12 + i*8); //low  order 32-bits
			if(hi32 != 0) {
				Log::error << "Overflow: 64-bit Chunk Offset value too large ("
						   << ((int64_t(hi32) << 32) | uint32_t(lo32)) << ").\n";
			}
			chunk_offsets.push_back(lo32);
		}
	}
	return chunk_offsets;
}

vector<int> Track::getSampleToChunk(Atom *t, int nchunks){
	assert(t != NULL);
	vector<int> sample_to_chunk;

	Atom *stsc = t->atomByName("stsc");
	if(!stsc)
		throw string("Missing 'Sample to Chunk' atom (stsc)");

	vector<int> first_chunks;
	int32_t entries = stsc->readInt(4);
	for(int i = 0; i < entries; i++)
		first_chunks.push_back(stsc->readInt(8 + 12*i));
	first_chunks.push_back(nchunks+1);

	for(int i = 0; i < entries; i++) {
		int first_chunk = first_chunks[i];
		int last_chunk  = first_chunks[i + 1];
		int32_t nsamples = stsc->readInt(12 + 12*i);

		for(int k = first_chunk; k < last_chunk; k++) {
			for(int j = 0; j < nsamples; j++)
				sample_to_chunk.push_back(k - 1);
		}
	}

	return sample_to_chunk;
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
		stts->writeInt(times.size(), 8);
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
						 4*sizes.size());   //size table
	stsz->writeInt(0, 4);
	stsz->writeInt(sizes.size(), 8);
	for(unsigned int i = 0; i < sizes.size(); i++)
		stsz->writeInt(sizes[i], 12 + 4*i);
}

void Track::saveSampleToChunk() {
	if(!trak)
		return;
	Atom *stsc = trak->atomByName("stsc");
	assert(stsc);
	if(!stsc)
		return;
	stsc->content.resize(4 +                //version
						 4 +                //number of entries
						 12);               //one sample per chunk.
	stsc->writeInt(1,  4);
	stsc->writeInt(1,  8);                  //first chunk (1 based)
	stsc->writeInt(1, 12);                  //one sample per chunk
	stsc->writeInt(1, 16);                  //id 1 (WHAT IS THIS!)
}

void Track::saveChunkOffsets() {
	if(!trak)
		return;
	Atom *co64 = trak->atomByName("co64");
	if(co64) {
		trak->prune("co64");
		Atom *stbl = trak->atomByName("stbl");
		if(stbl) {
			Atom *new_stco = new Atom;
			memcpy(new_stco->name, "stco", min(sizeof("stco"), sizeof(new_stco->name)-1));
			stbl->children.push_back(new_stco);
		}
	}
	Atom *stco = trak->atomByName("stco");
	assert(stco);
	if(!stco)
		return;
	stco->content.resize(4 +                //version
						 4 +                //number of entries
						 4*offsets.size());
	stco->writeInt(offsets.size(), 4);
	for(unsigned int i = 0; i < offsets.size(); i++)
		stco->writeInt(offsets[i], 8 + 4*i);
}

// vim:set ts=4 sw=4 sts=4 noet:
