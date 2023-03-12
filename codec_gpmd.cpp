#include "codec.h"

#include <set>
#include <iostream>
using namespace std;
Match Codec::gpmdMatch(const unsigned char *start, int maxlength) {
	Match match;

	set<string> fourcc =  {"DEVC", "DVID", "DVNM", "STRM", "STNM", "RMRK",  "SCAL",
							 "SIUN", "UNIT", "TYPE", "TSMP", "TIMO", "EMPT"};
	if(!fourcc.count(string((char *)start, 4)))
		return match;

	match.chances = 1<<20;
	match.length = (readBE<int32_t>(start + 4) & 0xffff) + 8;
	return match;
}

Match Codec::gpmdSearch(const unsigned char *start, int maxlength, int maxskip) {
	Match match;
	const unsigned char *end = start + maxskip;
	const  unsigned char *current = start;

	set<string> fourcc =  {"DEVC", "DVID", "DVNM", "STRM", "STNM", "RMRK",  "SCAL",
							 "SIUN", "UNIT", "TYPE", "TSMP", "TIMO", "EMPT"};

	while(current < end) {
		if(fourcc.count(string((char *)current, 4))) {
			match.chances = 1<<20;
			match.offset = current - start;
			return match;
		}
		current++;
	}
	return match;
}

