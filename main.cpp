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

#include "mp4.h"
#include "atom.h"
#include "log.h"

#include <cstdlib>
#include <iostream>
#include <string>
using namespace std;

void usage() {
	cout << "Usage: untrunc [-aisdetmMbNvwqeo] <ok.mp4> [<corrupt.mp4>]\n\n"
		 << "	-o: output filename (if repairing)\n"
		 << "	-i: info about codecs and mov structure\n"
		 << "	-a: test the ok video\n"
		 << "	-s: simulate recovering the ok video for debug purposes\n"
		 << "	-t: analyze track\n"
		 << "	-m: use the same offset for mdat beginning\n"
		 << "	-M: search for probable packet starts for mdat\n"
		 << "	-b: specify initial byte for mdat content\n"
		 << "	-N: don't skip zeros. (useful for pcm audio)"
		 << "	-d: attepmt to fix audio/video drifting"
		 << "	-q: silent\n"
		 << "	-e: error\n"
		 << "	-v; verbose\n"
		 << "	-w: debug info\n\n";
}

void searchFile(std::string ok, std::vector<uint8_t> search) {
	FILE * fp = fopen(ok.c_str(), "r");
	if(!fp) {
		cerr << "Could not open file: " << ok << endl;
		return;
	}
	int batch = 1<<20;
	uint8_t buffer[batch];
	size_t pos = 0;
	while(1) {
		int readed = fread(buffer, 1, batch, fp);
		if(readed <= search.size()) break;
		for(int k = 0; k < readed - search.size(); k++) {
			int i = 0;
			while(i < search.size()) {
				if(search[i] != buffer[k+i])
					break;
				i++;
			}
			if(i == search.size()) {
				cout << "Found at:" << pos + k << endl;
			}
		}
		pos += readed - search.size();
		fseek(fp, - search.size(), SEEK_CUR);
	}
}


int toHex(char s) {
	if(s >= 'a') return 10 + (s - 'a');
	if(s >= 'A') return 10 + (s - 'A');
	return s - '0';
}
std::vector<uint8_t> hexToStr(const char *str) {
	std::vector<uint8_t> s;
	while(*str != 0) {
		uint8_t c = 16*toHex(*str);
		str++;
		if(*str == 0) break;
		c += toHex(*str);
		s.push_back(c);
		str++;
	}
	return s;
}

int main(int argc, char *argv[]) {

	std::string output_filename;
	bool info = false;
	bool analyze = false;
	bool simulate = false;
	int analyze_track = -1;
	bool drifting = false;
	Mp4::MdatStrategy mdat_strategy = Mp4::FIRST;
	//bool same_mdat_start = false; //if mdat can be found or starting of packets try using the same absolute offset.
	//bool ignore_mdat_start = false; //ignore mdat string and look for first recognizable packet.
	bool skip_zeros = true;
	int64_t mdat_begin = -1; //start of packets if specified.
	int i = 1;
	std::vector<uint8_t> search;
	for(; i < argc; i++) {
		string arg(argv[i]);
		if(arg[0] == '-') {
			switch(arg[1]) {
			case 'o': output_filename = std::string(argv[i+1]); i++; break;
			case 'i': info = true; break;
			case 'a': analyze = true; break;
			case 's': simulate = true; break;
			case 'd': drifting = true; break;
			case 't': analyze_track = atoi(argv[i+1]); i++; break;
			case 'q': Logger::log_level = Logger::SILENT; break;
			case 'e': Logger::log_level = Logger::ERROR; break;
			case 'v': Logger::log_level = Logger::INFO; break;
			case 'w': Logger::log_level = Logger::DEBUG; break;
			case 'm': mdat_strategy = Mp4::SAME; break;
			case 'M': mdat_strategy = Mp4::SEARCH; break;
			case 'b': mdat_strategy = Mp4::SPECIFIED; mdat_begin = atoi(argv[i+1]); i++; break;
			case 'B': skip_zeros = false; break;
			case 'S': search = hexToStr(argv[i+1]); i++; break;
			}
		} else
			break;
	}
	if(argc == i) {
		usage();
		return -1;
	}

	string ok = argv[i];
	if(search.size()) {
		searchFile(ok, search);
		return 0;
	}

	string corrupt;
	i++;
	if(i < argc)
		corrupt = argv[i];

	Log::info << "Reading: " << ok << endl;
	Mp4 mp4;

	try {
		mp4.open(ok);

		if(info) {
			mp4.printMediaInfo();
			mp4.printAtoms();
		}
		if(analyze) {
			mp4.analyze(analyze_track);
		}
		if(simulate)
			mp4.simulate(mdat_strategy, mdat_begin);

		if(corrupt.size()) {

			bool success = mp4.repair(corrupt, mdat_strategy, mdat_begin, skip_zeros, drifting);
			//if the user didn't specify the strategy, try them all.
			if(!success  && mdat_strategy == Mp4::FIRST) {
				vector<Mp4::MdatStrategy> strategies = { Mp4::SAME, Mp4::SEARCH, Mp4::LAST };
				for(Mp4::MdatStrategy strategy: strategies) {
					Log::info << "\n\nTrying a different approach to locate mdat start" << endl;
					success = mp4.repair(corrupt, strategy, mdat_begin, skip_zeros);
					if(success) break;
				}
			}
			if(!success) {
				Log::error << "Failed recovering the file\n";
			}

			size_t lastindex = corrupt.find_last_of(".");
			if(output_filename.size() == 0)
				output_filename = corrupt.substr(0, lastindex) + "_fixed.mp4";
			mp4.saveVideo(output_filename);
		}
	} catch(string e) {
		Log::error << e << endl;
		return -1;
	} catch(const char *e) {
		Log::error << e << endl;
		return -1;
	}
	return 0;
}
