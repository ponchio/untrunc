//==================================================================//
/*
	Untrunc - file.cpp

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

#include "file.h"

#include <vector>
#include <string>
#include <cstdio>
#include <cassert>

using namespace std;


// Swap the 8-bit bytes into their reverse order.
uint16_t swap16(uint16_t us) {
	return ((us >> 8) | (us << 8));
}

uint32_t swap32(uint32_t ui) {
	return ((ui << 24) | ((ui << 8) & 0x00FF0000) | ((ui >> 8) & 0x0000FF00) | (ui >> 24));
}

uint64_t swap64(uint64_t ull) {
	return
		(  (ull << 56)
		| ((ull << 40) & 0x00FF000000000000)
		| ((ull << 24) & 0x0000FF0000000000)
		| ((ull <<  8) & 0x000000FF00000000)
		| ((ull >>  8) & 0x00000000FF000000)
		| ((ull >> 24) & 0x0000000000FF0000)
		| ((ull >> 40) & 0x000000000000FF00)
		|  (ull >> 56) );
}



// Update file_sz on every write to the file.
#define FILE_SIZE_UPDATE_ON_WRITE   1
// Seek from end-of-file when seeking to a negative offset.
//#define FILE_SEEK_FROM_END          1


// Encapsulate FILE (RAII).
File::File() : file(NULL), file_sz(-1) { }

File::~File() {
	close();
}


bool File::open(string filename) {
	close();

	if(filename.empty())
		return false;
	file = fopen(filename.c_str(), "rb");
	if(!file)
		return false;

	fseeko(file, 0L, SEEK_END);
	off_t sz = ftello(file);
	fseeko(file, 0L, SEEK_SET);
	if(sz < 0)
		return false;
	file_sz = sz;
	return true;
}

bool File::create(string filename) {
	close();

	if(filename.empty())
		return false;
	file = fopen(filename.c_str(), "wb");
	if(!file)
		return false;

#ifdef FILE_SIZE_UPDATE_ON_WRITE
	file_sz = 0;
#endif
	return true;
}

void File::close() {
	if(file) {
		FILE *rm_file = file;
		file = NULL;
		fclose(rm_file);
	}
	file_sz = -1;
}


off_t File::pos() {
	return (file) ? ftello(file) : off_t(-1);
}

void File::seek(off_t offset) {
#ifdef FILE_SEEK_FROM_END
	if(file)
		fseeko(file, offset, (offset >= 0) ? SEEK_SET : SEEK_END);
#else
	assert(offset >= 0);
	if(file && offset >= 0)
		fseeko(file, offset, SEEK_SET);
#endif
}

void File::rewind() {
	if(file) {
		fseeko(file, 0L, SEEK_SET);
		clearerr(file);
	}
}

bool File::atEnd() {
	if(!file)
		return true;
	off_t pos = ftello(file);
	if(pos < 0)
		return true;
	off_t sz  = file_sz;
#ifndef FILE_SIZE_UPDATE_ON_WRITE
	if(sz < 0) {
		fseeko(file, 0L,  SEEK_END);
		sz = ftello(file);
		fseeko(file, pos, SEEK_SET);
	}
#endif
	return (pos >= sz);
}

off_t File::size() {
#ifdef FILE_SIZE_UPDATE_ON_WRITE
	return file_sz;
#else
	if(file_sz >= 0)
		return file_sz;
	if(!file)
		return -1;
	off_t pos = ftello(file);
	if(pos < 0)
		return -1;
	fseeko(file, 0L,  SEEK_END);
	off_t sz  = ftello(file);
	fseeko(file, pos, SEEK_SET);
	if(sz < 0)
		return -1;
	return sz;
#endif
}


int32_t File::readInt() {
	uint32_t value = 0;
	size_t n = (file) ? fread(&value, sizeof(value), 1, file) : 0;
	if(n != 1)
		throw string("Could not read atom length");

	// Read a 32-bit big-endian value.
	// A compiler will optimize this to a single instruction if possible.
	const uint8_t *p = reinterpret_cast<const uint8_t*>(&value);
	return ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]));
}

int64_t File::readInt64() {
	uint64_t value = 0;
	size_t n = (file) ? fread(&value, sizeof(value), 1, file) : 0;
	if(n != 1)
		throw string("Could not read atom length");

	// Read a 64-bit big-endian value.
	// A compiler will optimize this to a single instruction if possible.
	const uint8_t *p = reinterpret_cast<const uint8_t*>(&value);
	return ( (uint64_t(p[0]) << 56) | (uint64_t(p[1]) << 48) | (uint64_t(p[2]) << 40) | (uint64_t(p[3]) << 32)
	       | (uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) | (uint64_t(p[6]) <<  8) |  uint64_t(p[7]) );
}

void File::readChar(char *dest, size_t n) {
	assert(dest != NULL || n == 0);
	if(n > 0) {
		size_t len = fread(dest, sizeof(char), n, file);
		if(len != n)
			throw string("Could not read chars");
	}
}

vector<unsigned char> File::read(size_t n) {
	vector<unsigned char> dest(n);
	if(n > 0) {
		size_t len = fread(&dest[0], sizeof(unsigned char), n, file);
		if(len != n)
			throw string("Could not read at position");
	}
	return dest;
}


ssize_t File::writeInt(int32_t value) {
	if(!file)
		return -1;

	// Write a 32-bit big-endian value.
	// A compiler can optimize the endian conversion to 1 or 2 instructions if possible.
	uint32_t val32 = value;
	uint8_t  data[4] = {
		static_cast<uint8_t>(val32 >> 24),
		static_cast<uint8_t>(val32 >> 16),
		static_cast<uint8_t>(val32 >>  8),
		static_cast<uint8_t>(val32)
	};

	size_t len = fwrite(&data, sizeof(data), 1, file);
	if(len == 0)
		return (ferror(file)) ? -1 : 0;
#ifdef FILE_SIZE_UPDATE_ON_WRITE
	off_t  pos = ftello(file);
	if(file_sz < pos)
		file_sz = pos;
#endif
	return len;
}

ssize_t File::writeInt64(int64_t value) {
	if(!file)
		return -1;

	// Write a 64-bit big-endian value.
	// A compiler can optimize the endian conversion to 1 or 2 instructions if possible.
	uint64_t val64 = value;
	uint8_t  data[8] = {
		static_cast<uint8_t>(val64 >> 56),
		static_cast<uint8_t>(val64 >> 48),
		static_cast<uint8_t>(val64 >> 40),
		static_cast<uint8_t>(val64 >> 32),
		static_cast<uint8_t>(val64 >> 24),
		static_cast<uint8_t>(val64 >> 16),
		static_cast<uint8_t>(val64 >>  8),
		static_cast<uint8_t>(val64)
	};

	size_t len = fwrite(&data, sizeof(data), 1, file);
	if(len == 0)
		return (ferror(file)) ? -1 : 0;
#ifdef FILE_SIZE_UPDATE_ON_WRITE
	off_t  pos = ftello(file);
	if(file_sz < pos)
		file_sz = pos;
#endif
	return len;
}

ssize_t File::writeChar(const char *source, size_t n) {
	assert(source != NULL || n == 0);
	if(n == 0)
		return  0;
	if(!file)
		return -1;

	size_t len = fwrite(source, sizeof(char), n, file);
	if(len == 0)
		return (ferror(file)) ? -1 : 0;
#ifdef FILE_SIZE_UPDATE_ON_WRITE
	off_t  pos = ftello(file);
	if(file_sz < pos)
		file_sz = pos;
#endif
	return len;
}

ssize_t File::write(vector<unsigned char> &v) {
	if(v.empty())
		return  0;
	if(!file)
		return -1;

	size_t len = fwrite(&v[0], sizeof(unsigned char), v.size(), file);
	if(len == 0)
		return (ferror(file)) ? -1 : 0;
#ifdef FILE_SIZE_UPDATE_ON_WRITE
	off_t  pos = ftello(file);
	if(file_sz < pos)
		file_sz = pos;
#endif
	return len;
}

