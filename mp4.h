//==================================================================//
/*
    Untrunc - mp4.h

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

#ifndef MP4_H
#define MP4_H

#include <vector>
#include <string>

#include "track.h"
class File;


class Atom;
class BufferedAtom;
struct AVFormatContext;


class Mp4 {
public:
    int timescale;
    int duration;
	enum MdatStrategy {
		FIRST,  //first mdat in file
		SAME,   //same offset as the working sample (sometimes the start is fixed and NOT at he beginning of mdat.
		SEARCH, //look for some packet signature
		LAST,    //last mdat in file (sometimes the first mdat is spurious.
		SPECIFIED //user supplied start.
	};

    Mp4();
    ~Mp4();

	void open(std::string filename);
	bool repair(std::string corrupt_filename, Mp4::MdatStrategy strategy = FIRST, int64_t begin = -1, bool skip_zeros = true, bool drifting = false);
	void fixTiming();
	int64_t findMdat(BufferedAtom *mdat,  MdatStrategy strategy = FIRST);
	BufferedAtom *findMdat(std::string filename, MdatStrategy strategy);
	int64_t contentStart();
	int searchNext(BufferedAtom *mdat, int64_t offset, int maxskip);
	BufferedAtom *bufferedMdat(Atom *mdat);


    bool save     (std::string output_filename);
    bool saveVideo(std::string output_filename) { return save(output_filename); }

    void printMediaInfo();
    void printAtoms();

	void analyze(int analyze_track = -1, bool interactive = true);
	//try to recover the working video, for debugging processing
	void simulate(MdatStrategy strategy, int64_t begin);

    static bool makeStreamable(std::string filename, std::string output_filename);

protected:
    std::string file_name;
    Atom *root;
    AVFormatContext *context;
    std::vector<Track> tracks;

    void close();
    bool parseTracks();
    void writeTracksToAtoms();

	MatchGroup match(int64_t offset, BufferedAtom *mdat);
};

#endif // MP4_H
