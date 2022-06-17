/*
 * Apple ProRes compatible decoder
 *
 * Copyright (c) 2010-2011 Maxim Poliakovski
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

#ifndef AVCODEC_PRORESDSP_H
#define AVCODEC_PRORESDSP_H

#include <stddef.h>
#include <stdint.h>

#define PRORES_BITS_PER_SAMPLE 10 ///< output precision of prores decoder

typedef struct ProresDSPContext {
    int idct_permutation_type;
    uint8_t idct_permutation[64];
    void (*idct_put)(uint16_t *out, ptrdiff_t linesize, int16_t *block, const int16_t *qmat);
} ProresDSPContext;

void ff_proresdsp_init(ProresDSPContext *dsp);

void ff_proresdsp_init_x86(ProresDSPContext *dsp);

#endif /* AVCODEC_PRORESDSP_H */
