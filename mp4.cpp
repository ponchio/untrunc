//==================================================================//
/*
	Untrunc - mp4.cpp

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

#include <cassert>
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <limits>

#ifndef  __STDC_LIMIT_MACROS
# define __STDC_LIMIT_MACROS    1
#endif
#ifndef  __STDC_CONSTANT_MACROS
# define __STDC_CONSTANT_MACROS 1
#endif
extern "C" {
#include <stdint.h>
#ifdef _WIN32
# include <io.h>        //for: _isatty()
#else
# include <unistd.h>    //for: isatty()
#endif
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/log.h"
}

#include "mp4.h"
#include "atom.h"
#include "file.h"


// Stdio file descriptors.
#ifndef STDIN_FILENO
# define STDIN_FILENO   0
# define STDOUT_FILENO  1
# define STDERR_FILENO  2
#endif


using namespace std;


namespace {
	const int MaxFrameLength = 1600000;

	// Store addresses of C++ stdio stream buffers as identifiers.
	// These addresses differ per thread and must be statically linked in.
	// Assume that the stream buffers at these stored addresses
	//  are always connected to their underlaying stdio files.
	static const streambuf* const StdioBufs[] = {
		cin.rdbuf(), cout.rdbuf(), cerr.rdbuf(), clog.rdbuf()
	};
	// Store start-up terminal statuses of C++ stdio stream buffers.
	// These statuses differ per thread and must be statically linked in.
	// Assume that the statuses don't change during this thread life-time.
	static const bool StdioTTys[sizeof(StdioBufs)/sizeof(StdioBufs[0])] = {
#ifdef _WIN32
		_isatty(STDIN_FILENO), _isatty(STDOUT_FILENO), _isatty(STDERR_FILENO), _isatty(STDERR_FILENO)
#else
		 isatty(STDIN_FILENO),  isatty(STDOUT_FILENO),  isatty(STDERR_FILENO),  isatty(STDERR_FILENO)
#endif
	};

	// Return terminal status of a C++ stdio stream (cin, cout, cerr & clog).
	bool isTerminal(const ios& strm) {
		for(unsigned i = 0; i < sizeof(StdioBufs)/sizeof(StdioBufs[0]); ++i) {
			if(strm.rdbuf() == StdioBufs[i])
				return StdioTTys[i];
		}
		return false;
	}

	// Configure FFmpeg/Libav logging for use in C++.
	class AvLog {
		int lvl;
#ifdef AV_LOG_PRINT_LEVEL
		int flgs;
#endif
	public:
#ifdef AV_LOG_PRINT_LEVEL
# define DEFAULT_AVLOG_FLAGS	AV_LOG_PRINT_LEVEL
#else
# define DEFAULT_AVLOG_FLAGS	0
#endif
		explicit AvLog(int level = AV_LOG_INFO, int flags = DEFAULT_AVLOG_FLAGS)
			: lvl(av_log_get_level())
#ifdef AV_LOG_PRINT_LEVEL
			, flgs(av_log_get_flags())
#endif
		{
			if(lvl < level)
				av_log_set_level(lvl);
			av_log_set_flags(flags);
			cout.flush();   //flush C++ standard streams
			//cerr.flush();   //unbuffered -> nothing to flush
			clog.flush();
		}

		~AvLog() {
			fflush(stdout); //flush C stdio files
			fflush(stderr);
			av_log_set_level(lvl);
#ifdef AV_LOG_PRINT_LEVEL
			av_log_set_flags(flgs);
#endif
		}
	};

	// Redirect C files.
	// This does not effect C++ standard I/O streams (cin, cout, cerr, clog).
	class FileRedirect {
		FILE *&file_ref;
		FILE * file_value;
	public:
		explicit FileRedirect(FILE *&file, FILE *to_file)
			: file_ref(file)
			, file_value(file)
		{
			file = to_file;
			if(file_ref)  fflush(file_ref);
		}

		~FileRedirect() {
			if(file_ref)  fflush(file_ref);
			file_ref = file_value;
		}
	};
}; // namespace


// Mp4
Mp4::Mp4() : timescale(0), duration(0), root(NULL), context(NULL) { }

Mp4::~Mp4() {
	close();
}

void Mp4::open(string filename) {
	clog << "Opening: " << filename << '\n';
	close();

	{
		File file;
		if(!file.open(filename))
			throw "Could not open file: " + filename;

		root = new Atom;
		do {
			Atom *atom = new Atom;
			atom->parse(file);
#ifdef VERBOSE1
			clog << "Found atom: " << atom->name << '\n';
#endif
			root->children.push_back(atom);
		} while(!file.atEnd());
	}
	file_name = filename;

	if(root->atomByName("ctts"))
		clog << "Found 'Composition Time To Sample' atom (ctts). Out of order samples possible.\n";

	if(root->atomByName("sdtp"))
		clog << "Found 'Independent and Disposable Samples' atom (sdtp). I and P frames might need to recover that info.\n";

	Atom *mvhd = root->atomByName("mvhd");
	if(!mvhd)
		throw string("Missing 'Movie Header' atom (mvhd)");
	timescale = mvhd->readInt(12);
	duration  = mvhd->readInt(16);

	{
		AvLog useAvLog();
		// Register all formats and codecs
		av_register_all();
		// Open video file
#ifdef OLD_AVFORMAT_API
		int error = av_open_input_file(&context, filename.c_str(), NULL, 0, NULL);
#else
		int error = avformat_open_input(&context, filename.c_str(), NULL, NULL);
#endif
		if(error != 0)
			throw "Could not parse AV file: " + filename;

		// Retrieve stream information
#ifdef OLD_AVFORMAT_API
		if(av_find_stream_info(context) < 0)
#else
		if(avformat_find_stream_info(context, NULL) < 0)
#endif
			throw string("Could not find stream info");
	}

	parseTracks();
}

void Mp4::close() {
	Atom *rm_root = root;
	root = NULL;    //invalidate Mp4 data
	timescale = 0;
	duration  = 0;
	tracks.clear(); //must clear tracks before closing context
	if(context) {
		AvLog useAvLog(AV_LOG_ERROR);
#ifdef OLD_AVFORMAT_API
		av_close_input_file(&context);
#else
		avformat_close_input(&context);
#endif
		context = NULL;
	}
	file_name.clear();
	delete rm_root;
}

void Mp4::printMediaInfo() {
	if(context) {
		cout.flush();
		clog.flush();
		cout << "Media Info:\n"
			<< "  Default stream: " << av_find_default_stream_index(context) << '\n';
		AvLog useAvLog(AV_LOG_INFO);
		FileRedirect redirect(stderr, stdout);
		av_dump_format(context, 0, file_name.c_str(), 0);
	}
}

void Mp4::printAtoms() {
	if(root) {
		cout << "Atoms:\n";
		root->print(0);
	}
}

void Mp4::makeStreamable(string filename, string output_filename) {
	clog << "Make Streamable: " << filename << '\n';
	Atom rootAtom;
	{
		File file;
		if(!file.open(filename))
			throw "Could not open file: " + filename;

		do {
			Atom *atom = new Atom;
			atom->parse(file);
#ifdef VERBOSE1
			clog << "Found atom: " << atom->name << '\n';
#endif
			rootAtom.children.push_back(atom);
		} while(!file.atEnd());
	}
	Atom *ftyp = rootAtom.atomByName("ftyp");
	Atom *moov = rootAtom.atomByName("moov");
	Atom *mdat = rootAtom.atomByName("mdat");
	if(!moov) {
		cerr << "Missing 'Container for all the Meta-data' atom (moov).\n";
		return;
	}
	if(!mdat) {
		cerr << "Missing 'Media Data container' atom (mdat).\n";
		return;
	}

	if(mdat->start > moov->start) {
		clog << "File is already streamable." << endl;
		return;
	}

	int64_t old_start = mdat->start  + 8;
	int64_t new_start = moov->length + 8;
	if(ftyp)
		new_start += ftyp->length + 8;

	int64_t diff = new_start - old_start;
	clog << "Old: " << old_start << " -> New: " << new_start << '\n';
#if 0 // MIGHT HAVE TO FIX THIS ONE TOO?
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
#endif
	std::vector<Atom *> stcos = moov->atomsByName("stco");
	for(unsigned int i = 0; i < stcos.size(); i++) {
		Atom *stco = stcos[i];
		int64_t offsets = stco->readInt(4); //4 version, 4 number of entries, 4 entries
		for(int64_t j = 0; j < offsets; j++) {
			int64_t pos = 8 + 4*j;
			int64_t offset = stco->readInt(pos) + diff;
			clog << "O: " << offset << '\n';
			stco->writeInt(offset, pos);
		}
	}

	{
		//save to file
		clog << "Saving to: " << output_filename << '\n';
		File file;
		if(!file.create(output_filename))
			throw "Could not create file for writing: " + output_filename;

		if(ftyp)
			ftyp->write(file);
		moov->write(file);
		mdat->write(file);
	}
	clog << endl;
}

void Mp4::saveVideo(string output_filename) {
	// We save all atoms except:
	//  ctts: composition offset (we use sample to time)
	//  cslg: because it is used only when ctts is present
	//  stps: partial sync, same as sync
	//
	// Movie is made by ftyp, moov, mdat (we need to know mdat begin, for absolute offsets).
	// Assume offsets in stco are absolute and so to find the relative just subtrack mdat->start + 8.

	clog << "Saving to: " << output_filename << '\n';
	if(!root) {
		cerr << "No file opened.\n";
		return;
	}

	duration = 0;
	for(unsigned int i = 0; i < tracks.size(); i++) {
		Track &track = tracks[i];
		track.writeToAtoms();
		//convert to movie timescale
		clog << "Track " << i << ": duration: " << track.duration << " timescale: " << track.timescale << '\n';
		//int track_duration = static_cast<int>(double(track.duration) * double(timescale) / double(track.timescale));
		int track_duration = static_cast<int>((int64_t(track.duration) * timescale - 1 + track.timescale) / track.timescale);
		if(track_duration > duration)
			duration = track_duration;

		Atom *tkhd = track.trak->atomByName("tkhd");
		tkhd->writeInt(track_duration, 20); //in movie timescale, not track timescale
	}
	cout.flush();
	Atom *mvhd = root->atomByName("mvhd");
	if(!mvhd)
		throw string("Missing 'Movie Header' atom (mvhd)");
	clog << "Movie duration: " << duration << " timescale: " << timescale << '\n';
	mvhd->writeInt(duration, 16);

	Atom *ftyp = root->atomByName("ftyp");
	Atom *moov = root->atomByName("moov");
	Atom *mdat = root->atomByName("mdat");
	if(!moov) {
		cerr << "Missing 'Container for all the Meta-data' atom (moov).\n";
		return;
	}
	if(!mdat) {
		cerr << "Missing 'Media Data container' atom (mdat).\n";
		return;
	}

	moov->prune("ctts");
	moov->prune("cslg");
	moov->prune("stps");

	root->updateLength();

	//fix offsets
	int64_t offset = 8 + moov->length;
	if(ftyp)
		offset += ftyp->length; //not all mov have a ftyp.

	for(unsigned int t = 0; t < tracks.size(); t++) {
		Track &track = tracks[t];
		for(unsigned int i = 0; i < track.offsets.size(); i++)
			track.offsets[i] += offset;

		track.writeToAtoms();  //need to save the offsets back to the atoms
	}

	{
		//save to file
		File file;
		if(!file.create(output_filename))
			throw "Could not create file for writing: " + output_filename;

		if(ftyp)
			ftyp->write(file);
		moov->write(file);
		mdat->write(file);
	}
	clog << endl;
}

void Mp4::analyze(bool interactive) {
	cout << "Analyze:\n";
	if(!root) {
		cerr << "No file opened.\n";
		return;
	}

	Atom *mdat = root->atomByName("mdat");
	if(!mdat) {
		cerr << "Missing 'Media Data container' atom (mdat).\n";
		return;
	}

	if(interactive) {
		// For interactive analyzis, std::cin & std::cout must be connected to a terminal/tty.
		if(!isTerminal(cin)) {
#ifdef VERBOSE1
			clog << "Cannot analyze interactively as input doesn't come directly from a terminal.\n";
#endif
			interactive = false;
		}
		if(interactive && !isTerminal(cout)) {
#ifdef VERBOSE1
			clog << "Cannot analyze interactively as output doesn't go directly to a terminal.\n";
#endif
			interactive = false;
		}
		if(interactive)
			cin.clear();    //reset state - clear transient errors of previous input operations
#ifdef VERBOSE1
		clog.flush();
#endif
	}

	for(unsigned int i = 0; i < tracks.size(); i++) {
		cout << "\n\nTrack " << i << endl;
		Track &track = tracks[i];
		cout << "Track codec: " << track.codec.name << '\n';
		cout << "Keyframes  : " << track.keyframes.size() << "\n\n";
		for(unsigned int i = 0; i < track.keyframes.size(); i++) {
			int k = track.keyframes[i];
			int64_t offset = track.offsets[k] - (mdat->start + 8);
			int64_t begin  = mdat->readInt(offset);
			int64_t next   = mdat->readInt(offset + 4);
			cout << setw(8) << k
				<< " Size: " << setw(6) << track.sizes[k]
				<< " offset " << setw(10) << track.offsets[k]
				<< "  begin: " << hex << setw(5) << begin << ' ' << setw(8) << next << dec << '\n';
		}

		for(unsigned int i = 0; i < track.offsets.size(); i++) {
			int64_t offset = track.offsets[i] - (mdat->start + 8);
			unsigned char *start = &(mdat->content[offset]);
			int64_t maxlength64 = mdat->contentSize() - offset;
			if(maxlength64 > MaxFrameLength)
				maxlength64 = MaxFrameLength;
			int maxlength = static_cast<int>(maxlength64);

			int64_t begin = mdat->readInt(offset);
			int64_t next  = mdat->readInt(offset + 4);
			int64_t end   = mdat->readInt(offset + track.sizes[i] - 4);
			cout << "\n\n>" << setw(7) << i
				<< " Size: " << setw(6) << track.sizes[i]
				<< " offset " << setw(10) << track.offsets[i]
				<< "  begin: " << hex << setw(5) << begin << ' ' << setw(8) << next
				<< " end: " << setw(8) << end << dec << '\n';

			bool matches  = track.codec.matchSample(start, maxlength);
			int  duration = 0;
			int  length   = track.codec.getLength(start, maxlength, duration);
			//TODO check if duration is working with the stts duration.
			cout << "Length: " << length << " true length: " << track.sizes[i] << '\n';

			bool wait = false;
			if(!matches) {
				cerr << "- Match failed!\n";
				wait = interactive;
			}
			if(length != track.sizes[i]) {
				cerr << "- Length mismatch!\n";
				wait = interactive;
			}
			if(length < -1 || length > MaxFrameLength) {
				cerr << "- Invalid length!\n";
				wait = interactive;
			}
			if(wait) {
				//cout and clog already flushed by cerr
				cout << "  <Press [Enter] for next match>\r";
				cin.ignore(numeric_limits<streamsize>::max(), '\n');
			}
			//assert(matches);
			//assert(length == track.sizes[i]);
		}
	}
	cout << endl;
}

void Mp4::writeTracksToAtoms() {
	for(unsigned int i = 0; i < tracks.size(); i++)
		tracks[i].writeToAtoms();
}

void Mp4::parseTracks() {
	assert(root != NULL);

	Atom *mdat = root->atomByName("mdat");
	if(!mdat) {
		cerr << "Missing 'Media Data container' atom (mdat).\n";
		return;
	}
	vector<Atom *> traks = root->atomsByName("trak");
	for(unsigned int i = 0; i < traks.size(); i++) {
		Track track;
		track.codec.context = context->streams[i]->codec;
		track.parse(traks[i], mdat);
		tracks.push_back(track);
	}
}

void Mp4::repair(string filename) {
	clog << "Repair: " << filename << '\n';
	BufferedAtom *mdat = NULL;
	{
		File file;
		if(!file.open(filename))
			throw "Could not open file: " + filename;

		//find mdat. fails with krois and a few other.
		//TODO check for multiple mdat, or just look for the first one.
		while(true) {
			Atom atom;
			try {
				atom.parseHeader(file);
			} catch(string) {
				throw string("Failed to parse atoms in truncated file");
			}

			if(atom.name != string("mdat")) {
				off_t pos = file.pos();
				file.seek(pos - 8 + atom.length);
				continue;
			}

			mdat = new BufferedAtom(filename);
			mdat->start = atom.start;
			memcpy(mdat->name, atom.name, sizeof(mdat->name)-1);
			memcpy(mdat->head, atom.head, sizeof(mdat->head));
			memcpy(mdat->version, atom.version, sizeof(mdat->version));

			mdat->file_begin = file.pos();
			mdat->file_end   = file.length() - file.pos();
			//mdat->content = file.read(file.length() - file.pos());
			break;
		}
	}

	for(unsigned int i = 0; i < tracks.size(); i++)
		tracks[i].clear();

	//mp4a is more reliable than avc1.
	if(tracks.size() > 1 && tracks[0].codec.name != "mp4a" && tracks[1].codec.name == "mp4a") {
#ifdef VERBOSE1
		clog << "Swapping tracks: " << tracks[0].codec.name << " <-> mp4a.\n";
#endif
		swap(tracks[0], tracks[1]);
	}

	//mp4a can be decoded and reports the number of samples (duration in samplerate scale).
	//in some videos the duration (stts) can be variable and we can rebuild them using these values.
	vector<int> audiotimes;
	unsigned long count = 0;
	off_t offset = 0;
	while(offset < mdat->contentSize()) {
		//unsigned char *start = &mdat->content[offset];
		int64_t maxlength64 = mdat->contentSize() - offset;
		if(maxlength64 > MaxFrameLength)
			maxlength64 = MaxFrameLength;
		unsigned char *start = mdat->getFragment(offset, maxlength64);
		int maxlength = static_cast<int>(maxlength64);

		unsigned int begin = mdat->readInt(offset);
		if(begin == 0) {
#if 0 // AARRGH this sometimes is not very correct, unless it's all zeros.
			//skip zeros to next 000
			offset &= 0xfffff000;
			offset += 0x1000;
#else
			offset += 4;
#endif
			continue;
		}

#ifdef VERBOSE1
		unsigned int next  = mdat->readInt(offset + 4);
		clog << "Offset: " << setw(10) << offset
			<< "  begin: " << hex << setw(5) << begin << ' ' << setw(8) << next << dec << '\n';
#endif

		//skip fake moov
		if(start[4] == 'm' && start[5] == 'o' && start[6] == 'o' && start[7] == 'v') {
			clog << "Skipping 'Container for all the Meta-data' atom (moov): begin: 0x" << hex << swap32(begin) << dec << ".\n";
			offset += swap32(begin);
			continue;
		}

		bool found = false;
		for(unsigned int i = 0; i < tracks.size(); i++) {
			Track &track = tracks[i];
			clog << "Track " << i << " codec: " << track.codec.name << '\n';
			//sometime audio packets are difficult to match, but if they are the only ones....
			if(tracks.size() > 1 && !track.codec.matchSample(start, maxlength))
				continue;
			int duration = 0;
			int length   = track.codec.getLength(start, maxlength, duration);
			if(length < -1 || length > MaxFrameLength) {
				clog << "\nInvalid length: " << length << ". Wrong match in track: " << i << ".\n";
				continue;
			}
			if(length == -1 || length == 0) {
				continue;
			}
			if(length >= maxlength)
				continue;
#ifdef VERBOSE1
			if(length > 8)
				clog << "Length: " << length << " found as: " << track.codec.name << '\n';
#endif
			bool keyframe = track.codec.isKeyframe(start, maxlength);
			if(keyframe)
				track.keyframes.push_back(track.offsets.size());
			track.offsets.push_back(offset);
			track.sizes.push_back(length);
			offset += length;

			if(duration)
				audiotimes.push_back(duration);

			found = true;
			break;
		}
#ifdef VERBOSE1
		clog << '\n';
#endif

		if(!found) {
			//this could be a problem for large files
			//assert(mdat->content.size() + 8 == mdat->length);
			mdat->file_end = mdat->file_begin + offset;
			mdat->length   = mdat->file_end - mdat->file_begin;
			//mdat->content.resize(offset);
			//mdat->length = mdat->content.size() + 8;
			break;
		}
		count++;
	}

	clog << "Found " << count << " packets.\n";

	for(unsigned int i = 0; i < tracks.size(); i++) {
		if(audiotimes.size() == tracks[i].offsets.size())
			swap(audiotimes, tracks[i].times);

		tracks[i].fixTimes();
	}

	Atom *original_mdat = root->atomByName("mdat");
	if(!original_mdat) {
		delete mdat;
		cerr << "Missing 'Media Data container' atom (mdat).\n";
		return;
	}
	mdat->start = original_mdat->start;
#ifdef VERBOSE1
	clog << "Replacing 'Media Data content' atom (mdat).\n"
#endif
	root->replace(original_mdat, mdat);
	//original_mdat->content.swap(mdat->content);
	//original_mdat->start = -8;
	delete original_mdat;

	clog << endl;
}

