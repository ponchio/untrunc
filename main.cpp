//==================================================================//
/*
	Untrunc - main.cpp

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

#include <cctype>
#include <string>
#include <iostream>
#include <iomanip>

#include "mp4.h"
extern "C" {
#include <libavutil/log.h>
}

using namespace std;


namespace {
	// Program exit status.
	enum ExitStatus {
		STATUS_SUCCESS          = 0,
		STATUS_FAILURE          = 1,
		STATUS_INVALID_ARGUMENT = 2
	};

	// Logging.
	enum LogLevel {
		//LOG_NONE,       // Print no output.
		//LOG_PANIC,      // Something went really wrong and we will crash now.
		//LOG_FATAL,      // Something went wrong and recovery is not possible.
		LOG_ERROR,      // Something went wrong and cannot losslessly be recovered.
		LOG_WARNING,    // Something somehow does not look correct.
		LOG_INFO,       // Standard information.
		//LOG_VERBOSE,    // Detailed information.
		LOG_DEBUG,      // Stuff which is only useful for developers.
		//LOG_TRACE,      // Extremely verbose debugging, useful for development.

		LOG_LEVEL_SIZE, // Number of log levels (used internallly).

		// Configuration only.
		LOG_QUIET   = LOG_ERROR, // Log level to use for "quiet" logging.
		LOG_DEFAULT = LOG_INFO   // Default log level.
	};

	LogLevel next(LogLevel level, int n = 1) { return LogLevel(level + n); }
	LogLevel prev(LogLevel level, int n = 1) { return LogLevel(level - n); }
	LogLevel clamp(LogLevel level) {
		return (level < LogLevel(0)) ? LogLevel(0) : (level < LOG_LEVEL_SIZE) ? level : LogLevel(LOG_LEVEL_SIZE-1);
	}

	// Log level name.
	const struct { LogLevel level; int av_level; const char *name; const char *descr;
	} LogInfo[LOG_LEVEL_SIZE + 2] = {
			//{ LOG_NONE,    AV_LOG_NONE,   "none",      "Show no output at all" },
			//{ LOG_PANIC,    AV_LOG_PANIC,   "panic",    "Something went really wrong and we will crash now" },
			//{ LOG_FATAL,    AV_LOG_FATAL,   "fatal",    "Something went wrong and recovery is not possible" },
			{ LOG_ERROR,    AV_LOG_ERROR,   "error",    "Something went wrong and cannot losslessly be recovered" },
			{ LOG_WARNING,  AV_LOG_WARNING, "warning",  "Something somehow does not look correct" },
			{ LOG_INFO,     AV_LOG_INFO,    "info",     "Standard information" },
			//{ LOG_VERBOSE,  AV_LOG_VERBOSE, "verbose",  "Detailed information" },
			{ LOG_DEBUG,    AV_LOG_DEBUG,   "debug",    "Stuff which is only useful for developers" },
			//{ LOG_TRACE,    AV_LOG_TRACE,   "trace",    "Extremely verbose debugging, useful for development" },

			// Configuration only.
			{ LOG_QUIET,    AV_LOG_QUIET,    "quiet",    "Log as little as needed" },
			{ LOG_DEFAULT,  AV_LOG_INFO,     "default",  "Use the default log level" }
	};

	const char* logName(LogLevel level) {
		if(LogInfo[0].level == 0 && LogInfo[LOG_LEVEL_SIZE - 1].level == LOG_LEVEL_SIZE - 1)
			return LogInfo[clamp(level)].name;
		else {
			level = clamp(level);
			for(unsigned int i = 0; i < sizeof(LogInfo)/sizeof(LogInfo[0]); ++i) {
				if(level == LogInfo[i].level)
					return LogInfo[i].name;
			}
			return "???";
		}
	}

	bool setLogLevel(LogLevel &level, char ch) {
		if(isdigit(ch)) {
			level = clamp(LogLevel(ch - '0'));
			return true;
		}
		return false;
	}

	bool setLogLevel(LogLevel &level, const string &name) {
		if(!name.empty()) {
			if (name.size() == 1 && setLogLevel(level, name[0]))
				return true;
			for(unsigned int i = 0; i < sizeof(LogInfo)/sizeof(LogInfo[0]); ++i) {
				if(name == LogInfo[i].name) {
					level = LogInfo[i].level;
					return true;
				}
			}
		}
		return false;
	}

	// AV library logging.
	LogLevel getLibavLogLevel() {
		int av_loglvl = av_log_get_level();
		for(unsigned int i = 0; i < sizeof(LogInfo)/sizeof(LogInfo[0]); ++i) {
			if(av_loglvl <= LogInfo[i].av_level)
				return LogInfo[i].level;
		}
		return LogLevel(LOG_LEVEL_SIZE - 1);
	}

	void setLibavLogLevel(LogLevel level) {
		if(LogInfo[0].level == 0 && LogInfo[LOG_LEVEL_SIZE - 1].level == LOG_LEVEL_SIZE - 1)
			return av_log_set_level(LogInfo[clamp(level)].av_level);
		else {
			level = clamp(level);
			for(unsigned int i = 0; i < sizeof(LogInfo)/sizeof(LogInfo[0]); ++i) {
				if(level == LogInfo[i].level) {
					av_log_set_level(LogInfo[i].av_level);
					return;
				}
			}
		}
	}

	// Command-line help.
	void printUsage() {
		cerr << "Usage: untrunc [-h -i -a -q -v] <ok.mp4> [<corrupt.mp4>]\n\n";
	}

	void printHelp() {
		LogLevel loglvl = getLibavLogLevel();
		// size "123456789+123456789+123456789+123456789+123456789+123456789+123456789+123456" = 76 chars
		cout << "\n"
				"Untrunc  -  Untruncate damaged video files\n"
				"\n"
				"Description:\n"
				"  Restores a damaged (truncated) mp4, m4v, mov, 3gp video.\n"
				"  Provided you have a similar not broken video. And some luck.\n"
				"\n"
				"  License: GPL-2.0-or-later (see: <https://www.gnu.org/licenses/>).\n" // SPDX Licence Id, https://spdx.org/licenses/
				"  Current log level: \"" << logName(loglvl) << "\" (" << loglvl << ").\n"
				"\n"
				"Usage: untrunc [-h -i -a -q -v] <ok.mp4> [<corrupt.mp4>]\n"
				"  -h, --help       This help text.\n"
				"  -i, --info       Information on <ok.mp4>.\n"
				"  -a, --analyze    Interactively analyze <ok.mp4> (waits for user input).\n"
				"  -q, --log=quiet  Be quiet and log as little as possible.\n"
				"  -v, --log        Increase logging verbosity (use multiple times).\n"
				"  -v<n>, --log=<n> Set logging verbosity to <n> in range [0..8].\n";
		for(unsigned int i = 0; i < sizeof(LogInfo)/sizeof(LogInfo[0]); ++i) {
			cout << "  --log=" << left << setw(18 - (sizeof("  --log=")-1)) << LogInfo[i].name
				<< ' ' << LogInfo[i].descr << ".\n";
		}
		cout << '\n';
		// size "123456789+123456789+123456789+123456789+123456789+123456789+123456789+123456" = 76 chars
	}
};


int main(int argc, const char *argv[]) {

	LogLevel loglvl  = getLibavLogLevel();
	bool     help    = false;
	bool     info    = false;
	bool     analyze = false;

	// Parse command-line arguments.
	int argidx = 1;
	for(; argidx < argc; argidx++) {
		string arg = argv[argidx];
		if(arg.size() < 2 || arg[0] != '-')
			break;      // Not an option.
		if(arg == "--")
			break;      // End-of-Options.
		bool invalid = false;
		if(arg[1] != '-') {
			// Short option format.
			for(unsigned i = 1; i < arg.size(); i++) {
				switch (arg[i]) {
				case 'h':	help    = true;       break;
				case 'i':	info    = true;       break;
				case 'a':	analyze = true;       break;
				case 'q':	loglvl  = LOG_QUIET;  break;
				case 'v':	loglvl  = next(loglvl);
							if(i+1 < arg.size() && setLogLevel(loglvl, arg[i+1]))
								++i;
							break;
				default:	invalid = true;       break;
				}
			}
		} else {
			// Long option format.
			if     (arg == "--help")        help    = true;
			else if(arg == "--info")        info    = true;
			else if(arg == "--analyze")     analyze = true;
			else if(arg == "--log")         loglvl  = next(loglvl);
			else if(arg.compare(0, sizeof("--log=")-1, "--log=") == 0
					&& setLogLevel(loglvl, arg.substr(sizeof("--log=")-1)))
				; // Level has already been set.
			else                            invalid = true;
		}
		if(invalid) {
			cerr << "Error: Invalid option '" << arg << "' in argument " << argidx << '\n';
			printUsage();
			return STATUS_INVALID_ARGUMENT;
		}
	}
	if(argidx >= argc) {
		bool invalid = !help | info | analyze;
		if(invalid) {
			cerr << "Error: Missing argument " << argc << '\n';
			printUsage();
			return STATUS_INVALID_ARGUMENT;
		}
		printHelp();
		return STATUS_SUCCESS;
	}
	// Get file argument(s).
	string ok      = argv[argidx++];
	string corrupt = (argidx < argc)? argv[argidx++] : "";
	if(argidx < argc) {
		cerr << "Error: Extra argument " << argidx << ((argc - argidx > 1)? "...":"") << '\n';
		printUsage();
		return STATUS_INVALID_ARGUMENT;
	}

	// Set log level.
	if(loglvl >= LOG_INFO) {
		LogLevel old_loglvl  = getLibavLogLevel();
		clog << "Log Level: \"";
		if(loglvl != old_loglvl)
			clog << logName(old_loglvl) << "\" (" << old_loglvl << ") -> \"";
		clog << logName(loglvl) << "\" (" << loglvl << ')' << endl;
	}
	setLibavLogLevel(loglvl);

	// Show help.
	if(help)
		printHelp();

	// Do the untrunc thing.
	cout << "Reading: " << ok << endl;
	Mp4 mp4;

	try {
		mp4.open(ok);
		if(info) {
			mp4.printMediaInfo();
			mp4.printAtoms();
		}
		if(analyze) {
			mp4.analyze();
		}
		if(!corrupt.empty()) {
			cout << "Reading: " << corrupt << endl;
			mp4.repair(corrupt);
			mp4.saveVideo(corrupt + "_fixed.mp4");
		}
	} catch(string e) {
		cerr << "Error: " << e << endl;
		return STATUS_FAILURE;
	}
	return STATUS_SUCCESS;
}

// vim:set ts=4 sw=4 sts=4 noet:
