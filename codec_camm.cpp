#include "codec.h"

//https://developers.google.com/streetview/publish/camm-spec

//TODO could tell from the working sample which types are used.
#include <set>
#include <iostream>
using namespace std;
Match Codec::cammMatch(const unsigned char *start, int maxlength) {
	Match match;

	int lengths[] = { 12, 8, 12, 12, 12, 24, 14*4, 12 };
	while(start[0] == 0 && start[1] == 0 || start[3] == 0 && start[2] < 7) {
		int type = start[2];
		match.chances = 1<<20;
		int len = lengths[type] + 4;
		match.length += len;
		start += len;
		if(match.length > maxlength - 15*4)
			break;
	}
	return match;
}

Match Codec::cammSearch(const unsigned char *start, int maxlength, int maxskip) {
	Match match;
	const unsigned char *end = start + maxskip;
	const  unsigned char *current = start;


	while(current < end) {
		if(start[0] == 0 && start[1] == 0 || start[3] == 0 && start[2] < 7) {
			match.chances = 1<<20;
			match.offset = current - start;
			return match;
		}
		current++;
	}
	return match;
}

