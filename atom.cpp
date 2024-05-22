//==================================================================//
/*
	Untrunc - atom.cpp

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

#include "AP_AtomDefinitions.h"
#include "atom.h"
#include "log.h"

#include <map>
#include <iostream>

#include <cstring>      //for: memcpy()
#include <cassert>

using namespace std;


namespace {
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

// Read an unaligned value in native-endian format.
// Encode the unaligned access intention by using memcpy() with its
//  destination and source pointing to types with the wanted alignment.
// Some compilers use the alignments of these types for further optimizations.
// A compiler can optimize this memcpy() into a single instruction.
template<class T>
static inline T readNE(const uint8_t *p) {
	T value;
	memcpy(&value, p, sizeof(value));
	return value;
}

template<class T>
static inline void readNE(T &result, const uint8_t *p) {
	memcpy(&result, p, sizeof(result));
}


// Atom definitions map.
static inline uint32_t id2Key(const char *id) {
	const unsigned char *uid = reinterpret_cast<const unsigned char*>(id);
	return ((uint32_t(uid[0]) << 24) | (uint32_t(uid[1]) << 16) | (uint32_t(uid[2]) << 8) | uid[3]);
}

AtomDefinition definition(const char *id) {
	static const AtomDefinition def_unknown = KnownAtoms[0];
	static map<uint32_t, AtomDefinition> def;
	if(def.empty()) {
		for(unsigned int i = 1; i < sizeof(KnownAtoms)/sizeof(KnownAtoms[0]); ++i) {
#if 1
			//for each atom name include the last of multiple definitions
			def[id2Key(KnownAtoms[i].known_atom_name)] = KnownAtoms[i];
#else
			//for each atom name include only the first of multiple definitions
			def.insert(make_pair(id2Key(KnownAtoms[i].known_atom_name), KnownAtoms[i]));
#endif
		}
	}

	if(id) {
		map<uint32_t, AtomDefinition>::const_iterator it = def.find(id2Key(id));
		if(it != def.end())
			return it->second;
	}
	return def_unknown;
}
}; //namespace



Atom::~Atom() {
	for(unsigned int i = 0; i < children.size(); i++)
		delete children[i];
}


void Atom::parseHeader(File &file) {
	start = file.pos();
	content_start += start + 8;
	length = file.readUInt();
	file.readChar(name, 4);

	if(length == 1) {
		length64 = true;
		length = file.readInt64();
		content_start += 8;
	} else if(length == 0) {
		length = file.length() - start;
	}
}

void Atom::parse(File &file) {
	parseHeader(file);

	if(isParent(name) && name != string("udta")) { //user data atom is dangerous... i should actually skip all
		while(file.pos() < start + length) {
			Atom *atom = new Atom;
			atom->parse(file);
			children.push_back(atom);
		}
		assert(file.pos() == start + length);

	} else {
		//skip reading mdat content, it's not useful for analysis and potentially too big
		int64_t content_size = length -(length64? 16 : 8);
		if(name == string("mdat")) {
			file.seek(file.pos() + content_size);
			return;
		}

		content = file.read(content_size); //length includes header
		if(content.size() < content_size)
			throw string("Failed reading atom content: ") + name;
	}
}

void Atom::write(File &file) {
	//1 write length
#ifndef NDEBUG
	off_t begin = file.pos();
#endif

	if(length64) {
		file.writeInt(1);
		file.writeChar(name, 4);
		file.writeInt64(length);
	} else {
		file.writeInt(length);
		file.writeChar(name, 4);
	}

	if(!content.empty())
		file.write(content);
	for(unsigned int i = 0; i < children.size(); i++)
		children[i]->write(file);

#ifndef NDEBUG
	off_t end = file.pos();
	assert(end - begin == length);
#endif
}

int readBits(int n, uint8_t *&buffer, int &offset) {
	assert(buffer != NULL && offset >= 0);
	int res = 0;
	// Can't read in a single reading.
	while(n + offset > 8) {
		int d = 8 - offset;
		res <<= d;
		res |= *buffer & ((1 << d) - 1);
		offset = 0;
		buffer++;
		n -= d;
	}
	// Read the remaining bits.
	int d = (8 - offset - n);
	res <<= n;
	res |= (*buffer >> d) & ((1 << n) - 1);
	return res;
}

void Atom::print(int offset) {
	string indent(offset, ' ');

	Log::info << string(offset, '-') << name << " [" << start << ", " << length << "]\n";
	if(name == string("mvhd") || name == string("mdhd")) {
		//timescale: time units per second
		//duration: in time units
		Log::info << indent << " Timescale: " << readInt(12) << " Duration: " << readInt(16) << '\n';

	} else if(name == string("tkhd")) {
		//track id:
		//duration:
		Log::info << indent << " Trak: " << readInt(12) << " Duration: "  << readInt(20) << '\n';

	} else if(name == string("hdlr")) {
		char type[5];
		readChar(type, 8, 4);
		Log::info << indent << " Type: " << type << '\n';

	} else if(name == string("dref")) {
		Log::info << indent << " Entries: " << readInt(4) << '\n';

	} else if(name == string("stsd")) { //sample description: (which codec...)
		//lets just read the first entry
		int nentries = readInt(4);
		int off = 8;

		for(int i = 0; i < nentries; i++) {
			int bytes = readInt(off + 0);
			char type[5];
			readChar(type, 12, off + 4);

			//TODO this actually depends on trak being soun or vide! Might be simpler to use the list here:
			//https://developer.apple.com/library/archive/documentation/QuickTime/QTFF/QTFFChap3/qtff3.html#//apple_ref/doc/uid/TP40000939-CH205-75770
			if(type == string("mp4a")) {
				int nchannels = readInt16(off + 24);
				int samplesize = readInt16(off + 26);
				int comprid = readInt16(off + 28);
				int packetsize = readInt16(off + 30);
				uint16_t sampleRate = readInt16(off + 32); //16.16 fixed point integer.
				Log::info << indent << "Media Sample Type: " << type << " N. channels: " << nchannels << " Sample size: " << samplesize
						  << " Compression ID: " << comprid << " Packet size: " << packetsize
						  << " Sample Rate: " << sampleRate << endl;

			} else if(type == string("avc1")) {
				int width = readInt16(off + 32);
				int height = readInt16(off + 34);
				int hres = readInt(off + 36);
				int vres = readInt(off + 40);
				int framecount = readInt16(off+48);
				char compressor[33];
				readChar(compressor, off+50, 32);
				int depth = readInt16(off + 82);
				int colorMap = readInt16(off + 84);
				int extensionSize = readInt(off + 86);

				Log::info << indent << "Media Sample Type: " << type << " w: " << width << " h: " << height << " compressor: " << (compressor+1) << " depth: " << depth << endl;
				char extension[5] = { 0, 0, 0, 0, 0};
				readChar(extension, off+90, 4);

				if(extension == string("avcC")) {
					Log::info << indent << "Extension: " << extension << endl;
					int eoff = 94;
					uint8_t sbuffer[extensionSize+1];
					readChar((char *)sbuffer, eoff, extensionSize);

					uint8_t *buffer = sbuffer;
					int boff= 0; //offset in bits into extension
					//buffer and boff are updated while reading bits.
					int version               = readBits(8, buffer, boff);
					int profile_indication    = readBits(8, buffer, boff);
					int profile_compatibility = readBits(8, buffer, boff);
					int level_code            = readBits(8, buffer, boff);
					int reserved              = readBits(6, buffer, boff);
					int lengthSizeMinusOne    = readBits(2, buffer, boff);
					reserved                   = readBits(3, buffer, boff);

					int numOfSeqParameterSets = readBits(5, buffer, boff);
					std::vector<uint8_t> sequenceParameterSetNALUnit;
					for (int i=0; i< numOfSeqParameterSets; i++) {
						int sequenceParameterSetLength = readBits(16, buffer, boff);
						for(int k = 0; k < sequenceParameterSetLength; k++) {
							sequenceParameterSetNALUnit.push_back(readBits(8, buffer, boff));
						}
					}
					int numOfPictureParameterSets = readBits(8, buffer, boff);
					std::vector<uint8_t> pictureParameterSetNALUnit;
					for (int i=0; i< numOfPictureParameterSets; i++) {
						int pictureParameterSetLength = readBits(16, buffer, boff);
						for(int k = 0; k < pictureParameterSetLength; k++) {
							pictureParameterSetNALUnit.push_back(readBits(8, buffer, boff));
						}
					}
					Log::info << indent << "Profile " << profile_indication << " SPS: " << hex;
					for(uint8_t s: sequenceParameterSetNALUnit)
						Log::info << (int)s << " ";
					Log::info << " PPS: ";
					for(uint8_t s: pictureParameterSetNALUnit)
						Log::info << (int)s << " ";
					Log::info << dec << endl;

					//might want to check for profile in sequenceParameterSet, or just use the length of the buffer.
					/*
				if( profile_idc == 100 || profile_idc == 110 ||
				profile_idc == 122 || profile_idc == 144 )
				{
				bit(6) reserved = ‘111111’b;
				unsigned int(2) chroma_format;
				bit(5) reserved = ‘11111’b;
				unsigned int(3) bit_depth_luma_minus8;
				bit(5) reserved = ‘11111’b;
				unsigned int(3) bit_depth_chroma_minus8;
				unsigned int(8) numOfSequenceParameterSetExt;
				for (i=0; i< numOfSequenceParameterSetExt; i++) {
				unsigned int(16) sequenceParameterSetExtLength;
				bit(8*sequenceParameterSetExtLength) sequenceParameterSetExtNALUnit;
				}
				} */



				} else {
					Log::info << indent << "Extension: " << extension << endl;
				}
			}
			off += bytes;

		}
		//4 bytes zero
		//4 bytes reference index (see stsc)
		//additional fields
		//video:
		//4 bytes zero
		///avcC: //see ISO 14496  5.2.4.1.1.
		//01 -> version
		//4d -> profile
		//00 -> compatibility
		//28 -> level code
		//ff ->  6 bit reserved as 1  + 2 bit as nal length -1  so this is 4.
		//E1 -> 3 bit as 1 + 5 for SPS (so 1)
		//00 09 -> length of sequence parameter set
		//27 4D 00 28 F4 02 80 2D C8  -> sequence parameter set
		//01 -> number of picture parameter set
		//00 04 -> length of picture parameter set
		//28 EE 16 20 -> picture parameter set. (28 ee 04 62),  (28 ee 1e 20)

		//Log::info << indent << " Entries: " << readInt(4) << " codec: " << type << '\n';

	} else if(name == string("stts")) { //run length compressed duration of samples
		//lets just read the first entry
		int entries = readInt(4);
		cout << indent << " Entries: " << entries << '\n';
		for(int i = 0; i < entries && i < 30; i++)
			Log::info << indent << " samples: " << readInt(8 + 8*i) << " for: " << readInt(12 + 8*i) << '\n';

	} else if(name == string("stss")) { //sync sample: (keyframes)
		//lets just read the first entry
		int entries = readInt(4);
		Log::info << indent << " Entries: " << entries << '\n';
		for(int i = 0; i < entries && i < 10; i++)
			Log::info << indent << " Keyframe: " << readInt(8 + 4*i) << '\n';


	} else if(name == string("stsc")) { //samples to chucnk:
		//lets just read the first entry
		int entries = readInt(4);
		Log::info << indent << " Entries: " << entries << '\n';
		for(int i = 0; i < entries && i < 10; i++) {
			Log::info << indent  << " chunk: "     << readInt( 8 + 12*i)
					  << " nsamples: "  << readInt(12 + 12*i)
					  << " id: "        << readInt(16 + 12*i)
					  << '\n';
		}

	} else if(name == string("stsz")) { //sample size atoms
		int entries = readInt(8);
		int sample_size = readInt(4);
		Log::info << indent << " Sample size: " << sample_size << " Entries: " << entries << '\n';
		if(sample_size == 0) {
			for(int i = 0; i < entries && i < 10; i++)
				Log::info << indent << " Size " << readInt(12 + i*4) << '\n';
		}

	} else if(name == string("stco")) { //sample chunk offset atoms
		int entries = readInt(4);
		Log::info << indent << " Entries: " << entries << '\n';
		for(int i = 0; i < entries && i < 10; i++)
			Log::info << indent << " chunk: " << readInt(8 + i*4) << '\n';

	} else if(name == string("co64")) {
		int entries = readInt(4);
		Log::info << indent << " Entries: " << entries << '\n';
		for(int i = 0; i < entries && i < 10; i++)
			Log::info << indent << " chunk: " << readInt(12 + i*8) << '\n';

	}

	for(unsigned int i = 0; i < children.size(); i++)
		children[i]->print(offset+1);

	Log::flush();
}


bool Atom::isParent(const char *id) {
	AtomDefinition def = definition(id);
	return def.container_state == PARENT_ATOM;// || def.container_state == DUAL_STATE_ATOM;
}

bool Atom::isDual(const char *id) {
	AtomDefinition def = definition(id);
	return def.container_state == DUAL_STATE_ATOM;
}

bool Atom::isVersioned(const char *id) {
	AtomDefinition def = definition(id);
	return def.box_type == VERSIONED_ATOM;
}


vector<Atom *> Atom::atomsByName(string name) const {
	vector<Atom *> atoms;
	for(unsigned int i = 0; i < children.size(); i++) {
		if(children[i]->name == name)
			atoms.push_back(children[i]);
		vector<Atom *> a = children[i]->atomsByName(name);
		atoms.insert(atoms.end(), a.begin(), a.end());
	}
	return atoms;
}

Atom *Atom::atomByName(string name) const {
	for(unsigned int i = 0; i < children.size(); i++) {
		if(children[i]->name == name)
			return children[i];
		Atom *a = children[i]->atomByName(name);
		if(a) return a;
	}
	return NULL;
}

void Atom::replace(Atom *original, Atom *replacement) {
	for(unsigned int i = 0; i < children.size(); i++) {
		if(children[i] == original) {
			children[i] = replacement;
			return;
		}
	}
	throw string("Atom not found");
}


void Atom::prune(string name) {
	if(children.empty()) return;

	length = 8;

	vector<Atom *>::iterator it = children.begin();
	while(it != children.end()) {
		Atom *child = *it;
		if(name == child->name) {
			delete child;
			it = children.erase(it);
		} else {
			child->prune(name);
			length += child->length;
			++it;
		}
	}
}

void Atom::updateLength() {
	length = 8;
	length += content.size();
	if(length >= 1L<<32) {
		length64 = true;
		length += 8;
	}

	for(unsigned int i = 0; i < children.size(); i++) {
		Atom *child = children[i];
		child->updateLength();
		length += child->length;
	}
}


void Atom::contentResize(size_t newsize) {
	content.resize(newsize);
}

uint8_t Atom::readUInt8(int64_t offset) {
	return uint8_t(content[offset]);
}

int16_t Atom::readInt16(int64_t offset) {
	assert(offset >= 0 && content.size() >= uint64_t(offset) + 2);
	return readBE<int16_t>(&content[offset]);

}

int32_t Atom::readInt(int64_t offset) {
	assert(offset >= 0 && content.size() >= uint64_t(offset) + 4);
	return readBE<int32_t>(&content[offset]);
}


uint32_t Atom::readUInt(int64_t offset) {
	assert(offset >= 0 && content.size() >= uint64_t(offset) + 4);
	return readBE<uint32_t>(&content[offset]);
}

int64_t Atom::readInt64(int64_t offset) {
	assert(offset >= 0 && content.size() >= uint64_t(offset) + 8);
	return readBE<int64_t>(&content[offset]);
}

void Atom::writeInt(int32_t value, int64_t offset) {
	assert(offset >= 0 && content.size() >= uint64_t(offset) + 4);
	writeBE(&content[offset], value);
}

void Atom::writeInt64(int64_t value, int64_t offset) {
	assert(offset >= 0 && content.size() >= uint64_t(offset) + 8);
	writeBE(&content[offset], value);
}


uint8_t *Atom::data(uint8_t *str, int64_t offset, int64_t length) {
	assert(str != NULL);
	assert(offset >= 0 && length >= 0 && content.size() >= uint64_t(offset) + uint64_t(length));
	return &content[offset];
}



void Atom::readChar(char *str, int64_t offset, int64_t length) {
	assert(str != NULL);
	assert(offset >= 0 && length >= 0 && content.size() >= uint64_t(offset) + uint64_t(length));
	const unsigned char *p = &content[offset];
	for(long int i = 0; i < length; i++)
		*str++ = *p++;
	*str = '\0';
}



// BufferedAtom
BufferedAtom::BufferedAtom(string filename)
	: file_begin(0),
	  file_end(0),
	  buffer(NULL),
	  buffer_begin(0),
	  buffer_end(0)
{
	if(!file.open(filename))
		throw string("Could not open file");
	file_end = file.length();
}

BufferedAtom::~BufferedAtom() {
	delete[] buffer;
}


unsigned char *BufferedAtom::getFragment(int64_t offset, int64_t size) {
	assert(size >= 0);
	if(offset < 0)
		throw string("Offset set before beginning of buffer");
	if(offset >= file_end)
		throw string("Out of buffer");
	if(offset + size > file_end - file_begin)
		size = file_end - offset;

	if(buffer) {
		if(buffer_begin <= offset && buffer_end >= offset + size)
			return buffer + (offset - buffer_begin);

		//reallocate and reread
		flush();
	}

	buffer_begin = offset;
	buffer_end   = offset + 2 * size;
	if(buffer_end + file_begin > file_end)
		buffer_end = file_end - file_begin;
	buffer = new unsigned char[buffer_end - buffer_begin];
	file.seek(file_begin + buffer_begin);
	file.readChar((char *)buffer, buffer_end - buffer_begin);
	return buffer;
}
void BufferedAtom::flush() {
	if(buffer) {
		delete []buffer;
		buffer = nullptr;
	}
	buffer_begin = buffer_end = 0;
}

void BufferedAtom::updateLength() {
	length  = 8;
	length += file_end - file_begin;
	if(length >= 1L<<32) {
		length64 = true;
		length += 8;
	}

	for(unsigned int i = 0; i < children.size(); i++) {
		Atom *child = children[i];
		child->updateLength();
		length += child->length;
	}
}


void BufferedAtom::contentResize(size_t newsize) {
	if(newsize > file_end - file_begin)
		throw string("Cannot resize buffered atom");
}


int32_t BufferedAtom::readInt(int64_t offset) {
	if(!buffer || offset < buffer_begin || offset > (buffer_end - 4)) {
		buffer = getFragment(offset, 1<<16);
	}
	return readNE<int32_t>(buffer + offset - buffer_begin);
}

int64_t BufferedAtom::readInt64(int64_t offset) {
	if(!buffer || offset < buffer_begin || offset > (buffer_end - 8)) {
		buffer = getFragment(offset, 1<<16);
	}
	return readNE<int64_t>(buffer + offset - buffer_begin);
}


void BufferedAtom::write(File &output) {
	//1 write length
#ifndef NDEBUG
	off_t begin = output.pos();
#endif

	if(length64) {
		output.writeInt(1);
		output.writeChar(name, 4);
		output.writeInt64(length);
	} else {
		output.writeInt(length);
		output.writeChar(name, 4);
	}

	char buff[1<<20];
	int64_t offset = file_begin;
	file.seek(file_begin);
	while(offset < file_end) {
		int64_t toread = 1<<20;
		if(toread + offset > file_end)
			toread = file_end - offset;
		file.readChar(buff, toread);
		offset += toread;
		output.writeChar(buff, toread);
	}
	for(unsigned int i = 0; i < children.size(); i++)
		children[i]->write(output);

#ifndef NDEBUG
	off_t end = output.pos();
	assert(end - begin == length);
#endif
}

