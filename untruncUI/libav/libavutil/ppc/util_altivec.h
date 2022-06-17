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

/**
 * @file
 * Contains misc utility macros and inline functions
 */

#ifndef AVUTIL_PPC_UTIL_ALTIVEC_H
#define AVUTIL_PPC_UTIL_ALTIVEC_H

#include <stdint.h>

#include "config.h"

/***********************************************************************
 * Vector types
 **********************************************************************/
#define vec_u8  vector unsigned char
#define vec_s8  vector signed char
#define vec_u16 vector unsigned short
#define vec_s16 vector signed short
#define vec_u32 vector unsigned int
#define vec_s32 vector signed int
#define vec_f   vector float

/***********************************************************************
 * Null vector
 **********************************************************************/
#define LOAD_ZERO const vec_u8 zerov = vec_splat_u8( 0 )

#define zero_u8v  (vec_u8)  zerov
#define zero_s8v  (vec_s8)  zerov
#define zero_u16v (vec_u16) zerov
#define zero_s16v (vec_s16) zerov
#define zero_u32v (vec_u32) zerov
#define zero_s32v (vec_s32) zerov

#if HAVE_ALTIVEC
#include <altivec.h>

// used to build registers permutation vectors (vcprm)
// the 's' are for words in the _s_econd vector
#define WORD_0 0x00,0x01,0x02,0x03
#define WORD_1 0x04,0x05,0x06,0x07
#define WORD_2 0x08,0x09,0x0a,0x0b
#define WORD_3 0x0c,0x0d,0x0e,0x0f
#define WORD_s0 0x10,0x11,0x12,0x13
#define WORD_s1 0x14,0x15,0x16,0x17
#define WORD_s2 0x18,0x19,0x1a,0x1b
#define WORD_s3 0x1c,0x1d,0x1e,0x1f

#define vcprm(a,b,c,d) (const vec_u8){WORD_ ## a, WORD_ ## b, WORD_ ## c, WORD_ ## d}


// Transpose 8x8 matrix of 16-bit elements (in-place)
#define TRANSPOSE8(a,b,c,d,e,f,g,h) \
do { \
    vec_s16 A1, B1, C1, D1, E1, F1, G1, H1; \
    vec_s16 A2, B2, C2, D2, E2, F2, G2, H2; \
 \
    A1 = vec_mergeh (a, e); \
    B1 = vec_mergel (a, e); \
    C1 = vec_mergeh (b, f); \
    D1 = vec_mergel (b, f); \
    E1 = vec_mergeh (c, g); \
    F1 = vec_mergel (c, g); \
    G1 = vec_mergeh (d, h); \
    H1 = vec_mergel (d, h); \
 \
    A2 = vec_mergeh (A1, E1); \
    B2 = vec_mergel (A1, E1); \
    C2 = vec_mergeh (B1, F1); \
    D2 = vec_mergel (B1, F1); \
    E2 = vec_mergeh (C1, G1); \
    F2 = vec_mergel (C1, G1); \
    G2 = vec_mergeh (D1, H1); \
    H2 = vec_mergel (D1, H1); \
 \
    a = vec_mergeh (A2, E2); \
    b = vec_mergel (A2, E2); \
    c = vec_mergeh (B2, F2); \
    d = vec_mergel (B2, F2); \
    e = vec_mergeh (C2, G2); \
    f = vec_mergel (C2, G2); \
    g = vec_mergeh (D2, H2); \
    h = vec_mergel (D2, H2); \
} while (0)


/** @brief loads unaligned vector @a *src with offset @a offset
    and returns it */
static inline vec_u8 unaligned_load(int offset, uint8_t *src)
{
    register vec_u8 first = vec_ld(offset, src);
    register vec_u8 second = vec_ld(offset + 15, src);
    register vec_u8 mask = vec_lvsl(offset, src);
    return vec_perm(first, second, mask);
}

/**
 * loads vector known misalignment
 * @param perm_vec the align permute vector to combine the two loads from lvsl
 */
static inline vec_u8 load_with_perm_vec(int offset, uint8_t *src, vec_u8 perm_vec)
{
    vec_u8 a = vec_ld(offset, src);
    vec_u8 b = vec_ld(offset + 15, src);
    return vec_perm(a, b, perm_vec);
}

#define vec_unaligned_load(b)                                   \
    vec_perm(vec_ld(0, b), vec_ld(15, b), vec_lvsl(0, b));

#endif /* HAVE_ALTIVEC */

#if HAVE_VSX
#if HAVE_BIGENDIAN
#define vsx_ld_u8_s16(off, p)                               \
    ((vec_s16)vec_mergeh((vec_u8)vec_splat_u8(0),           \
                         (vec_u8)vec_vsx_ld((off), (p))))
#else
#define vsx_ld_u8_s16(off, p)                               \
    ((vec_s16)vec_mergeh((vec_u8)vec_vsx_ld((off), (p)),    \
                         (vec_u8)vec_splat_u8(0)))
#endif /* HAVE_BIGENDIAN */
#endif /* HAVE_VSX */

#endif /* AVUTIL_PPC_UTIL_ALTIVEC_H */
