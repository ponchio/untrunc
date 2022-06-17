/*
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_PIXBLOCKDSP_H
#define AVCODEC_PIXBLOCKDSP_H

#include <stdint.h>

#include "config.h"

#include "avcodec.h"

typedef struct PixblockDSPContext {
    void (*get_pixels)(int16_t *restrict block /* align 16 */,
                       const uint8_t *pixels /* align 8 */,
                       ptrdiff_t stride);
    void (*diff_pixels)(int16_t *restrict block /* align 16 */,
                        const uint8_t *s1 /* align 8 */,
                        const uint8_t *s2 /* align 8 */,
                        ptrdiff_t stride);
} PixblockDSPContext;

void ff_pixblockdsp_init(PixblockDSPContext *c, AVCodecContext *avctx);
void ff_pixblockdsp_init_arm(PixblockDSPContext *c, AVCodecContext *avctx,
                             unsigned high_bit_depth);
void ff_pixblockdsp_init_ppc(PixblockDSPContext *c, AVCodecContext *avctx,
                             unsigned high_bit_depth);
void ff_pixblockdsp_init_x86(PixblockDSPContext *c, AVCodecContext *avctx,
                             unsigned high_bit_depth);

#endif /* AVCODEC_PIXBLOCKDSP_H */
