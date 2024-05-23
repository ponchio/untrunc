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
#include <set>
#include <algorithm>
#include <functional>
#include <string>
#include <iostream>
#include <ios>          // Pre-C++11: may not be included by <iostream>.
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
# include <io.h>        // for: _isatty()
#else
# include <unistd.h>    // for: isatty()
#endif
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/log.h"
}  // extern "C"

#include "mp4.h"
#include "atom.h"
#include "file.h"
#include "log.h"

// Stdio file descriptors.
#ifndef STDIN_FILENO
# define STDIN_FILENO   0
# define STDOUT_FILENO  1
# define STDERR_FILENO  2
#endif


#include <algorithm>
using namespace std;


namespace {
const int MaxFrameLength = 20000000;


// Store start-up addresses of C++ stdio stream buffers as identifiers.
// These addresses differ per process and must be statically linked in.
// Assume that the stream buffers at these stored addresses
//  are always connected to their underlaying stdio files.
static const streambuf* const StdioBufs[] = {
	cin.rdbuf(), cout.rdbuf(), cerr.rdbuf(), clog.rdbuf()
};
// Store start-up terminal/TTY statuses of C++ stdio stream buffers.
// These statuses differ per process and must be statically linked in.
// Assume that the statuses don't change during the process life-time.
static const bool StdioTtys[sizeof(StdioBufs)/sizeof(StdioBufs[0])] = {
		#ifdef _WIN32
		_isatty(STDIN_FILENO), _isatty(STDOUT_FILENO), _isatty(STDERR_FILENO), _isatty(STDERR_FILENO)
		#else
		(bool)isatty(STDIN_FILENO),  (bool)isatty(STDOUT_FILENO),  (bool)isatty(STDERR_FILENO),  (bool)isatty(STDERR_FILENO)
		#endif
};

// Is a Terminal/Console/TTY connected to the C++ stream?
// Use on C++ stdio chararacter streams: cin, cout, cerr and clog.
bool isATerminal(const ios& strm) {
	for(unsigned int i = 0; i < sizeof(StdioBufs)/sizeof(StdioBufs[0]); ++i) {
		if(strm.rdbuf() == StdioBufs[i])
			return StdioTtys[i];
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

	explicit AvLog()
		: lvl(av_log_get_level())
	#ifdef AV_LOG_PRINT_LEVEL
		, flgs(av_log_get_flags())
	#endif
	{
		av_log_set_flags(DEFAULT_AVLOG_FLAGS);
//		cout.flush();   // Flush C++ standard streams.
		//cerr.flush();   // Unbuffered -> nothing to flush.
//		clog.flush();
	}

	explicit AvLog(int level, int flags = DEFAULT_AVLOG_FLAGS)
		: lvl(av_log_get_level())
	#ifdef AV_LOG_PRINT_LEVEL
		, flgs(av_log_get_flags())
	#endif
	{
		if(lvl < level)
			av_log_set_level(level);
		av_log_set_flags(flags);
		cout.flush();   // Flush C++ standard streams.
		//cerr.flush();   // Unbuffered -> nothing to flush.
		clog.flush();
	}

	~AvLog() {
		fflush(stdout); // Flush C stdio files.
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
	Log::debug << "Opening: " << filename << '\n';
	close();

	try {  // Parse ok file.
		File file;
		if(!file.open(filename))
			throw "Could not open file: " + filename;

		root = new Atom;
		do {
			Atom *atom = new Atom;
			atom->parse(file);
			Log::debug << "Found atom: " << atom->name << '\n';

			root->children.push_back(atom);
		} while(!file.atEnd());

	} catch(const string &error) {
		Log::info << error << "\n";
		Log::flush();
		if(!root->atomByName("moov"))
			throw string("Failed parsing working mp4. Maybe the broken and working files got inverted.");
	}/* catch(...) {
		throw string("Failed parsing working mp4. Maybe the broken and working files got inverted.");
	}*/
	// {
	file_name = filename;

	if(root->atomByName("ctts"))
		Log::debug << "Found 'Composition Time To Sample' atom (ctts). Out of order samples possible.\n";

	if(root->atomByName("sdtp"))
		Log::debug << "Found 'Independent and Disposable Samples' atom (sdtp). I and P frames might need to recover that info.\n";

	Atom *mvhd = root->atomByName("mvhd");
	if(!mvhd)
		throw string("Missing 'Movie Header' atom (mvhd)");
	// ASSUME: mvhd atom version 0.
	timescale = mvhd->readInt(12);
	duration  = mvhd->readInt(16);

	{  // Setup AV library.
		AvLog useAvLog();
		// Register all formats and codecs.
		av_register_all();
		// Open video file.
#ifdef OLD_AVFORMAT_API
		int error = av_open_input_file(&context, filename.c_str(), NULL, 0, NULL);
#else
		int error = avformat_open_input(&context, filename.c_str(), NULL, NULL);
#endif
		if(error != 0)
			throw "Could not parse AV file: " + filename;

		// Retrieve stream information.
#ifdef OLD_AVFORMAT_API
		if(av_find_stream_info(context) < 0)
#else
		if(avformat_find_stream_info(context, NULL) < 0)
#endif
			throw string("Could not find stream info");
	}  // {

	parseTracks();
}

void Mp4::close() {
	Atom *rm_root = root;
	root      = NULL;   // Invalidate Mp4 data.
	timescale = 0;
	duration  = 0;
	tracks.clear();     // Must clear tracks before closing context.
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
		Log::info << "Media Info:\n"
				  << "  Default stream: " << av_find_default_stream_index(context) << '\n';
		AvLog useAvLog(AV_LOG_INFO);
		FileRedirect redirect(stderr, stdout);
		av_dump_format(context, 0, file_name.c_str(), 0);
	}
}

void Mp4::printAtoms() {
	if(root) {
		Log::info << "Atoms:\n";
		root->print(0);
	}
}

bool Mp4::makeStreamable(string filename, string output_filename) {
	Log::info << "Make Streamable: " << filename << '\n';
	Atom atom_root;
	{  // Parse input file.
		File file;
		if(!file.open(filename))
			throw "Could not open file: " + filename;

		while(!file.atEnd()) {
			Atom *atom = new Atom;
			atom->parse(file);
			Log::debug << "Found atom: " << atom->name << '\n';
			atom_root.children.push_back(atom);
		}
	}  // {

	Atom *ftyp = atom_root.atomByName("ftyp");
	Atom *moov = atom_root.atomByName("moov");
	Atom *mdat = atom_root.atomByName("mdat");
	if(!moov || !mdat) {
		if(!moov)
			Log::error << "Missing 'Container for all the Meta-data' atom (moov).\n";
		if(!mdat)
			Log::error << "Missing 'Media Data container' atom (mdat).\n";
		return false;
	}

	if(mdat->start > moov->start) {
		Log::info << "File is already streamable." << endl;
		return true;
	}

	int64_t old_start = mdat->start  + 8;
	int64_t new_start = moov->length + 8;
	if(ftyp)
		new_start += ftyp->length;

	int64_t diff = new_start - old_start;
	Log::debug << "Old: " << old_start << " -> New: " << new_start << '\n';

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
	for(unsigned int i = 0; i < stcos.size(); ++i) {
		Atom *stco = stcos[i];
		int32_t nchunks = stco->readInt(4); // 4 version, 4 number of entries, 4 entries.
		for(int j = 0; j < nchunks; ++j) {
			int64_t pos    = int64_t(8) + 4*j;
			int64_t offset = stco->readInt(pos) + diff;
			Log::debug << "O: " << offset << '\n';
			stco->writeInt(offset, pos);
		}
	}

	{  // Save to output file.
		Log::debug << "Saving to: " << output_filename << '\n';
		File file;
		if(!file.create(output_filename))
			throw "Could not create file for writing: " + output_filename;

		if(ftyp)
			ftyp->write(file);
		moov->write(file);
		mdat->write(file);
	}  // {
	Log::debug << endl;
	return true;
}

bool Mp4::save(string output_filename) {
	// We save all atoms except:
	//  ctts: composition offset (we use sample to time).
	//  cslg: because it is used only when ctts is present.
	//  stps: partial sync, same as sync.
	//
	// Movie is made by ftyp, moov, mdat (we need to know mdat begin, for absolute offsets).
	// Assume offsets in stco are absolute and so to find the relative just subtrack mdat->start + 8.

	Log::info << "Saving to: " << output_filename << '\n';
	if(!root) {
		Log::error << "No file opened.\n";
		return false;
	}

	if(timescale == 0) {
		timescale = 600;  // Default movie time scale.
		Log::info << "Using new movie time scale: " << timescale << ".\n";
	}
	duration = 0;
	for(unsigned int i = 0; i < tracks.size(); ++i) {
		Track &track = tracks[i];
		Log::debug << "Track " << i << " (" << track.codec.name << "): duration: "
				   << track.duration << " timescale: " << track.timescale << '\n';
		if(track.timescale == 0 && track.duration != 0)
			Log::info << "Track " << i << " (" << track.codec.name << ") has no time scale.\n";

		track.writeToAtoms();

		// Convert duration to movie timescale.
		if(timescale == 0) continue;  // Shouldn't happen.
		// Use default movie time scale if no track time scale was found.
		int track_timescale = (track.timescale != 0) ? track.timescale : 600;
		//convert track duration (in track.timescale units) to movie timesscale units.
		int track_duration  = static_cast<int>((int64_t(track.duration) * timescale - 1 + track_timescale)
											   / track_timescale);

		if(duration < track_duration)
			duration = track_duration;

		Atom *tkhd = track.trak->atomByName("tkhd");
		if(!tkhd) {
			Log::debug << "Missing 'Track Header' atom (tkhd).\n";
			continue;
		}
		if(tkhd->readInt(20) == track_duration) continue;

		Log::debug << "Adjusting track duration to movie timescale: New duration: "
				   << track_duration << " timescale: " << timescale << ".\n";
		tkhd->writeInt(track_duration, 20); // In movie timescale, not track timescale.
	}

	Log::debug << "Movie duration: " << duration/(double)timescale << "s with timescale: " << timescale << '\n';
	Atom *mvhd = root->atomByName("mvhd");
	if(!mvhd)
		throw string("Missing 'Movie Header' atom (mvhd)");
	mvhd->writeInt(duration, 16);

	Atom *ftyp = root->atomByName("ftyp");
	Atom *moov = root->atomByName("moov");
	Atom *mdat = root->atomByName("mdat");
	if(!moov || !mdat) {
		if(!moov)
			Log::error << "Missing 'Container for all the Meta-data' atom (moov).\n";
		if(!mdat)
			Log::error << "Missing 'Media Data container' atom (mdat).\n";
		return false;
	}

	moov->prune("ctts");
	moov->prune("cslg");
	moov->prune("stps");

	root->updateLength();





	//we need to add mdat header bytes
	int64_t offset = moov->length + 8;
	if(mdat->length64)
		offset += 8;

	if(ftyp)
		offset += ftyp->length; // Not all .mov have an ftyp.

	for(unsigned int t = 0; t < tracks.size(); ++t) {
		Track &track = tracks[t];
		for(unsigned int i = 0; i < track.offsets.size(); ++i)
			track.offsets[i] += offset;

		track.writeToAtoms();  // Need to save the offsets back to the atoms.
	}

	{  // Save to output file.
		File file;
		if(!file.create(output_filename))
			throw "Could not create file for writing: " + output_filename;

		if(ftyp)
			ftyp->write(file);
		moov->write(file);
		mdat->write(file);
	}  // {
	return true;
}

//used for variable sample time attempt to guess
class ChunkTime: public Track::Chunk {
public:
	int sample;
	int sample_time;
	int track;
	bool operator<(const ChunkTime &c) const { return offset < c.offset; }
};

void Mp4::analyze(int analyze_track, bool interactive) {
	Log::info << "Analyze:\n";
	if(!root) {
		Log::error << "No file opened.\n";
		return;
	}

	Atom *_mdat = root->atomByName("mdat");
	if(!_mdat) {
		Log::error << "Missing 'Media Data container' atom (mdat).\n";
		return;
	}

	BufferedAtom *mdat = bufferedMdat(_mdat);
	if(interactive) {
		// For interactive analyzis, std::cin & std::cout must be connected to a terminal/tty.
		if(!isATerminal(cin)) {
			Log::debug << "Cannot analyze interactively as input doesn't come directly from a terminal.\n";
			interactive = false;
		}
		if(interactive && !isATerminal(cout)) {
			Log::debug << "Cannot analyze interactively as output doesn't go directly to a terminal.\n";
			interactive = false;
		}
		if(interactive)
			cin.clear();  // Reset state - clear transient errors of previous input operations.
#ifdef VERBOSE1
		clog.flush();
#endif
	}

	//sample time analysis
	vector<ChunkTime> chunktimes;

	for(unsigned int trackId = 0; trackId < tracks.size(); ++trackId) {

		if(analyze_track != -1 && trackId != analyze_track)
			continue;
		Log::info << "\n\nTrack " << trackId << endl;
		Track &track = tracks[trackId];

		if(track.hint_track) {
			Log::info << "Hint track for track: " << track.hinted_id << "\n";
		} else {
			Log::info << "Track codec: " << track.codec.name << '\n';
		}

		Log::info << "Keyframes  : " << track.keyframes.size() << "\n\n";

		if(track.codec.pcm) {
			Log::info << "PCM codec, skipping keyframes\n\n";
		} else if(track.default_size) {
			Log::info << "Default size packets, skipping keyframes\n\n";
		} else {
			for(unsigned int i = 0; i < track.keyframes.size(); ++i) {
				int k = track.keyframes[i];
				int64_t  offset = track.offsets[k] - mdat->content_start;
				uint32_t begin  = mdat->readInt(offset);
				uint32_t next   = mdat->readInt(offset + 4);
				Log::debug << setw(8) << k
						   << " Size: " << setw(6) << track.getSize(k)
						   << " offset " << setw(10) << track.offsets[k]
							  << "  begin: " << hex << setw(5) << begin << ' ' << setw(8) << next << dec
							  << " time: " << (track.default_time ? track.default_time : track.times[i]) << '\n';
			}
		}

		if(track.default_size) {
			Log::info << "Constant size for samples: " << track.default_size << "\n";
		} else {
			Log::info << "Sizes for samples: " << "\n";
			for(int i = 0; i < 10 && i < track.sample_sizes.size(); i++) {
				Log::info << track.sample_sizes[i] << " ";
			}
			Log::info << "\n";
		}

		if(track.default_time) {
			Log::info << "Constant time for packet: " << track.default_time << endl;
		}

		if(track.default_size) {
			if(!track.codec.pcm) {
				Log::info << "Not a PCM codec, default size though..  we have hope.\n";
			}
			int i = 0;
			for(Track::Chunk &chunk: track.chunks) {
				int64_t offset = chunk.offset - mdat->content_start;

				int64_t maxlength64 = mdat->contentSize() - offset;
				if(maxlength64 > MaxFrameLength)
					maxlength64 = MaxFrameLength;
				int maxlength = static_cast<int>(maxlength64);

				int32_t begin = mdat->readInt(offset);
				int32_t next  = mdat->readInt(offset + 4);
				int32_t end   = mdat->readInt(offset + track.getSize(i) - 4);

				Log::info << " Size: " << setw(6) << chunk.size
						  << " offset " << setw(10) << chunk.offset
						  << "  begin: " << hex << setw(8) << begin << ' ' << setw(8) << next
						  << " end: " << setw(8) << end << dec
						  << " time: " << (track.default_time ? track.default_time : track.times[i]) << '\n';
				i++;
			}
		}

		int sample = 0;
		for(unsigned int i = 0; i < track.chunks.size(); ++i) {
			Track::Chunk &chunk = track.chunks[i];

			ChunkTime chunktime;
			chunktime.offset = chunk.offset;
			chunktime.nsamples = chunk.nsamples;
			chunktime.size = chunk.size;
			chunktime.track = trackId;
			chunktime.sample_time = 0;

			int64_t offset = chunk.offset - mdat->content_start;
			for(int k = 0; k < chunk.nsamples; k++) {
				int64_t size = track.getSize(chunk.first_sample + k);
				if(track.codec.pcm)
					size = chunk.size;

				int64_t extendedsize = std::min(mdat->contentSize() - offset, size + 200000);
				unsigned char *start = mdat->getFragment(offset, extendedsize); //&(mdat->content[offset]);
				int32_t begin = mdat->readInt(offset);
				int32_t next  = mdat->readInt(offset + 4);
				Log::info << " Size: " << setw(6) << size
									  << " offset " << setw(10) << offset + mdat->content_start
									  << "  begin: " << hex << setw(8) << begin << ' ' << setw(8) << next <<  dec << '\n';

				chunktime.sample_time += track.default_time ? track.default_time : track.times[sample];

				Match match = track.codec.match(start, extendedsize);

				MatchGroup matches = this->match(offset, mdat);
				Match &best = matches.back();
				if(best.id !=  trackId) {
					Log::error << "Wrong track matches with higher score";
					MatchGroup rematches = this->match(offset, mdat);
				}

				if(track.codec.pcm)
					break;

				sample++;
				offset += size;



				if(match.length == size)
					continue;

				if(match.length == 0) {
					Log::error << "- Match failed!\n";
				} else if(match.length < 0 || match.length > MaxFrameLength) {
					Log::error << "- Invalid length!\n";
				} else {
					Log::error << "- Length mismatch: got " << match.length << " expected: " << size << "\n";
				}

				if(interactive) {
					Log::info << "  <Press [Enter] for next match>\r";
					cin.ignore(numeric_limits<streamsize>::max(), '\n');
				}

			}
			chunktimes.push_back(chunktime);

		}
	}
	cout << "Chunks " << tracks[1].chunks.size() << " times: " << tracks[1].times.size() << endl;
	std::sort(chunktimes.begin(), chunktimes.end());
	vector<float> times(tracks.size(), 0);
	int count = 0;
	for(ChunkTime &c: chunktimes) {
		times[c.track] += c.sample_time / (float)tracks[c.track].timescale;
		cout << "Track: " << c.track <<  " time: " << c.sample_time << " nsamples: " << c.nsamples << " tot: " << times[c.track] << endl;
		if(count++ > 200)
			break;	}
}

void Mp4::simulate(Mp4::MdatStrategy strategy, int64_t begin) {

	//TODO remove duplicated code with analyze.
	Log::info << "Simulate:\n";

	if(!root) {
		Log::error << "No file opened.\n";
		return;
	}
	Atom *original_mdat = root->atomByName("mdat");
	if(!original_mdat) {
		Log::error << "Missing 'Media Data container' atom (mdat).\n";
		return;
	}

	BufferedAtom *mdat = findMdat(file_name, strategy);
	if(!mdat) {
		Log::error << "MDAT not found.\n";
		return;
	}


	//sort packets by start, length, track id

	std::vector<Match> packets;

	std::string codecs[tracks.size()];

	for(unsigned int t = 0; t < tracks.size(); ++t) {
		Track &track = tracks[t];
		codecs[t] = track.codec.name;
		//if pcm -> offsets should be chunks!

		if(track.default_size) {
			Log::debug << "Track " << t << " packets: " << track.chunks.size() << endl;
			for(unsigned int i = 0;i < track.chunks.size(); i++) {
				Track::Chunk &chunk = track.chunks[i];
				Match match;
				match.id = t;

				match.offset = chunk.offset;
				match.length = chunk.size;
				match.duration = track.default_time? track.default_time : track.times[i];
				packets.push_back(match);
			}
		} else {
			Log::debug << "Track " << t << " packets: " << track.offsets.size() << endl;

			for(unsigned int i = 0; i < track.chunks.size(); ++i) {
				Track::Chunk &chunk = track.chunks[i];
				uint64_t offset = chunk.offset;
				for(int k = 0; k < chunk.nsamples; k++) {
					Match match;
					match.id = t;
					match.offset = offset;
					match.length = track.getSize(chunk.first_sample + k);
					match.duration = track.getTimes(k);
					packets.push_back(match);
					offset += match.length;
				}
			}
/*
			for(unsigned int i = 0; i < track.offsets.size(); ++i) {
				Match match;

				match.id = t;
				match.offset = track.offsets[i];
				match.length = track.chunk_sizes[i];
				match.duration = track.default_time? track.default_time : track.times[i];
				packets.push_back(match);
			}*/
		}
	}
	std::sort(packets.begin(), packets.end(), [](const Match &m1, const Match &m2) { return m1.offset < m2.offset; });

	if(packets[0].offset != original_mdat->content_start) {
		Log::error << "First packet does not start with mdat, finding the start of the packets might be problematic" << endl;
	}


	//ensure mdat is correctly found.
	if(original_mdat->content_start != mdat->file_begin) {
		Log::error << "Wrong start of mdat: " << mdat->file_begin << " should be " << original_mdat->content_start << "\n";
		mdat->file_begin = mdat->content_start = packets[0].offset;
	}



	int64_t offset = 0; //mdat->file_begin;
	for(Match &m: packets) {
		if(m.offset != mdat->file_begin + offset) {
			Log::error << "Some empty space to be skipped! Real start = " << m.offset << " mdat start guessed at: " << offset + mdat->file_begin << "\n";
			break;
		}

		unsigned char *start = mdat->getFragment(offset, 8);
		unsigned int begin = readBE<int>(start);
		unsigned int next  = readBE<int>(start + 4);
		Log::debug << "\n" << codecs[m.id] << " (" << m.id << ") offset: " << setw(10) << (m.offset) << " Length: " << m.length
					<< "  begin: " << hex << setw(8) << begin << ' ' << setw(8) << next << dec << "\n";

		MatchGroup matches = match(offset, mdat);
		Match &best = matches.back();


		for(Match m: matches) {
			Log::debug << "Match for: " << m.id << " (" << codecs[m.id] << ") chances: " << m.chances << " length: " << m.length << "\n";
		}
		if(best.chances == 0.0f) {
			//we could not detect best, in reconstruction we need to backtrack
			Log::error << "Could not match packet for track " << m.id << "\n";
			Log::flush();

			Log::info << "  <Press [Enter] for next match>\r";
			cin.ignore(numeric_limits<streamsize>::max(), '\n');

//			break;
		}
		if(m.id  != best.id) {
			Log::error << "Mismatch! Packet track should be on track: " << m.id << " (" << codecs[m.id] << ") it is: " << best.id << " (" << codecs[best.id] << ")\n";
			Log::flush();

			Log::info << "  <Press [Enter] for next match>\r";
			cin.ignore(numeric_limits<streamsize>::max(), '\n');
//			break;
		}
		if(m.length != best.length) {
			Log::error << "Packet length is wrong." << endl;
			Log::flush();

			Log::info << "  <Press [Enter] for next match>\r";
			cin.ignore(numeric_limits<streamsize>::max(), '\n');
//			break;
		}
		offset += m.length;
	}
}

MatchGroup Mp4::match(int64_t offset, BufferedAtom *mdat) {
	MatchGroup group;
	group.offset = offset;

	int64_t maxlength64 = mdat->contentSize() - offset;
	if(maxlength64 > MaxFrameLength)
		maxlength64 = MaxFrameLength;
	unsigned char *start = mdat->getFragment(offset, maxlength64);
	int maxlength = static_cast<int>(maxlength64);


	for(unsigned int i = 0; i < tracks.size(); ++i) {
		Track &track = tracks[i];
		Match m = track.codec.match(start, maxlength);
		m.id = i;
		m.offset = group.offset;
		group.push_back(m);
	}

	sort(group.begin(), group.end());//, [](const Match &m1, const Match &m2) { return m1.chances > m2.chances; });
	return group;
}

void Mp4::writeTracksToAtoms() {
	for(unsigned int i = 0; i < tracks.size(); ++i)
		tracks[i].writeToAtoms();
}

bool Mp4::parseTracks() {
	assert(root != NULL);

	Atom *_mdat = root->atomByName("mdat");
	if(!_mdat) {
		Log::error << "Missing 'Media Data container' atom (mdat).\n";
		return false;
	}
	BufferedAtom *mdat = bufferedMdat(_mdat);

	vector<Atom *> traks = root->atomsByName("trak");
	for(unsigned int i = 0; i < traks.size(); ++i) {
		Track track;
		track.codec.context = context->streams[i]->codec;
		track.parse(traks[i]);
		track.codec.stats.init(track, mdat);

		tracks.push_back(track);
	}
	return true;
}

BufferedAtom *Mp4::bufferedMdat(Atom *mdat) {
	BufferedAtom *_mdat = new BufferedAtom(file_name);
	_mdat->start = mdat->start;
	memcpy(_mdat->name, "mdat", 5);
	_mdat->content_start = mdat->start;
	_mdat->file_begin = mdat->start;
	_mdat->file_end = _mdat->file.length();
	return _mdat;
}

BufferedAtom *Mp4::findMdat(std::string filename, Mp4::MdatStrategy strategy) {
	BufferedAtom *mdat = new BufferedAtom(filename);
	int64_t start = findMdat(mdat, strategy);
	if(start < 0) {
		delete mdat;
		return nullptr;
	}

	mdat->start = 0; //will be overwritten in repair.
	memcpy(mdat->name, "mdat", 5);

	mdat->content_start = start;
	mdat->file_begin = start;
	mdat->file_end = mdat->file.length();
	return mdat;
}

/* strategy:
 *
 * 1) Look for mdat. It's almost always a good start, but sometime, the actual packets can start from 8 to 200000k and more after.
 *     1.a) if non start guessable packets with fixed lenght are present we are blindly looking for them
 *     2.b) if we have guessable start make a map of possible starts and check with what we have found, in that case we need to lookup find mdat start with a different approach.
 * 2) if method 1 fails, we need to look for start guessable packets.
 *
 * Note: avc1 has size then start and for keyframes they are pretty guessable.
 */

int64_t Mp4::contentStart() {
	vector<int64_t> offsets;
	for(Track &track: tracks) {
		for(Track::Chunk &chunk: track.chunks) {
			offsets.push_back(chunk.offset);
			break;
		}
	}
	sort(offsets.begin(), offsets.end());
	return offsets[0];
}

int64_t Mp4::findMdat(BufferedAtom *mdat, Mp4::MdatStrategy strategy) {

	if(strategy == SAME)
		return contentStart();

	//look for mdat
	int64_t mdat_offset = -1;
	char m[4];
	m[3] = 0;
	//look for mdat in the first 20 MB
	int64_t length = std::min(int64_t(20000000), mdat->file_end - mdat->file_begin);
	uint8_t *data = mdat->getFragment(0, length);
	if(strategy == FIRST || strategy == LAST) {
		for(uint64_t i = 4; i < length-4; i++) {
			uint32_t c = readBE<uint32_t>(data + i);
			if(c != 0x6D646174) //mdat
				continue;

			mdat_offset = i+4;

			//check if its 64bit mdat
			uint32_t size = readBE<uint32_t>(data + i -4);
			if(size == 1)
				mdat_offset += 8;
			//sometimes the length is not  specified and still the first packet starts at +8 (see in repair)
			if(strategy == FIRST)
				break;
		}



	} else if(strategy == SEARCH) {
		//TODO if we have some unique beginnigs, try to spot the first one.
		for(uint64_t i = 4; i < length-4; i++) {
			uint32_t c = readBE<uint32_t>(data + i);
			if(c == 0) continue;

			for(Track &track: tracks) {
				//might want to look for video keyframes and actually skip the size of the frame (which is useless).
				if(track.codec.stats.beginnings32.count(c)) {
					mdat_offset = i;
					break;
				}
			}
			if(mdat_offset != -1)
				break;
		}
	}

	mdat->flush();

	if(mdat_offset != -1) {
		mdat->start = mdat_offset - 8;
		mdat->content_start = mdat_offset;

	}

	Log::info << "Mdat not found!" << endl;
	return mdat_offset;
}


//skip vast tract of zeros (up to the last one.
//multiple of 1024. If it's all zeros skip.
//if it's less than 8 bytes dont (might be alac?)
//otherwise we need to search for the actual begin using matches.
int zeroskip(BufferedAtom *mdat, unsigned char *start, int64_t maxlength) {
	int64_t block_size = std::min(int64_t(1<<10), maxlength);

	//skip 4 bytes at a time.
	int k = 0;
	for(;k < block_size - 4; k+= 4) {
		int value = readBE<int>(start + k);
		if(value != 0)
			break;
	}

	//don't skip very short zero sequences
	if(k < 16)
		return 0;



	//play conservative of non aligned zero blocks
	if(k < block_size)
		k -= 4;

	//zero bytes block aligned.

	Log::debug << "Skipping zero bytes: " << k << "\n";
	return k;
}

int Mp4::searchNext(BufferedAtom *mdat, int64_t offset, int maxskip) {
	int64_t maxlength64 = mdat->contentSize() - offset;
	if(maxlength64 > MaxFrameLength)
		maxlength64 = MaxFrameLength;
	unsigned char *start = mdat->getFragment(offset, maxlength64);
	int maxlength = static_cast<int>(maxlength64);
	Match best;
	best.chances = 0;
	best.offset = 0;
	for(Track &track: tracks) {
		Match m = track.codec.search(start, maxlength, maxskip);
		if(m.chances != 0 &&
		   (best.chances == 0 ||
			m.chances > best.chances ||
			m.offset < best.offset))
			best = m;
	}
	return best.offset;
}

/* Entropy could be used to detect wrong sowt packets. */
double entropy(uint8_t *data, int size) {
	int count[256];
	memset(count, 0, 256*sizeof(int));
	for(int i = 0; i < size; i++)
		count[data[i]]++;
	double e = 0.0;
	double log2 = log(2.0);
	for(int i = 0; i < 256; i++) {
		if(count[i] == 0)
			continue;
		double p = count[i]/(double)size;
		e -= p*log(p)/log2;
	}
	return e;
}


bool Mp4::repair(string corrupt_filename, Mp4::MdatStrategy strategy, int64_t mdat_begin, bool skip_zeros, bool drifting) {
	Log::info << "Repair: " << corrupt_filename << '\n';
	BufferedAtom *mdat = NULL;
	File file;
	if(!file.open(corrupt_filename))
		throw "Could not open file: " + corrupt_filename;

	if(0) {  // Parse corrupt file.


		// Find mdat.  This fails with krois and a few other.
		// TODO: Check for multiple mdat, or just look for the first one.
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

			mdat = new BufferedAtom(corrupt_filename);
			mdat->start = atom.start;
			memcpy(mdat->name, atom.name, sizeof(mdat->name)-1);
			memcpy(mdat->head, atom.head, sizeof(mdat->head));
			memcpy(mdat->version, atom.version, sizeof(mdat->version));

			mdat->file_begin = file.pos();
			mdat->file_end   = file.length() - file.pos();

			Log::debug << "MDAT SIZE: " << mdat->file_end - mdat->file_begin << endl;			//mdat->content = file.read(file.length() - file.pos());
			break;
		}
	} else {
		mdat = new BufferedAtom(corrupt_filename);

		int64_t mdat_offset = 0;
		if(mdat_begin >= 0)
			mdat_offset = mdat_begin;
		else
			mdat_offset = findMdat(mdat, strategy);

		if(mdat_offset < 0) {
			Log::debug << "Failed finding start" << endl;
			return false;
		}

		mdat->start = mdat_offset;
		mdat->content_start = mdat_offset;
		memcpy(mdat->name, "mdat", 5);
		//	memcpy(mdat->head, atom.head, sizeof(mdat->head));
		//memcpy(mdat->version, atom.version, sizeof(mdat->version));gedit
		file.seek(mdat_offset);
		mdat->file_begin = file.pos();
		mdat->file_end   = file.length();
	}

	Log::debug << "Mdat start: " << mdat->file_begin << " end: " << mdat->file_end << endl;
	for(unsigned int i = 0; i < tracks.size(); ++i)
		tracks[i].clear();


	// mp4a can be decoded and reports the number of samples (duration in samplerate scale).
	// In some videos the duration (stts) can be variable and we can rebuild them using these values.
	vector<int> audiotimes;
	//TODO: if it fails, try again with the same offset as the good one.
	//carefu the offset is relative to mdat, use the same absolute position.
	int64_t offset = 0;


//GOPRO TMCD is a single packet with a timestamp

	bool haspcm = false;
	int tmcd_id = -1;
	for(unsigned int i = 0; i < tracks.size(); ++i) {
		Track &track = tracks[i];
		if(string(track.codec.name) == "tmcd") {
			tmcd_id = i;
			track.codec.tmcd_seen = false; //tmcd happens only once, but if we try twice we need to reset it.
		}
		if(track.codec.pcm)
			haspcm = true;


	}


	std::vector<MatchGroup> matches;


	int percent = 0;
	//keep track of how many backtraced.
	int backtracked = 0;
	while(offset <  mdat->contentSize()) {
		int p = 100*offset / mdat->contentSize();
		if(p > percent) {
			percent = p;
			Log::info << "Processed: " << percent << "%\n";
		}
		int64_t maxlength64 = mdat->contentSize() - offset;
		if(maxlength64 > MaxFrameLength)
			maxlength64 = MaxFrameLength;
		unsigned char *start = mdat->getFragment(offset, maxlength64);
		int maxlength = static_cast<int>(maxlength64);



		unsigned int begin = readBE<int>(start); //mdat->readInt(offset);
		unsigned int next  = readBE<int>(start + 4);//mdat->readInt(offset + 4);

		Log::debug << "Offset: " << setw(10) << (mdat->file_begin + offset)
					   << "  begin: " << hex << setw(8) << begin << ' ' << setw(8) << next << dec << '\n';


		//zeros in pcm are possible, if mixed with zero padding it becames impossible to correctly detect the start.
		if(begin == 0 && !haspcm) {

			int skipped = zeroskip(mdat, start, maxlength64);
/*			if(skipped) {
				Log::debug << "Skipping " << skipped << " zeroes!" << endl;
				offset += skipped;
				continue;
			} */

			cout << (offset + skipped + mdat->file_begin) << " vs: " << (offset + skipped + mdat->file_begin) % 256 << endl;
			while((offset + skipped + mdat->file_begin) % 256)
				skipped++;
			Log::debug << "Skipped: " << skipped << endl;
			offset += skipped;
			continue;

		}



		Log::flush();
		//skip internal atoms
		if(!strncmp("moov", (char *)start + 4, 4) ||
			!strncmp("free", (char *)start + 4, 4) ||
			!strncmp("hoov", (char *)start + 4, 4) ||
			!strncmp("moof", (char *)start + 4, 4) ||
			!strncmp("wide", (char *)start + 4, 4)) {

			Log::debug << "Skipping containers for all the meta-data atom (moov, free, wide): begin: 0x"
					   << hex << begin << dec << ".\n";
			offset += begin;
			continue;
		}

		if(!strncmp("mdat", (char *)start + 4, 4)) {
			Log::debug << "Mdat encoutered, skipping header\n";
			offset += 8;
			continue;
		}

		//skip RTP:
		if(begin && 0xff00ffff) { // && readBE<uint16_t>(start);
			if((mdat->file_begin + offset) == 129770)
				Log::debug << "RTP test\n";
			//testing up to
			Match match = Codec::rtpMatch(start, maxlength);
			if(match.chances) {
				Log::debug << "Rtp packets. Lenght: " << match.length << "\n";
				offset += match.length;
				continue;
			}
		}

		//skip MISOUDAT: seen in a free container, but repeated just after outside of the free.
		if(!strncmp("MISOUDAT", (char *)start, 8)) {
			offset += 72;
			continue;
		}

		//new strategy: try to match all tracks.
		//each codec can:
		//matchable match with some probability.
		//know length

		//if more than one non matchable and not searchable we die.


		//if we have >1 possible match try best prob.
		//if no match assume is a non matchable for wich we know length.
		//search for a beginning
		//backtrace otherwise

		if(offset + mdat->content_start == 52087808)
			cout << "AHGH" << endl;

		MatchGroup group = match(offset, mdat);

		Match &best = group.back();

		/*MatchGroup group;
		group.offset = offset;
		for(unsigned int i = 0; i < tracks.size(); ++i) {
			Track &track = tracks[i];

			Match m = track.codec.match(start, maxlength);
			m.id = i;
			group.push_back(m);
			//if(track.codec.pcm)
			//	step = track.codec.pcm_bytes_per_sample;
		}
		sort(group.begin(), group.end());

		Match &best = group.back(); */
		//no hope!
		if(best.chances == 0.0f) {

			if(offset == 0) { //failed on first packet, maybe mdat starts at + 8
				offset = 8;
				continue;
			}

			//maybe the lenght is almost correct just a few padding bytes.
			int next = searchNext(mdat, offset, 16);
			if(next  < 16 && next != 0) {
				offset += next;
				continue;
			}
			Log::flush();
			//can we backtrack? if not, we are done


			//we are at the end!
			if(mdat->contentSize() -offset < 65000) {
				Log::debug << "We are at the end of the file: " << offset << endl;
				break;
			}
			//look for a different candidate in the past matches
			while(backtracked < 7) {

				if(matches.size() == 0) {
					backtracked = 7;
					break;
				}

				backtracked++;

				MatchGroup &last = matches.back();
				if(best.id == tmcd_id)
					tracks[best.id].codec.tmcd_seen = false;

				//last packet found in previous group wasn't good.
				last.pop_back();

				if(last.size() == 0) {
					//we need to go to the previous group
					matches.pop_back();
					continue;
				}
				Match &candidate = last.back();
				if(candidate.chances > 0.0f && candidate.length > 0 ) {
					if(candidate.id == tmcd_id)
						tracks[candidate.id].codec.tmcd_seen = true;
					offset = last.offset + candidate.length;
					break;
				}
				//no luck either, try another one looping
			}
			if(backtracked >= 7) {
				Log::error << "Backtracked enough!" << endl;
				break;
			}

			//we changed the offset lets restart.
			continue;
		}
		backtracked = 0;

		if(mdat->file_begin + offset + best.length > mdat->file_end)
			break;

		if(best.length) {
			Log::debug << "Matched track: " << best.id << " length: " << best.length << "\n";
			offset += best.length;

		} else {

			best.length = searchNext(mdat, offset, 8192);
			Log::debug << "Unknown length, search for next beginning, guessed as: " << best.length << endl;
			if(!best.length) {
				Log::error << "Could not guess length of best match" << endl;
				//throw "Can't guess the length of any packet.";
				break;
			}
			offset += best.length;
			//This should only happen with pcm codecs, and here we should search for the beginning of another codec
		}
		if(best.id == tmcd_id)
			tracks[best.id].codec.tmcd_seen = true; //id in tracks start from 1.
		matches.push_back(group);
	}

	if(matches.size() < 4) //to few packets.
		return false;


/* multiple audio in stream, we expect to have 1 video n audio1 and n audio2 then 1 video again,
 * where n can vary, we find out how big n is looking for the next video packet */
//#define DOUBLEAUDIO_ALTERNATE
#ifdef DOUBLEAUDIO_ALTERNATE


	int start = -1;
	for(int i = 0; i < matches.size(); i++) {
		MatchGroup &g = matches[i];
		Match &m = g.back();
		if(m.id != 0 && start == -1)
			start = i;
		if(m.id == 0) {


			if(start != -1) {
				int tot_audio = i - start;
				for(int k = start; k < start + tot_audio/2; k++) {
					assert(matches[k].back().id != 0);
					matches[k].back().id = 1;
				}
				for(int k = start + tot_audio/2; k < start + tot_audio; k++) {
					assert(matches[k].back().id != 0);
					matches[k].back().id = 2;
				}
			}
			start = -1;
		}

	}
#endif

/*Here we expect v a1 a2 a1 a2 v a1 a2 v a1 a2 a1 a2 a1 a2 v
 * audio just alternate.
 * we also assume track 0 is video audio 1 and audio 2 (to be fixed) */

//#define DOUBLEAUDIO_INTERLEAVED
#ifdef DOUBLEAUDIO_INTERLEAVED

	bool first = true;
	for(int i = 0; i < matches.size(); i++) {
		MatchGroup &g = matches[i];
		Match &m = g.back();
		if(m.id == 0)
			continue;
		first ? m.id = 1 : m.id = 2;
		first = !first;
	}
#endif


//this time 4 tracks interleaved and video is first track
//#define QUADAUDIO
#ifdef QUADAUDIO


		int start = -1;
		for(int i = 0; i < matches.size(); i++) {
			MatchGroup &g = matches[i];
			Match &m = g.back();
			if(m.id != 0 && start == -1)
				start = i;
			if(m.id == 0) {

				if(start != -1) {
					int tot_audio = i - start;
					for(int k = 0; k < tot_audio; k++) {
						assert(matches[k + start].back().id != 0);
						matches[k+start].back().id = (k%4)+1;
					}
				}
				start = -1;
			}

		}
#endif


	//copy matches into tracks
	int count = 0;
	double drift = 0; //difference in times between audio and video.
	double audio_current = 0;
	double video_current = 0;
	for(MatchGroup &group: matches) {
		assert(group.size() > 0);
		Match &match = group.back();

		Track &track = tracks[match.id];
		if(match.keyframe)
			track.keyframes.push_back(track.offsets.size());

		track.offsets.push_back(group.offset);
		if(track.default_size) {
			//if number of samples per chunk is variable, encode each sample in a different chunk.
			if(track.default_chunk_nsamples == 0) {
				if(track.codec.pcm)
					track.nsamples += match.length/track.codec.pcm_bytes_per_sample;
				else //probably not audio or video, no way to guess time drifting.
					track.nsamples += track.chunks[0].nsamples;
			} else
				track.nsamples += track.default_chunk_nsamples;
		} else {
			//might be wrong for variable number of samples per chunk.
			track.nsamples++; //we just hope we get 1 sample
			track.sample_sizes.push_back(match.length);
		}
		track.chunk_sizes.push_back(match.length);


		if(match.duration)
			audiotimes.push_back(match.duration);

		//check timing drifting
		double t = track.default_time || track.times.size() == 0 ? track.default_time : track.times[count% track.times.size()];
		if(track.type == string("vide")) {
			video_current += t*timescale / track.timescale;
		} else if(track.type == string("soun")) {
			audio_current += t*timescale / track.timescale;
		}
		drift = audio_current - video_current;
		//Log::debug << "Drift audio - video: " << drift << "\n";
		count++;
	}
	if(drifting) {
		fixTiming();


	}
	//move drifting fix into som other function.
	if(0 && drifting && audio_current > 0 && video_current > 0 && (drift > timescale || drift < -timescale)) { //drifting of packets for more than 1 seconds
		Log::debug << "Drift audio - video: " << drift << ". Fixing\n";
		//fix video
		for(Track &track: tracks) {
			if(track.type != string("vide"))
				continue;
			//convert back drift to track timescale
			drift = track.timescale * drift /timescale;
			int64_t per_sample = int64_t(drift /(double)track.offsets.size());
			if(track.default_time)
				track.default_time += per_sample;
			else {
				if(per_sample > track.times[0]) {
					Log::debug << "Something got really wrong with the timing...\n";
					continue;
				}
				for(auto &t: track.times)
					t -= per_sample;
			}
		}
	}

	mdat->file_end = mdat->file_begin + offset;
	mdat->length   = mdat->file_end - mdat->file_begin;

	Log::info << "Found " << matches.size() << " packets.\n";

	for(unsigned int i = 0; i < tracks.size(); ++i) {
		Log::info << "Found " << tracks[i].offsets.size() << " chunks for " << tracks[i].codec.name << endl;
		if(audiotimes.size() == tracks[i].offsets.size())
			swap(audiotimes, tracks[i].times);
		if(tracks[i].offsets.size())
			tracks[i].fixTimes();
	}

	Atom *original_mdat = root->atomByName("mdat");
	if(!original_mdat) {
		Log::error << "Missing 'Media Data container' atom (mdat).\n";
		delete mdat;
		return false;
	}
	mdat->start = original_mdat->start;
	Log::debug << "Replacing 'Media Data content' atom (mdat).\n";

	root->replace(original_mdat, mdat);
	//original_mdat->content.swap(mdat->content);
	//original_mdat->start = -8;
	delete original_mdat;

	return true;
}
void Mp4::fixTiming() {
	int leading_track =0;
	double leading_variance = 1e20;
	bool need_fixing = false;
	vector<ChunkTime> chunktimes;
	for(int i = 0; i < tracks.size(); i++) {
		Track &track = tracks[i];
		double variance = track.codec.stats.variance;
		need_fixing |= variance > 0;

		int sample = 0;
		for(Track::Chunk &chunk: track.chunks) {

			ChunkTime chunktime;
			chunktime.offset = chunk.offset;
			chunktime.nsamples = chunk.nsamples; //should always be 1
			chunktime.size = chunk.size;
			chunktime.track = i;
			chunktime.sample_time = 0;


			for(int k = 0; k < chunk.nsamples; k++) {
				chunktime.sample = sample;
				chunktime.sample_time += track.default_time ? track.default_time : (int)track.codec.stats.average_time;
				chunktime.offset++; //just to keep them ordered.
				chunktimes.push_back(chunktime);
				sample++;
			}
		}
		if((track.type == string("vide") || track.type == string("soun")) &&
			(track.default_time || variance < leading_variance)) {
			leading_track = i;
			leading_variance = variance;
		}
	}
	if(!need_fixing)
		return;

	//sort all packages by position in mdat.
	sort(chunktimes.begin(), chunktimes.end());
	std::vector<double> current_time(tracks.size());
	for(ChunkTime &timing: chunktimes) {
		Track &track = tracks[timing.track];

		if(!track.default_time) {
			double delay = (current_time[timing.track] - current_time[leading_track]);
			int corrected = timing.sample_time;
			if(delay*track.timescale > 4*track.codec.stats.average_time) {
				corrected = track.codec.stats.average_time/2;
			} else if(delay*track.timescale < -4*track.codec.stats.average_time) {
				corrected = track.codec.stats.average_time*2;
			}
			timing.sample_time = track.times[timing.sample] = corrected;
		}
		current_time[timing.track] += timing.sample_time/(double)track.timescale;
	}

	return;


	//check if some track has variable timing and some has fixed timing,
	int best_fixed = -1;
	int best_fixed_size = 0;
	std::set<int> variables;
	for(int t = 0; t < tracks.size(); t++) {
		if(tracks[t].default_time == 0)
			variables.insert(t);
		else if(best_fixed == -1 || best_fixed_size < tracks[t].chunks.size()) {
			best_fixed = t;
			best_fixed_size = tracks[t].chunks.size();
		}
	}
	if(best_fixed != -1 && variables.size()) {
		//1: inspect working video for packet timing variability and max out of sync.
		//2:keep the two stream as much in sync as possible withing variability.

		//create index of offset and timings for fixed one
		Track &fixed = tracks[best_fixed];

		for(int t: variables) {
			Track &track = tracks[t];

			if(!track.chunks.size())  //check if there is something to fix.
				continue;

			track.times.clear();

			double average_time = 0.0; //used when no other indications.
			for(int i = 0; i < track.times[i]; i++)
				average_time += track.times[i];
			average_time /= track.times.size();

			//variable gets before first fixed packet or after?
			bool before = track.chunks[0].offset < fixed.chunks[0].offset;


			int fix = 0; //current fixed chunk
			int var = 0; //current variable chunk
			int start_var = 0; //where the group of variable is.
			int nsamples = 0;
			int fixed_time = 0;  //time elapsed in the fixed group
			int previous_fixed_time = 0;
			//this is good for after.
			int status = before? 0 : 1; //0 advance track, 1 advance fixed, 2 start track 3 start fixed
			while(1) {
				if(status == 0) {
					cout << "Status 0, var: " << var << endl;
					if(var < track.chunks.size() && track.chunks[var].offset < fixed.chunks[fix].offset) {
						nsamples += track.chunks[var].nsamples;
						var++;
					} else { //var new group (or past the last)
						status = 1;
					}
				} else if(status == 2) {
					cout << "Status 2, var: " << var << endl;

					int time = (before ? fixed_time : previous_fixed_time) / nsamples;
					time = time*track.timescale / fixed.timescale;
					for(int i = start_var; i < var; i++) {
						for(int k = 0; k < nsamples; k++)
							track.times.push_back(time);
					}
					start_var = var;
					nsamples = 0;
					status = 0;
					previous_fixed_time = fixed_time;
					fixed_time = 0;
					if(var == track.chunks.size())
						break;
				} else if(status == 1) {
					cout << "Status 1, var: " << var << endl;

					if((var >= track.chunks.size() || fixed.chunks[fix].offset < track.chunks[var].offset) && fix < fixed.chunks.size()) {
						fixed_time += fixed.chunks[fix].nsamples*fixed.default_time;
						fix++;
					} else {
						status = 2
								;
					}
				}
			}
		}

	}
}

// vim:set ts=4 sw=4 sts=4 noet:
