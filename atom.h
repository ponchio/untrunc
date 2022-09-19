//==================================================================//
/*
    Untrunc - atom.h

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

#ifndef ATOM_H
#define ATOM_H

extern "C" {
#include <stdint.h>
}
#include <vector>
#include <string>

#include "file.h"


class Atom {
public:
	int64_t start = 0;       //including 8 header bytes
	int64_t content_start = 0; //after the name and eventually the extended 64 bit size;
    int64_t length = 0;      //including 8 header bytes
    char    name[5] = "";
    char    head[4] = "";
    char    version[4] = "";
    std::vector<unsigned char> content;
    std::vector<Atom *> children;
	bool length64 = false;

	Atom() {};
    virtual ~Atom();

    void parseHeader  (File &file); //read just name and length
    void parse        (File &file);
    virtual void write(File &file);
    void print(int offset);

    std::vector<Atom *> atomsByName(std::string name) const;
    Atom *              atomByName (std::string name) const;
    void replace(Atom *original, Atom *replacement);

    void prune(std::string name);
    virtual void updateLength();

    virtual int64_t contentSize() const { return content.size();   }
    virtual void    contentResize(size_t newsize); //unused!

    static bool isParent   (const char *id);
    static bool isDual     (const char *id);
    static bool isVersioned(const char *id);

	virtual uint8_t readUInt8(int64_t offset);
	virtual int16_t readInt16(int64_t offset);
    virtual int32_t readInt  (int64_t offset);
	virtual uint32_t readUInt  (int64_t offset);

    virtual int64_t readInt64(int64_t offset);
    void writeInt  (int32_t value, int64_t offset);
    void writeInt64(int64_t value, int64_t offset);
	uint8_t *data(uint8_t *str, int64_t offset, int64_t length);
	void readChar(char *str, int64_t offset, int64_t length);

private:
    // Disable copying (BufferedAtom can't be copied, so children can't either).
    Atom(const Atom&);
    Atom& operator=(const Atom&);
};


class BufferedAtom : public Atom {
public:
    int64_t file_begin;
    int64_t file_end;
	File    file; //don't touch!

    explicit BufferedAtom(std::string filename);
    ~BufferedAtom();

    virtual void write(File &file);

    unsigned char *getFragment(int64_t offset, int64_t size);
	void flush();
    virtual void updateLength();

    virtual int64_t contentSize() const { return file_end - file_begin; }
    virtual void    contentResize(size_t newsize);   //can't actually resize!

	virtual uint8_t readUInt8(int64_t offset) { throw "unimplementd"; }
	virtual int16_t readInt16(int64_t offset) { throw "unimplementd"; }

    virtual int32_t readInt  (int64_t offset);
    virtual int64_t readInt64(int64_t offset);

protected:
	unsigned char  *buffer;
    int64_t         buffer_begin;
    int64_t         buffer_end;

private:
    // Disable copying (File can't be copied).
    BufferedAtom(const BufferedAtom&);
    BufferedAtom& operator=(const BufferedAtom&);
};

#endif // ATOM_H
