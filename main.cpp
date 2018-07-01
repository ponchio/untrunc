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

using namespace std;


namespace {
    enum ExitStatus {
        STATUS_SUCCESS          = 0,
        STATUS_FAILURE          = 1,
        STATUS_INVALID_ARGUMENT = 2
    };

    void printUsage() {
        cerr << "Usage: untrunc [-h -i -a] <ok.mp4> [<corrupt.mp4>]\n\n";
    }

    void printHelp() {
        cout << "Untrunc\n"
                "Description:\n"
                "  Restores a damaged (truncated) mp4, m4v, mov, 3gp video.\n"
                "  Provided you have a similar not broken video. And some luck.\n"
                "Usage: untrunc [-h -i -a] <ok.mp4> [<corrupt.mp4>]\n"
                "  -h, --help    This help text.\n"
                "  -i, --info    Information on <ok.mp4>.\n"
                "  -a, --analyze Interactively analyze <ok.mp4> (press key to continue).\n"
                "\n";
    }
};


int main(int argc, const char *argv[]) {

    bool help    = false;
    bool info    = false;
    bool analyze = false;

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
                case 'h': help    = true; break;
                case 'i': info    = true; break;
                case 'a': analyze = true; break;
                default:  invalid = true; break;
                }
            }
        } else {
            // Long option format.
            if     (arg == "--help")    help    = true;
            else if(arg == "--info")    info    = true;
            else if(arg == "--analyze") analyze = true;
            else                        invalid = true;
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


    // Do the untrunc thing.
    if(help)
        printHelp();

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
