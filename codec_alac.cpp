#include "codec.h"
#include "log.h"
#include <string.h>
#include "avlog.h"

#include <iostream>
using namespace std;
//#include <get_bits.h>


using namespace std;
/* alac is a compressed codec for audio.
 * can be found here: git clone https://github.com/macosforge/alac.git ALAC
 * The header is as such:
 * 3 bits for number of channels.
 * 16 bit unknown skipped (most probably zero),
 * 1 bit has size for output sample size
 * 2 bits number of bytes in the uncompressed stream
 * 1 bit: uncompressed if true
 *

 * next is a 16 bits skipped
 * 4 prediction type
 * 4 prediction quantization
 * 3 rice modified
 * 5 predictor coeff num.
 * * Anyway without parsing it's very probable that the first 24 bits stay exactly the same.
 * so we need to use the sample as a starting point and add a 2x penalty for every different bit.
 */

typedef struct ALACContext {
	AVCodecContext *avctx;
	GetBitContext gb;
	int channels;

	int32_t *predict_error_buffer[2];
	int32_t *output_samples_buffer[2];
	int32_t *extra_bits_buffer[2];

	uint32_t max_samples_per_frame;
	uint8_t  sample_size;
	uint8_t  rice_history_mult;
	uint8_t  rice_initial_history;
	uint8_t  rice_limit;

	int extra_bits;     /**< number of extra bits ALACContext *beyond 16-bit */
	int nb_samples;     /**< number of samples in the current frame */
} ALACContext;

Match Codec::alacMatch(const unsigned char *start, int maxlength) {

	if(!context)
		throw string("Missing context for alac codec.");

	ALACContext *alac = (ALACContext *)context->priv_data;

	Match match;


	uint32_t duration = 0;


	AvLog useAvLog();
	av_log_set_level(0);
	AVFrame *frame = av_frame_alloc();
	if(!frame)
		throw string("Could not create AVFrame");
	AVPacket avp;
	av_init_packet(&avp);
	avp.data = (uint8_t *)start;
	avp.size = maxlength;
	int got_frame = 0;
	avcodec_decode_audio4(context, frame, &got_frame, &avp);

	int consumed = (alac->gb.index-1) /8 + 1;
	Log::debug << "Alac length in bits: " << alac->gb.index << " in bytes: " << consumed << "\n";

	av_packet_unref(&avp);
	av_frame_free(&frame);

	if(consumed < 12) {
		match.chances = 0.0f;

	} else {
		match.chances = 10000;
		match.length = consumed;
	}
	return match;
}
