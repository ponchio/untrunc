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

#include <map>
#include <iostream>
#include <cstring>      //for: memcpy()
#include <cassert>

using namespace std;


namespace {
    static inline uint32_t id2Key(const char *id) {
        const unsigned char *uid = reinterpret_cast<const unsigned char*>(id);
        return ((uint32_t(uid[0]) << 24) | (uint32_t(uid[1]) << 16) | (uint32_t(uid[2]) << 8) | uint32_t(uid[3]));
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


// Atom
Atom::Atom() : start(0), length(0), name(""), head(""), version("") { }

Atom::~Atom() {
    for(unsigned int i = 0; i < children.size(); i++)
        delete children[i];
}

void Atom::parseHeader(File &file) {
    start = file.pos();
    length = file.readInt();
    file.readChar(name, 4);

    if(length == 1) {
        length = file.readInt64() - 8;
        start += 8;
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
        content = file.read(length -8); //length includes header
        if(content.size() < length -8)
            throw string("Failed reading atom content: ") + name;
    }
}

void Atom::write(File &file) {
    //1 write length
#ifndef NDEBUG
    off_t begin = file.pos();
#endif

    file.writeInt(length);
    file.writeChar(name, 4);
    if(!content.empty())
        file.write(content);
    for(unsigned int i = 0; i < children.size(); i++)
        children[i]->write(file);

#ifndef NDEBUG
    off_t end = file.pos();
    assert(end - begin == length);
#endif
}

void Atom::print(int offset) {
    string indent(offset, ' ');

    cout << string(offset, '-') << name << " [" << start << ", " << length << "]\n";
    if(name == string("mvhd") || name == string("mdhd")) {
        //timescale: time units per second
        //duration: in time units
        cout << indent << " Timescale: " << readInt(12) << " Duration: " << readInt(16) << '\n';

    } else if(name == string("tkhd")) {
        //track id:
        //duration:
        cout << indent << " Trak: " << readInt(12) << " Duration: "  << readInt(20) << '\n';

    } else if(name == string("hdlr")) {
        char type[5];
        readChar(type, 8, 4);
        cout << indent << " Type: " << type << '\n';

    } else if(name == string("dref")) {
        cout << indent << " Entries: " << readInt(4) << '\n';

    } else if(name == string("stsd")) { //sample description: (which codec...)
        //lets just read the first entry
        char type[5];
        readChar(type, 12, 4);
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

        cout << indent << " Entries: " << readInt(4) << " codec: " << type << '\n';

    } else if(name == string("stts")) { //run length compressed duration of samples
        //lets just read the first entry
        int entries = readInt(4);
        cout << indent << " Entries: " << entries << '\n';
        for(int i = 0; i < entries && i < 30; i++)
            cout << indent << " samples: " << readInt(8 + 8*i) << " for: " << readInt(12 + 8*i) << '\n';

    } else if(name == string("stss")) { //sync sample: (keyframes)
        //lets just read the first entry
        int entries = readInt(4);
        cout << indent << " Entries: " << entries << '\n';
        for(int i = 0; i < entries && i < 10; i++)
            cout << indent << " Keyframe: " << readInt(8 + 4*i) << '\n';


    } else if(name == string("stsc")) { //samples to chucnk:
        //lets just read the first entry
        int entries = readInt(4);
        cout << indent << " Entries: " << entries << '\n';
        for(int i = 0; i < entries && i < 10; i++) {
            cout << indent  << " chunk: "     << readInt( 8 + 12*i)
                            << " nsamples: "  << readInt(12 + 12*i)
                            << " id: "        << readInt(16 + 12*i)
                            << '\n';
        }

    } else if(name == string("stsz")) { //sample size atoms
        int entries = readInt(8);
        int sample_size = readInt(4);
        cout << indent << " Sample size: " << sample_size << " Entries: " << entries << '\n';
        if(sample_size == 0) {
            for(int i = 0; i < entries && i < 10; i++)
                cout << indent << " Size " << readInt(12 + i*4) << '\n';
        }

    } else if(name == string("stco")) { //sample chunk offset atoms
        int entries = readInt(4);
        cout << indent << " Entries: " << entries << '\n';
        for(int i = 0; i < entries && i < 10; i++)
            cout << indent << " chunk: " << readInt(8 + i*4) << '\n';

    } else if(name == string("co64")) {
        int entries = readInt(4);
        cout << indent << " Entries: " << entries << '\n';
        for(int i = 0; i < entries && i < 10; i++)
            cout << indent << " chunk: " << readInt(12 + i*8) << '\n';

    }

    for(unsigned int i = 0; i < children.size(); i++)
        children[i]->print(offset+1);

    cout.flush();
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

    for(unsigned int i = 0; i < children.size(); i++) {
        Atom *child = children[i];
        child->updateLength();
        length += child->length;
    }
}

int32_t Atom::readInt(int64_t offset) {
    assert(offset >= 0 && content.size() >= uint64_t(offset) + 4);
    return swap32(*reinterpret_cast<uint32_t *>(&content[offset]));
    // Read  an unaligned, 32-bit big-endian value.
    // A compiler will optimize this to a single instruction if possible.
    const uint8_t *p = &content[offset];
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void Atom::writeInt(int32_t value, int64_t offset) {
    assert(offset >= 0 && content.size() >= uint64_t(offset) + 4);
    *reinterpret_cast<uint32_t *>(&content[offset]) = swap32(value);
    // Write an unaligned, 32-bit big-endian value.
    uint32_t val32 = value;
    uint8_t *p     = &content[offset];
    p[0] = static_cast<uint8_t>(val32 >> 24);
    p[1] = static_cast<uint8_t>(val32 >> 16);
    p[2] = static_cast<uint8_t>(val32 >>  8);
    p[3] = static_cast<uint8_t>(val32);
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
}

BufferedAtom::~BufferedAtom() {
    delete[] buffer;
}

unsigned char *BufferedAtom::getFragment(int64_t offset, int64_t size) {
    if(offset < 0)
        throw string("Offset set before beginning of buffer");
    if(offset + size > file_end - file_begin)
        throw string("Out of buffer");

    if(buffer) {
        if(buffer_begin >= offset && buffer_end >= offset + size)
            return buffer + (offset - buffer_begin);

        //reallocate and reread
        delete[] buffer;
        buffer = NULL;
    }

    buffer_begin = offset;
    buffer_end = offset + 2*size;
    if(buffer_end + file_begin > file_end)
        buffer_end = file_end - file_begin;
    buffer = new unsigned char[buffer_end - buffer_begin];
    file.seek(file_begin + buffer_begin);
    file.readChar((char *)buffer, buffer_end - buffer_begin);
    return buffer;
}

void BufferedAtom::updateLength() {
    length = 8;
    length += file_end - file_begin;

    for(unsigned int i = 0; i < children.size(); i++) {
        Atom *child = children[i];
        child->updateLength();
        length += child->length;
    }
}

int32_t BufferedAtom::readInt(int64_t offset) {
    if(!buffer || offset < buffer_begin || offset > (buffer_end - 4)) {
        buffer = getFragment(offset, 1<<16);
    }
    // Read an unaligned, 32-bit value.
    // Encode the unaligned access intention by using memcpy() with its
    //  destination and source pointing to types with the wanted alignment.
    // Some compilers use the alignments of these types for further optimizations.
    // A compiler can optimize this memcpy() into a single instruction.
    const unsigned char *p = buffer + offset - buffer_begin;
    uint32_t val32;
    memcpy(&val32, p, sizeof(val32));
    return val32;
}

void BufferedAtom::write(File &output) {
    //1 write length
#ifndef NDEBUG
    off_t begin = output.pos();
#endif

    output.writeInt(length);
    output.writeChar(name, 4);
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

