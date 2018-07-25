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

#include "mp4.h"

#include <iostream>
#include <string>
#include <cctype>
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
        LOG_QUIET = 0,  // Print no output.
        LOG_PANIC,      // Something went really wrong and we will crash now.
        LOG_FATAL,      // Something went wrong and recovery is not possible.
        LOG_ERROR,      // Something went wrong and cannot losslessly be recovered.
        LOG_WARNING,    // Something somehow does not look correct.
        LOG_INFO,       // Standard information.
        LOG_VERBOSE,    // Detailed information.
        LOG_DEBUG,      // Stuff which is only useful for developers.
        LOG_TRACE       // Extremely verbose debugging, useful for development.
    };
    LogLevel next(LogLevel level, int n = 1) { return LogLevel(level + n); }
    LogLevel prev(LogLevel level, int n = 1) { return LogLevel(level - n); }
    LogLevel clmp(LogLevel level) { return (level < LOG_QUIET) ? LOG_QUIET : (LOG_TRACE < level) ? LOG_TRACE : level; }

    const char* logName(LogLevel level) {
        const char* const LogNames[] = {
            "quiet", "panic", "fatal", "error", "warning", "info", "verbose", "debug", "trace"
        };
        return LogNames[clmp(level)];
    }

    LogLevel getLibavLogLevel() {
        return clmp(LogLevel((av_log_get_level() - AV_LOG_QUIET) / (AV_LOG_WARNING - AV_LOG_ERROR)));
    }
    void setLibavLogLevel(LogLevel level) {
        av_log_set_level(((AV_LOG_WARNING - AV_LOG_ERROR) * clmp(level)) + AV_LOG_QUIET);
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
                "  -q, --log=quiet  Be quiet and don't log at all.\n"
                "  -v, --log        Increase logging verbosity (use multiple times).\n"
                "  -v<n>            Set logging verbosity to <n> in range [0..8].\n"
                "  --log=quiet      Print no output.\n"
                "  --log=panic      Something went really wrong and we will crash now.\n"
                "  --log=fatal      Something went wrong and recovery is not possible.\n"
                "  --log=error      Something went wrong and cannot losslessly be recovered.\n"
                "  --log=warning    Something somehow does not look correct.\n"
                "  --log=info       Standard information.\n"
                "  --log=verbose    Detailed information.\n"
                "  --log=debug      Stuff which is only useful for developers.\n"
                "  --log=trace      Extremely verbose debugging, useful for development.\n"
                "\n";
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
                case 'h': help    = true;       break;
                case 'i': info    = true;       break;
                case 'a': analyze = true;       break;
                case 'q': loglvl  = LOG_QUIET;  break;
                case 'v': loglvl  = next(loglvl);
                          if(i+1 < arg.size() && isdigit(arg[i+1]))
                              loglvl = clmp(LogLevel(arg[++i] - '0'));
                          break;
                default:  invalid = true;       break;
                }
            }
        } else {
            // Long option format.
            if     (arg == "--help")        help    = true;
            else if(arg == "--info")        info    = true;
            else if(arg == "--analyze")     analyze = true;
            else if(arg == "--log")         loglvl  = next(loglvl);
            else if(arg == "--log=quiet")   loglvl  = LOG_QUIET;
            else if(arg == "--log=panic")   loglvl  = LOG_PANIC;
            else if(arg == "--log=fatal")   loglvl  = LOG_FATAL;
            else if(arg == "--log=error")   loglvl  = LOG_ERROR;
            else if(arg == "--log=warning") loglvl  = LOG_WARNING;
            else if(arg == "--log=info")    loglvl  = LOG_INFO;
            else if(arg == "--log=verbose") loglvl  = LOG_VERBOSE;
            else if(arg == "--log=debug")   loglvl  = LOG_DEBUG;
            else if(arg == "--log=trace")   loglvl  = LOG_TRACE;
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
