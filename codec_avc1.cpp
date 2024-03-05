#include "codec.h"
#include "log.h"

#include "avlog.h"

using namespace std;



// AVC1
class H264sps {
public:
	int  log2_max_frame_num;
	bool frame_mbs_only_flag;
	int  poc_type;
	int  log2_max_poc_lsb;

	H264sps()
		: log2_max_frame_num(0),
		  frame_mbs_only_flag(false),
		  poc_type(0),
		  log2_max_poc_lsb(0)
	{ }

	H264sps(const SPS &avsps)
		: log2_max_frame_num(avsps.log2_max_frame_num),
		  frame_mbs_only_flag(bool(avsps.frame_mbs_only_flag)),
		  poc_type(avsps.poc_type),
		  log2_max_poc_lsb(avsps.log2_max_poc_lsb)
	{ }

	H264sps &operator=(const SPS &avsps) { return *this = H264sps(avsps); }

	void parseSPS(const uint8_t *data, int size);
};


void H264sps::parseSPS(const uint8_t *data, int size) {

	assert(data != NULL);
	if (data[0] != 1) {
		Log::debug << "Uncharted territory...\n";
	}

	if (size < 7) {
		throw string("Could not parse SPS!");
	}

	// Decode SPS from avcC.
	const uint8_t *p   = data;
	int            cnt = p[5] & 0x1f;   // Number of SPS.
	p += 6;
	if(cnt != 1) {
		Log::debug << "Not supporting more than 1 SPS unit for the moment; might fail horribly.\n";
	}
	for(int i = 0; i < cnt; i++) {
		//nalsize = AV_RB16(p) + 2;
		int nalsize = readBE<uint16_t>(p);
		if (p - data + nalsize > size) {
			throw string("Could not parse SPS!");
		}
#if 0
		// From: libavcodec/h264_parse.c
		ret = decode_extradata_ps_mp4(p, nalsize, ps, err_recognition, logctx);
		if (ret < 0) {
			av_log(logctx, AV_LOG_ERROR,
				   "Decoding SPS %d from avcC failed\n", i);
			return ret;
		}
		p += nalsize;
#endif
		break;
	}

	// Skip PPS.
}


class NalInfo {

public:
	static const int MaxAVC1Length = 8 * (1 << 20);

	int length;

	int ref_idc;
	int nal_type;
	int first_mb;           // Unused.
	int slice_type;         // Should match the NAL type (1, 5).
	int pps_id;             // Pic parameter set id: which parameter set to use.
	int frame_num;
	int field_pic_flag;
	int bottom_pic_flag;
	int idr_pic_flag;       // Actually 1 for nal_type 5, 0 for nal_type 0.
	int idr_pic_id;         // Read only for nal_type 5.
	int poc_type;           // If zero, check the poc lsb.
	int poc_lsb;            // Pic order count - least significant bit.

	NalInfo()
		: length(0),
		  ref_idc(0),
		  nal_type(0),
		  first_mb(0),
		  slice_type(0),
		  pps_id(0),
		  frame_num(0),
		  field_pic_flag(0),
		  bottom_pic_flag(0),
		  idr_pic_flag(0),
		  idr_pic_id(0),
		  poc_type(0),
		  poc_lsb(0)
	{ }

	bool getNalInfo(const H264sps &sps, uint32_t maxlength, const uint8_t *buffer);
	void clear();
	void print(int indentation = 0);

private:
	static int golomb  (       uint8_t *&buffer, int &offset);
	static int readBits(int n, uint8_t *&buffer, int &offset);
};


void NalInfo::clear() {
	length          = 0;
	ref_idc         = 0;
	nal_type        = 0;
	first_mb        = 0;
	slice_type      = 0;
	pps_id          = 0;
	frame_num       = 0;
	field_pic_flag  = 0;
	bottom_pic_flag = 0;
	idr_pic_flag    = 0;
	idr_pic_id      = 0;
	poc_type        = 0;
	poc_lsb         = 0;
}

void NalInfo::print(int indentation) {
	const string indent((indentation >= 0)? indentation : 0, ' ');

	Log::debug << indent << "Length         : " << length          << ((length < 8+4 || length > MaxAVC1Length+4) ? " (incorrect)":"") << '\n';
	Log::debug << indent << "Ref idc        : " << ref_idc         << ((unsigned(ref_idc) > 3) ? " (incorrect)":"") << '\n';
	Log::debug << indent << "Nal type       : " << nal_type        << ((unsigned(nal_type) > 0x1f) ? " (incorrect)":"") << '\n';
	Log::debug << indent << "First mb       : " << first_mb        << '\n';
	Log::debug << indent << "Slice type     : " << slice_type      << ((unsigned(slice_type) > 9) ? " (incorrect)":"") << '\n';
	Log::debug << indent << "Pic parm set id: " << pps_id          << '\n';
	Log::debug << indent << "Frame number   : " << frame_num       << '\n';
	Log::debug << indent << "Field  pic flag: " << field_pic_flag  << '\n';
	if(field_pic_flag)
		Log::debug << indent << "Bottom pic flag: " << bottom_pic_flag << '\n';
	Log::debug << indent << "Idr pic id     : " << idr_pic_id      << '\n';
	if(poc_type)
		Log::debug << indent << "Poc type       : " << poc_type    << '\n';
	else
		Log::debug << indent << "Poc lsb        : " << poc_lsb     << '\n';
	Log::flush();
}


int NalInfo::golomb(uint8_t *&buffer, int &offset) {
	assert(buffer != NULL && offset >= 0 && offset < 8);
	// Count the leading zeroes.
	int count = 0;
	while((*buffer & (0x01 << (7 - offset))) == 0) {
		count++;
		offset++;
		if(offset == 8) {
			buffer++;
			offset = 0;
		}
		if(count > 20) {
			cerr << "Failed reading golomb: too large!\n";
			return -1;
		}
	}
	// Skip the single 1 delimiter.
	offset++;
	if(offset == 8) {
		buffer++;
		offset = 0;
	}
	uint32_t res = 1;
	// Read count bits.
	while(count-- > 0) {
		res <<= 1;
		res |= (*buffer & (0x01 << (7 - offset))) >> (7 - offset);
		//res |= (*buffer >> (7 - offset)) & 0x01;
		offset++;
		if(offset == 8) {
			buffer++;
			offset = 0;
		}
	}
	return res - 1;
}

int NalInfo::readBits(int n, uint8_t *&buffer, int &offset) {
	assert(buffer != NULL && offset >= 0);
	int res = 0;
	// Can't read in a single reading.
	while(n + offset > 8) {
		int d = 8 - offset;
		res <<= d;
		res |= *buffer & ((1 << d) - 1);
		offset = 0;
		buffer++;
		n -= d;
	}
	// Read the remaining bits.
	int d = (8 - offset - n);
	res <<= n;
	res |= (*buffer >> d) & ((1 << n) - 1);
	return res;
}

// Return false means this probably is not a NAL.
bool NalInfo::getNalInfo(const H264sps &sps, uint32_t maxlength, const uint8_t *buffer) {
	// Re-initialize.
	clear();

	if(buffer[0] != 0) {
		Log::debug << "First byte expected 0.\n";
		return false;
	}

	// This is supposed to be the length of the NAL unit.
	uint32_t len = readBE<uint32_t>(buffer);
	if(len > MaxAVC1Length) {
		Log::debug << "Max AVC1 length exceeded.\n";
		return false;
	}
	if(len > maxlength) {
		Log::debug << "Buffer size exceeded (" << (len ) << " > " << maxlength << ").\n";
		return false;
	}
	length = len + 4;
	//Log::debug << "Length         : " << length << '\n';

	buffer += 4;
	if(*buffer & (1 << 7)) {
		Log::debug << "Forbidden first bit 1.\n";
		return false;
	}
	ref_idc = *buffer >> 5;
	//Log::debug << "Ref idc        : " << ref_idc << '\n';

	nal_type = *buffer & 0x1f;
	//Log::debug << "Nal type       : " << nal_type << '\n';
	if(nal_type != 1 && nal_type != 5)
		return true;

	// Check if size is reasonable.
	if(len < 6) {
		Log::debug << "Length too short! (" << len << " < 7).\n";
		return false;
	}

	// Skip NAL header.
	buffer++;

	// Remove the emulation prevention 0x03 byte.
	// Could be done in place to speed up things.
	vector<uint8_t> data;
	data.reserve(len);
	for(unsigned int i = 0; i < len; i++) {
		data.push_back(buffer[i]);
		if(i+2 < len && buffer[i] == 0 && buffer[i+1] == 0 && buffer[i+2] == 3) {
			data.push_back(buffer[i+1]);
			assert(buffer[i+2] == 0x03);
			i += 2; // Skipping 3 byte!
		}
	}

	uint8_t *start  = data.data();
	int      offset = 0;
	first_mb   = golomb(start, offset);
	if(first_mb < 0) return false;
	// TODO: Is there a max number, so we could validate?
	//Log::debug << "First mb       : " << first_mb << '\n';

	slice_type = golomb(start, offset);
	if(slice_type < 0) return false;

	if(slice_type > 9) {
		Log::debug << "Invalid slice type (" << slice_type << "), probably this is not an avc1 sample.\n";
		return false;
	}
	//Log::debug << "Slice type     : " << slice_type << '\n';

	pps_id     = golomb(start, offset);
	if(pps_id < 0) return false;

	//Log::debug << "Pic parm set id: " << pps_id << '\n';
	// pps id: should be taked from master context (h264_slice.c:1257).

	// Assume separate colour plane flag is 0,
	//  otherwise we would have to read colour_plane_id which is 2 bits.

	// Assuming same sps for all frames.
	//SPS *sps = reinterpret_cast<SPS *>(h->ps.sps_list[0]->data);  // may_alias.
	frame_num = readBits(sps.log2_max_frame_num, start, offset);
	//Log::debug << "Frame number   : " << frame_num << '\n';

	// Read 2 flags.
	field_pic_flag  = 0;
	bottom_pic_flag = 0;
	if(sps.frame_mbs_only_flag) {
		field_pic_flag = readBits(1, start, offset);
		//Log::debug << "Field  pic flag: " << field_pic_flag << '\n';
		if(field_pic_flag) {
			bottom_pic_flag = readBits(1, start, offset);
			//Log::debug << "Bottom pic flag: " << bottom_pic_flag << '\n';
		}
	}

	idr_pic_flag = (nal_type == 5) ? 1 : 0;
	if (idr_pic_flag) {
		idr_pic_id = golomb(start, offset);
		if(idr_pic_id < 0) return false;

		//Log::debug << "Idr pic id     : " << idr_pic_id << '\n';
	}

	// If the pic order count type == 0.
	poc_type = sps.poc_type;
	if(sps.poc_type == 0) {
		poc_lsb = readBits(sps.log2_max_poc_lsb, start, offset);
		//Log::debug << "Poc lsb        : " << poc_lsb << '\n';
	}

	// Ignoring the delta_poc for the moment.
	return true;
}





Match Codec::avc1Search(const unsigned char *start, int maxlength, int makskip) {
	for(int i = 0; i < makskip; i++) {
		Match m = mp4aMatch(start + i, maxlength - i);
		if(m.chances > 0) {
			m.offset = i;
			return m;
		}
	}
	return Match();
	/*
	Match match;
	for(int offset = 0; offset < maxlength - 8; offset++) {
		if(start[offset] != 0)
			continue;
		uint32_t len = readBE<uint32_t>(start + offset);
		//too might use smallestSample and largestSample to constrain the size
		if(len < 8 || len > NalInfo::MaxAVC1Length)
			continue;
		//todo common values for 4 and 5 bytes should be usedc
		//if(start[offset+4] != 0x41 || start[offset+5] != 0x9a ) continue;
		if(start[offset + 4] & (1 << 7))
			continue;
		int nal_type = start[offset + 4] & 0x1f;
		if(nal_type > 21)
			continue;
		//this looks like it might be a packet. might want to do a more thorough check.
		match.offset = offset;
		match.chances = 10;
		return match;
	}
	return match; */
}


Match Codec::avc1Match(const unsigned char *start, int maxlength) {
	Match match;

	// This works only for a very specific kind of video.
	//#define SPECIAL_VIDEO
#ifdef SPECIAL_VIDEO
	int32_t s2 = readBE<int32_t>(start + 4);
	if(s != 0x00000002 || (s2 != 0x09300000 && s2 != 0x09100000))
		return false;
	return true;
#endif

	//First 4 bytes is the length the the packet and it's expected not to be bigger than 16M
	if(start[0] != 0) {
//		Log::debug << "avc1: Match with 0 header.\n";
		return match;
	}

	// NAL unit types
	//  see: libavcodec/h264.h
	//  see: ITU-T T-REC-H.264-201704, Table 7-1
	// enum {
	//     NAL_SLICE             =  1,  // Non keyframe.
	//     NAL_DPA               =  2,
	//     NAL_DPB               =  3,
	//     NAL_DPC               =  4,
	//     NAL_IDR_SLICE         =  5,  // Keyframe.
	//     NAL_SEI               =  6,
	//     NAL_SPS               =  7,
	//     NAL_PPS               =  8,
	//     NAL_AUD               =  9,
	//     NAL_END_SEQUENCE      = 10,
	//     NAL_END_STREAM        = 11,
	//     NAL_FILLER_DATA       = 12,
	//     NAL_SPS_EXT           = 13,
	//     NAL_PREFIX            = 14,
	//     NAL_SUB_SPS           = 15,
	//     NAL_DPS               = 16,
	//     NAL_AUXILIARY_SLICE   = 19,best
	//     NAL_EXTEN_SLICE       = 20,
	//     NAL_DEPTH_EXTEN_SLICE = 21,
	//
	//     NAL_FF_IGNORE         = 0xff0f001,
	// };
	//
	// First 4 bytes are the length, then the NAL starts.
	// ref_idc != 0 per unit_type = 5
	// ref_idc == 0 per unit_type = 6, 9, 10, 11, 12

	// See 7.4.1.2.4 Detection of the first VCL NAL unit of a primary coded picture
	//  for rules on how to group NALs into a picture.

	int64_t begin32 = readBE<int32_t>(start);
	if(begin32 == 0)
		return match;

	// TODO: Use the first byte of the NAL: forbidden bit and type!
	int nal_type = (start[4] & 0x1f);

	if(nal_type == 0)
		return match;
	// The other values are really uncommon on cameras...
	if(nal_type > 12) {
		Log::debug << "avc1: Possibly No match because of NAL type: " << nal_type << '\n';
//		return match;
	}
	if(nal_type > 29) {
		//if(nal_type != 1 && !(nal_type >= 5 && nal_type <= 12)) {
		Log::debug << "avc1: No match because of NAL type: " << nal_type << '\n';
		return match;
	}

	if(!context)
		return match;

	// XXX: Horrible Hack: Referencing unstable, internal data structures. XXX
	H264Context *h    = static_cast<H264Context*>(context->priv_data); //context->codec->
	const SPS   *hsps = NULL;
	if(h) {
		// Use currently active SPS.
		if(!hsps && h->ps.sps_list[0]) {
			// Use first SPS.
			hsps = reinterpret_cast<const SPS*>(h->ps.sps_list[0]->data); // may_alias.
		}
	}
	if(!hsps) {
		Log::debug << "Could not retrieve SPS.\n";
		return match;
	}
	H264sps sps(*hsps);

#if 0
	int consumed = -1;
	{
		AvLog useAvLog();
		AVFrame *frame = av_frame_alloc();
		if(!frame)
			throw string("Could not create AVFrame");
		AVPacket avp;
		av_init_packet(&avp);
		avp.data = start;
		avp.size = maxlength;
		int got_frame = 0;
		consumed = avcodec_decode_video2(context, frame, &got_frame, &avp);
		if(consumed == 0) {
			// Flush decoder to receive buffered packets.
			clog << "Flush " << name << " decoder.\n";
			got_frame = 0;
			av_packet_unref(&avp);
			av_frame_unref(frame);
			int consumed2 = avcodec_decode_video2(context, frame, &got_frame, &avp);
			if(consumed2 >= 0)
				consumed = consumed2;
		}
		av_packet_unref(&avp);
		av_frame_free(&frame);
	}

	return consumed;
#endif


	uint32_t  length = 0;
	const unsigned char *pos    = start;

	NalInfo previous;
	bool  seen_slice = false;

	bool first_pack = true;
	while(true) {
		NalInfo info;
		bool ok = info.getNalInfo(sps, maxlength, pos);
		if(!ok) {
			//THIS should never happens, but it happens
			if(first_pack) {

				//throw string("What's ahppinghin egherklhj HELP!");
				match.chances = 0.0f;
				if(info.length == 0) {
					NalInfo info1;

					info1.getNalInfo(sps, maxlength, pos);
					return match;
				}
				return match;
			}
			goto final;
		}
		match.chances = 1024;

		first_pack = false;

		Log::debug << "Nal type: " << info.nal_type << " Nal ref idc " << info.ref_idc << endl;

		switch(info.nal_type) {
		case 12: break; //filler data
		case 1:
		case 5:
			if(!seen_slice) {
				previous   = info;
				seen_slice = true;
			} else {
				// Check for changes.
				//cout << "Frame number: " << info.frame_num << endl;
				if(previous.frame_num != info.frame_num) {
					Log::debug << "Different frame number.\n";
					goto final;

				}
				if(previous.pps_id != info.pps_id) {
					Log::debug << "Different pic parameter set id.\n";
					goto final;
				}
				// All these conditions are listed in the docs, but
				// it looks like it creates invalid packets if respected.
				// Puzzling.
				//#define STRICT_NAL_INFO_CHECKING  1
//#define STRICT_FIELD_PIC_CHECKING
#ifdef STRICT_FIELD_PIC_CHECKING
				if(previous.field_pic_flag != info.field_pic_flag) {
					Log::debug << "Different field  pic flag.\n";
					goto final;
				}
				if(previous.field_pic_flag && info.field_pic_flag
						&& previous.bottom_pic_flag != info.bottom_pic_flag)
				{
					Log::debug << "Different bottom pic flag.\n";
					goto final;
				}
#endif

#define STRICT_REF_IDC_CHECKING
#ifdef STRICT_REF_IDC_CHECKING
				if(previous.ref_idc != info.ref_idc) {
					Log::debug << "Different ref idc.\n";
					goto final;
				}
#endif

//#define STRICT_POC_TYPE_CHECKING
#ifdef STRICT_POC_TYPE_CHECKING
				if(previous.poc_type == 0 && info.poc_type == 0
						&& previous.poc_lsb != info.poc_lsb)
				{
					Log::debug << "Different pic order count lsb (poc lsb).\n";
					goto final;
				}
#endif
				if(previous.idr_pic_flag != info.idr_pic_flag) {
					Log::debug << "Different NAL type (5, 1).\n";
					//TODO check this was an error and not on purpouse.
					goto final;
				}
//#define STRICT_PIC_IDR_CHECKING
#ifdef STRICT_PIC_IDR_CHECKING
				if(previous.idr_pic_flag == 1 && info.idr_pic_flag == 1
						&& previous.idr_pic_id != info.idr_pic_id)
				{
					Log::debug << "Different idr pic id for keyframe.\n";
					goto final;
				}
#endif
			}
			break;
		default:
			if(seen_slice) {
				Log::debug << "New access unit since seen picture.\n";
				goto final;
			}
			break;
		}
		if(info.nal_type == 5) {
			match.keyframe = true;
			Log::debug << "KeyFrame!" << endl;
		}
		pos       += info.length;
		length    += info.length;
		maxlength -= info.length;
		//Log::debug << "Partial length : " << length << '\n';
	}
	final:
	match.length = length;
	//probability depends on length, the longer the higher.
	match.chances = 1 + length/10;
	if(maxlength < 8)
		return match;

	float stat_chanches = 0;
	if(stats.beginnings32.count(begin32))
		stat_chanches = stats.beginnings32[begin32];
	else {
		//TODO this actually depends by the number of different beginnings.
		//changes = (n -1); //se sono due le chanches sono davvero piccole.
		stat_chanches = stats.beginnings32.size() -1;
	}

	int64_t begin64 = readBE<int64_t>(start);
	if(stats.beginnings64.count(begin64))
		stat_chanches = stats.beginnings64[begin64];

	match.chances = std::max(stat_chanches, match.chances);

	Log::flush();
	return match;
}
