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

// This header should only be used to simplify code where
// threading is optional, not as a generic threading abstraction.

#ifndef AVUTIL_THREAD_H
#define AVUTIL_THREAD_H

#include "config.h"

#if HAVE_PTHREADS || HAVE_W32THREADS

#if HAVE_PTHREADS
#include <pthread.h>
#else
#include "compat/w32pthreads.h"
#endif

#define AVMutex pthread_mutex_t

#define ff_mutex_init    pthread_mutex_init
#define ff_mutex_lock    pthread_mutex_lock
#define ff_mutex_unlock  pthread_mutex_unlock
#define ff_mutex_destroy pthread_mutex_destroy

#define AVOnce pthread_once_t
#define AV_ONCE_INIT PTHREAD_ONCE_INIT

#define ff_thread_once(control, routine) pthread_once(control, routine)

#else

#define AVMutex char

static inline int ff_mutex_init(AVMutex *mutex, const void *attr){ return 0; }
static inline int ff_mutex_lock(AVMutex *mutex){ return 0; }
static inline int ff_mutex_unlock(AVMutex *mutex){ return 0; }
static inline int ff_mutex_destroy(AVMutex *mutex){ return 0; }

#define AVOnce char
#define AV_ONCE_INIT 0

static inline int ff_thread_once(char *control, void (*routine)(void))
{
    if (!*control) {
        routine();
        *control = 1;
    }
    return 0;
}

#endif

#endif /* AVUTIL_THREAD_H */
