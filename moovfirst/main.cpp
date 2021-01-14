#include "../atom.h"
#include "../file.h"

#include <iostream>
#include <deque>

using namespace std;

void writeAtom(Atom *atom, File &input, File &output) {
	int64_t size = 10 * (1<<20);
	char *buffer = new char[size];
	int64_t start = atom->start;
	int64_t end = start + atom->length;
	input.seek(atom->start);
	while(start < end) {
		int64_t s = std::min(end - start, size);
		input.readChar(buffer, s);
		output.writeChar(buffer, s);
		start += size;
	}
	delete []buffer;
}

void makeStreamable(string filename, string output_filename) {
	Atom atom_root;

	File input;
	if(!input.open(filename))
		throw "Could not open file: " + filename;

	while(!input.atEnd()) {
		Atom *atom = new Atom;
		atom->parse(input);
		atom_root.children.push_back(atom);
	}

	Atom *ftyp = atom_root.atomByName("ftyp");

	Atom *moov = atom_root.atomByName("moov");
	if(!moov)
		throw string("Missing 'Container for all the Meta-data' atom (moov).");

	Atom *mdat = atom_root.atomByName("mdat");
	if(!mdat)
		throw string("Missing 'Media Data container' atom (mdat).");


	if(mdat->start > moov->start)
		throw string("File is already streamable.");

	int64_t old_start = mdat->start;
	int64_t new_start = moov->length;
	if(ftyp)
		new_start += ftyp->length;

	int64_t diff = new_start - old_start;

	std::vector<Atom *> stcos = moov->atomsByName("stco");
	for(Atom *stco: stcos) {
		int32_t nchunks = stco->readInt(4); // 4 version, 4 number of entries, 4 entries.
		for(int j = 0; j < nchunks; ++j) {
			int64_t pos = int64_t(8) + 4*j;
			int64_t offset = stco->readInt(pos) + diff;
			stco->writeInt(offset, pos);
		}
	}

	std::vector<Atom *> cos64 = moov->atomsByName("co64");
	for(Atom *stco: cos64) {
		int32_t nchunks = stco->readInt(4); // 4 version, 4 number of entries, 4 entries.
		for(int j = 0; j < nchunks; ++j) {
			int64_t pos    = int64_t(8) + 8*j;
			int64_t offset = stco->readInt(pos) + diff;
			stco->writeInt(offset, pos);
		}
	}

	File output;
	if(!output.create(output_filename))
		throw "Could not create file for writing: " + output_filename;

	if(ftyp)
		ftyp->write(output);
	moov->write(output);
	writeAtom(mdat, input, output);
}


int main(int argc, char *argv[]) {

	if(argc != 3) {
		cerr << "Usage: " << argv[0] << " <input.mp4> <output.mp4>" << endl;
		return 0;
	}
	if(string(argv[1]) == string(argv[2])) {
		cerr << "Cannot overwrite the original file!" << endl;
		return 0;
	}

	try {
		makeStreamable(argv[1], argv[2]);
	} catch(const string &error) {
		cerr << error << endl;
	}
	return 1;
}
