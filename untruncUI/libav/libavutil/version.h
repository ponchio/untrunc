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

#ifndef AVUTIL_VERSION_H
#define AVUTIL_VERSION_H

#include "macros.h"

/**
 * @defgroup version_utils Library Version Macros
 *
 * Useful to check and match library version in order to maintain
 * backward compatibility.
 *
 * @{
 */

#define AV_VERSION_INT(a, b, c) (a<<16 | b<<8 | c)
#define AV_VERSION_DOT(a, b, c) a ##.## b ##.## c
#define AV_VERSION(a, b, c) AV_VERSION_DOT(a, b, c)

/**
 * @}
 */

/**
 * @file
 * @ingroup lavu
 * Libavutil version macros
 */

/**
 * @defgroup lavu_ver Version and Build diagnostics
 *
 * Macros and function useful to check at compiletime and at runtime
 * which version of libavutil is in use.
 *
 * @{
 */

#define LIBAVUTIL_VERSION_MAJOR 56
#define LIBAVUTIL_VERSION_MINOR  8
#define LIBAVUTIL_VERSION_MICRO  0

#define LIBAVUTIL_VERSION_INT   AV_VERSION_INT(LIBAVUTIL_VERSION_MAJOR, \
                                               LIBAVUTIL_VERSION_MINOR, \
                                               LIBAVUTIL_VERSION_MICRO)
#define LIBAVUTIL_VERSION       AV_VERSION(LIBAVUTIL_VERSION_MAJOR,     \
                                           LIBAVUTIL_VERSION_MINOR,     \
                                           LIBAVUTIL_VERSION_MICRO)
#define LIBAVUTIL_BUILD         LIBAVUTIL_VERSION_INT

#define LIBAVUTIL_IDENT         "Lavu" AV_STRINGIFY(LIBAVUTIL_VERSION)

/**
 * @}
 *
 * @defgroup depr_guards Deprecation guards
 * FF_API_* defines may be placed below to indicate public API that will be
 * dropped at a future version bump. The defines themselves are not part of
 * the public API and may change, break or disappear at any time.
 *
 * @{
 */

#ifndef FF_API_CRYPTO_CONTEXT
#define FF_API_CRYPTO_CONTEXT           (LIBAVUTIL_VERSION_MAJOR < 57)
#endif
#ifndef FF_API_PLUS1_MINUS1
#define FF_API_PLUS1_MINUS1             (LIBAVUTIL_VERSION_MAJOR < 57)
#endif
#ifndef FF_API_ERROR_FRAME
#define FF_API_ERROR_FRAME              (LIBAVUTIL_VERSION_MAJOR < 57)
#endif
#ifndef FF_API_VAAPI
#define FF_API_VAAPI                    (LIBAVUTIL_VERSION_MAJOR < 57)
#endif
#ifndef FF_API_PKT_PTS
#define FF_API_PKT_PTS                  (LIBAVUTIL_VERSION_MAJOR < 57)
#endif
#ifndef FF_API_CRYPTO_SIZE_T
#define FF_API_CRYPTO_SIZE_T            (LIBAVUTIL_VERSION_MAJOR < 57)
#endif


/**
 * @}
 */

#endif /* AVUTIL_VERSION_H */
