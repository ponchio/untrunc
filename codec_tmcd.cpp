#include "codec.h"

/* https://developer.apple.com/library/archive/documentation/QuickTime/QTFF/QTFFChap3/qtff3.html
 *
Timecode Sample Descritpion

Reserved
A 32-bit integer that is reserved for future use. Set this field to 0.

Flags
A 32-bit integer containing flags that identify some timecode characteristics.
The following flags are defined.

Drop frame
Indicates whether the timecode is drop frame. Set it to 1 if the timecode is drop frame.
This flag’s value is 0x0001.

24 hour max
Indicates whether the timecode wraps after 24 hours. Set it to 1 if the timecode wraps.
This flag’s value is 0x0002.

Negative times OK
Indicates whether negative time values are allowed. Set it to 1 if the timecode supports negative values.
This flag’s value is 0x0004.

Counter
Indicates whether the time value corresponds to a tape counter value. Set it to 1 if the timecode values are tape counter values.
This flag’s value is 0x0008.

Time scale
A 32-bit integer that specifies the time scale for interpreting the frame duration field.

Frame duration
A 32-bit integer that indicates how long each frame lasts in real time.

Number of frames
An 8-bit integer that contains the number of frames per second for the timecode format.
If the time is a counter, this is the number of frames for each counter tick.

Reserved
An 8-bit quantity that must be set to 0.

Source reference
A user data atom containing information about the source tape. The only currently used user data list entry is the 'name' type.
This entry contains a text item specifying the name of the source tape.

*/


#include <iostream>
using namespace std;
Match Codec::tmcdMatch(const unsigned char *start, int maxlength) {

	Match match;

//tmcd is problematic.. usually is at the beginning, it's just easier to move mdat begin later
//	if(tmcd_seen) 
		return match;

	unsigned int timestamp = readBE<unsigned int>(start);

	if( timestamp > 2592000)
		match.chances = 0;
	else {
		match.length = 4;
		match.chances = 1024;
	}
	return match;

	/* This is how to read the time code IN THE STSD ATOM!, not here.
	 * it is possible taht using this values we might guess which values are acceptable */

/*	uint32_t reserved = readBE<uint32_t>(start);

	uint32_t flags = readBE<uint32_t>(start + 4);
	uint32_t timescale = readBE<uint32_t>(start + 8);

	//dunno what values are reasonable (might use the value from the other video though.
	uint32_t frameduration = readBE<uint32_t>(start + 12);
	uint8_t nframes = readBE<uint8_t>(start + 16);
	uint8_t empty = readBE<uint8_t>(start + 17);
*/

	return match;
}
