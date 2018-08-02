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


class Atom;
struct AVCodecContext;
struct AVCodec;


class Codec {
public:
    std::string     name;
    AVCodecContext *context;
    AVCodec        *codec;

    Codec();

    bool parse(Atom *trak, std::vector<int> &offsets, Atom *mdat);
    void clear();

    bool matchSample(const unsigned char *start, int maxlength);
    bool isKeyframe (const unsigned char *start, int maxlength);
    int  getLength  (      unsigned char *start, int maxlength, int &duration);

private:
    // Used by mp4a.
    int mask1;
    int mask0;
};


class Track {
public:
    Atom *trak;
    int   timescale;
    int   duration;
    Codec codec;

    std::vector<int> times;
    std::vector<int> keyframes; // 0 based!
    std::vector<int> sizes;
    std::vector<int> offsets;   // Should be 64-bit!

    Track();

    bool parse(Atom *trak, Atom *mdat);
    void clear();
    void writeToAtoms();
    void fixTimes();

protected:
    void cleanUp();

    std::vector<int> getSampleTimes  (Atom *t);
    std::vector<int> getKeyframes    (Atom *t);
    std::vector<int> getSampleSizes  (Atom *t);
    std::vector<int> getChunkOffsets (Atom *t);
    std::vector<int> getSampleToChunk(Atom *t, int nchunks);

    void saveSampleTimes();
    void saveKeyframes();
    void saveSampleSizes();
    void saveSampleToChunk();
    void saveChunkOffsets();
};

#endif // TRACK_H
