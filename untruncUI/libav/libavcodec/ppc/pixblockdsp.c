/*
 * Copyright (c) 2002 Brian Foley
 * Copyright (c) 2002 Dieter Shirley
 * Copyright (c) 2003-2004 Romain Dolbeau <romain@dolbeau.org>
 *
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

#include "config.h"

#include "libavutil/attributes.h"
#include "libavutil/cpu.h"
#include "libavutil/ppc/cpu.h"
#include "libavutil/ppc/util_altivec.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/pixblockdsp.h"

#if HAVE_ALTIVEC && HAVE_BIGENDIAN

static void get_pixels_altivec(int16_t *restrict block, const uint8_t *pixels,
                               ptrdiff_t stride)
{
    int i;
    vec_u8 perm = vec_lvsl(0, pixels);
    const vec_u8 zero = (const vec_u8)vec_splat_u8(0);

    for (i = 0; i < 8; i++) {
        /* Read potentially unaligned pixels.
         * We're reading 16 pixels, and actually only want 8,
         * but we simply ignore the extras. */
        vec_u8 pixl = vec_ld(0, pixels);
        vec_u8 pixr = vec_ld(7, pixels);
        vec_u8 bytes = vec_perm(pixl, pixr, perm);

        // Convert the bytes into shorts.
        vec_s16 shorts = (vec_s16)vec_mergeh(zero, bytes);

        // Save the data to the block, we assume the block is 16-byte aligned.
        vec_st(shorts, i * 16, (vec_s16 *)block);

        pixels += stride;
    }
}

static void diff_pixels_altivec(int16_t *restrict block, const uint8_t *s1,
                                const uint8_t *s2, ptrdiff_t stride)
{
    int i;
    vec_u8 perm1 = vec_lvsl(0, s1);
    vec_u8 perm2 = vec_lvsl(0, s2);
    const vec_u8 zero = (const vec_u8)vec_splat_u8(0);
    vec_s16 shorts1, shorts2;

    for (i = 0; i < 4; i++) {
        /* Read potentially unaligned pixels.
         * We're reading 16 pixels, and actually only want 8,
         * but we simply ignore the extras. */
        vec_u8 pixl  = vec_ld(0,  s1);
        vec_u8 pixr  = vec_ld(15, s1);
        vec_u8 bytes = vec_perm(pixl, pixr, perm1);

        // Convert the bytes into shorts.
        shorts1 = (vec_s16)vec_mergeh(zero, bytes);

        // Do the same for the second block of pixels.
        pixl  = vec_ld(0,  s2);
        pixr  = vec_ld(15, s2);
        bytes = vec_perm(pixl, pixr, perm2);

        // Convert the bytes into shorts.
        shorts2 = (vec_s16)vec_mergeh(zero, bytes);

        // Do the subtraction.
        shorts1 = vec_sub(shorts1, shorts2);

        // Save the data to the block, we assume the block is 16-byte aligned.
        vec_st(shorts1, 0, (vec_s16 *)block);

        s1    += stride;
        s2    += stride;
        block += 8;

        /* The code below is a copy of the code above...
         * This is a manual unroll. */

        /* Read potentially unaligned pixels.
         * We're reading 16 pixels, and actually only want 8,
         * but we simply ignore the extras. */
        pixl  = vec_ld(0,  s1);
        pixr  = vec_ld(15, s1);
        bytes = vec_perm(pixl, pixr, perm1);

        // Convert the bytes into shorts.
        shorts1 = (vec_s16)vec_mergeh(zero, bytes);

        // Do the same for the second block of pixels.
        pixl  = vec_ld(0,  s2);
        pixr  = vec_ld(15, s2);
        bytes = vec_perm(pixl, pixr, perm2);

        // Convert the bytes into shorts.
        shorts2 = (vec_s16)vec_mergeh(zero, bytes);

        // Do the subtraction.
        shorts1 = vec_sub(shorts1, shorts2);

        // Save the data to the block, we assume the block is 16-byte aligned.
        vec_st(shorts1, 0, (vec_s16 *)block);

        s1    += stride;
        s2    += stride;
        block += 8;
    }
}

#endif /* HAVE_ALTIVEC && HAVE_BIGENDIAN */

#if HAVE_VSX
static void get_pixels_vsx(int16_t *restrict block, const uint8_t *pixels,
                           ptrdiff_t stride)
{
    int i;
    for (i = 0; i < 8; i++) {
        vec_s16 shorts = vsx_ld_u8_s16(0, pixels);

        vec_vsx_st(shorts, i * 16, block);

        pixels += stride;
    }
}

static void diff_pixels_vsx(int16_t *restrict block, const uint8_t *s1,
                            const uint8_t *s2, ptrdiff_t stride)
{
    int i;
    vec_s16 shorts1, shorts2;
    for (i = 0; i < 8; i++) {
        shorts1 = vsx_ld_u8_s16(0, s1);
        shorts2 = vsx_ld_u8_s16(0, s2);

        shorts1 = vec_sub(shorts1, shorts2);

        vec_vsx_st(shorts1, 0, block);

        s1    += stride;
        s2    += stride;
        block += 8;
    }
}
#endif /* HAVE_VSX */

av_cold void ff_pixblockdsp_init_ppc(PixblockDSPContext *c,
                                     AVCodecContext *avctx,
                                     unsigned high_bit_depth)
{
#if HAVE_ALTIVEC && HAVE_BIGENDIAN
    if (!PPC_ALTIVEC(av_get_cpu_flags()))
        return;

    c->diff_pixels = diff_pixels_altivec;

    if (!high_bit_depth) {
        c->get_pixels = get_pixels_altivec;
    }
#endif /* HAVE_ALTIVEC && HAVE_BIGENDIAN */

#if HAVE_VSX
    if (!PPC_VSX(av_get_cpu_flags()))
        return;

    c->diff_pixels = diff_pixels_vsx;

    if (!high_bit_depth)
        c->get_pixels = get_pixels_vsx;
#endif /* HAVE_VSX */
}
