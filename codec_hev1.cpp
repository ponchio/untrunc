#include "codec.h"
#include "log.h"


using namespace std;

/* H265 NAL unit types */
enum {
	NAL_TRAIL_N     = 0, // non keyframe
	NAL_TRAIL_R     = 1,
	NAL_RASL_N      = 8,
	NAL_RASL_R      = 9,
	NAL_IDR_W_RADL  = 19, // keyframe
	NAL_IDR_N_LP    = 20,
	NAL_CRA_NUT     = 21,
	NAL_AUD         = 35,  // Access unit delimiter
	NAL_EOB_NUT     = 37,  // End of bitstream
	NAL_FILLER_DATA = 38,
};


class H265NalInfo {
public:
	H265NalInfo() = default;
	H265NalInfo(const unsigned char* start, int max_size) {
		is_ok = parseNal(start, max_size);
	}

	uint32_t length_ = 0;
	int nuh_layer_id_ = 0;
	int nal_type_ = 0;
	int nuh_temporal_id_plus1 = 0;

	bool is_ok = false;  // did parsing work
	bool is_forbidden_set_ = false;
	const unsigned char* data_ = nullptr;
	bool isInNewFrame = false;
	bool parseNal(const unsigned char* start, uint32_t max_size);

	bool isKeyFrame() {
		return
			nal_type_ == NAL_IDR_N_LP ||
			nal_type_ == NAL_IDR_W_RADL;
	}
	bool isSlice() {
		return
			nal_type_ == NAL_TRAIL_N ||
			nal_type_ == NAL_TRAIL_R ||
			nal_type_ == NAL_CRA_NUT ||
			nal_type_ == NAL_RASL_N ||
			nal_type_ == NAL_RASL_R ||
	//	    nal_type_ == NAL_IDR_N_LP ||
			nal_type_ == NAL_IDR_W_RADL;
	}
};





// see avc1/nal.cpp for more detailed comments
bool H265NalInfo::parseNal(const unsigned char *buffer, uint32_t maxlength) {

	if(buffer[0] != 0)
		return false;

	uint32_t len = readBE<int32_t>(buffer);//swap32(*(uint32_t *)buffer);
	length_ = len + 4;

	if(length_ > maxlength)
		return false;
	buffer += 4;

	if(*buffer & (1 << 7)) {
		is_forbidden_set_ = true;
		// sometimes the length is still correct
		return false;
	}
	nal_type_ = *buffer >> 1 ;
	if(nal_type_ > 40)
		return false;

	nuh_layer_id_ = (*buffer & 1) << 6 | (*(buffer+1) >> 5);

	nuh_temporal_id_plus1 = (*(buffer+1) & 0b111);

	if((nal_type_ == NAL_EOB_NUT && nuh_temporal_id_plus1) || (nal_type_ != NAL_EOB_NUT && !nuh_temporal_id_plus1))
		return false;

	if(isSlice()) {
		data_ = buffer+2;
		isInNewFrame = *data_ >> 7;
	}
	Log::debug << "NAL Type :" << nal_type_  << endl;
	return true;
}


Match Codec::hev1Match(const unsigned char *start, int maxlength) {
	Match match;

	const unsigned char *pos = start;
	int iter=0;
	
	H265NalInfo previous_nal;

	while(1) {
		iter++;
		Log::debug << "In While loop [" << iter << "] :" << (void *) pos  << '\n';
		H265NalInfo nal_info(pos, maxlength);
#ifdef __GEAR360__
		// I never have more than 2 iterations in this test for Gear 360
		// In case I find 3 iteratinons that this leads to an error in 
		// content parsing
		if((!nal_info.is_ok)|| (iter >=2 ))
#else
		if (!nal_info.is_ok)
#endif
			return match;

		if (nal_info.isKeyFrame())
			match.keyframe = true;

		if(nal_info.isSlice()) {
			if (previous_nal.is_ok) {
				if (nal_info.isInNewFrame ||
					previous_nal.nuh_layer_id_ != nal_info.nuh_layer_id_){
					return match;
				}
			}
		} else switch(nal_info.nal_type_) {
		case NAL_AUD: // Access unit delimiter
			if (!previous_nal.is_ok)
				break;
			return match;
		case NAL_FILLER_DATA:
			break;
		default:
/*			vector<int> dont_warn = {20, 32, 33, 34, 39};
			if (!contains(dont_warn, nal_info.nal_type_))
				logg(W2, "unhandled nal_type: ", nal_info.nal_type_, "\n"); */
			if (nal_info.is_forbidden_set_) {
				return match;
			}
			break;
		}

		pos += nal_info.length_;
		match.length += nal_info.length_;
		maxlength -= nal_info.length_;
		if (maxlength == 0)
			return match;

		previous_nal = nal_info;

		match.chances = 1e10;
	}
	return match;
}

