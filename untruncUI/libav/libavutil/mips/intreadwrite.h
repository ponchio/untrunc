/*
 * Copyright (c) 2009 Mans Rullgard <mans@mansr.com>
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

#ifndef AVUTIL_MIPS_INTREADWRITE_H
#define AVUTIL_MIPS_INTREADWRITE_H

#include <stdint.h>
#include "config.h"

/*
 * GCC actually handles unaligned accesses correctly in all cases
 * except, absurdly, 32-bit loads on mips64.
 *
 * https://git.libav.org/?p=libav.git;a=commit;h=b82b49a5b774b6ad9119e981c72b8f594fee2ae0
 */
#if HAVE_MIPS64R2_INLINE || HAVE_MIPS64R1_INLINE

#define AV_RN32 AV_RN32
static av_always_inline uint32_t AV_RN32(const void *p)
{
    struct __attribute__((packed)) u32 { uint32_t v; };
    const uint8_t *q = p;
    const struct u32 *pl = (const struct u32 *)(q + 3 * !HAVE_BIGENDIAN);
    const struct u32 *pr = (const struct u32 *)(q + 3 *  HAVE_BIGENDIAN);
    uint32_t v;
    __asm__ ("lwl %0, %1  \n\t"
             "lwr %0, %2  \n\t"
             : "=&r"(v)
             : "m"(*pl), "m"(*pr));
    return v;
}

#endif /* HAVE_MIPS64R2_INLINE || HAVE_MIPS64R1_INLINE */

#endif /* AVUTIL_MIPS_INTREADWRITE_H */
