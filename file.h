//==================================================================//
/*
	Untrunc - file.h

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

#ifndef FILE_H
#define FILE_H

#include <vector>
#include <string>
extern "C" {
#include <stdint.h>
}
#include <cstdio>


// Swap the 8-bit bytes into their reverse order.
uint16_t swap16(uint16_t us);
uint32_t swap32(uint32_t ui);
uint64_t swap64(uint64_t ull);


// Encapsulate FILE (RAII).
class File {
public:
	File();
	~File();

	bool open  (std::string filename);
	bool create(std::string filename);

	operator bool() { return static_cast<bool>(file); }

	off_t pos();
	void  seek(off_t offset);
	void  rewind();
	bool  atEnd();
	off_t size();
	off_t length() { return size(); }

	uint32_t readInt();
	uint64_t readInt64();
	void    readChar(char *dest, size_t n);
	std::vector<unsigned char> read(size_t n);

	ssize_t writeInt  (int32_t value);
	ssize_t writeInt64(int64_t value);
	ssize_t writeChar (const char *source, size_t n);
	ssize_t write(std::vector<unsigned char> &v);

protected:
	std::FILE *file;
	off_t file_sz;

	void close();

private:
	// Disable copying.
	File(const File&);
	File& operator=(const File&);
};

#endif // FILE_H
