/*
 * Copyright (c) 2001-2003 The FFmpeg project
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

/**
 * @file
 * ADPCM tables
 */

#ifndef AVCODEC_ADPCM_DATA_H
#define AVCODEC_ADPCM_DATA_H

#include <stdint.h>

extern const int8_t  ff_adpcm_index_table[16];
extern const int16_t ff_adpcm_step_table[89];
extern const int16_t ff_adpcm_AdaptationTable[];
extern const uint8_t ff_adpcm_AdaptCoeff1[];
extern const int8_t  ff_adpcm_AdaptCoeff2[];
extern const int16_t ff_adpcm_yamaha_indexscale[];
extern const int8_t  ff_adpcm_yamaha_difflookup[];

#endif /* AVCODEC_ADPCM_DATA_H */
