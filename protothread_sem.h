/**************************************************************/
/* PROTOTHREAD_SEM.H */
/* https://github.com/LarryRuane/protothread */
/* Copyright (c) 2008-present Larry Ruane */
/* Distributed under the MIT software license, see the accompanying */
/* file LICENSE or https://opensource.org/licenses/MIT. */
/* SPDX-License-Identifier: MIT */
/**************************************************************/
#ifndef PROTOTHREAD_SEM_H
#define PROTOTHREAD_SEM_H

#include "protothread.h"

typedef struct _pt_sem_env_t {
    pt_func_t pt_func ;
} pt_sem_env_t ;

/* This implementation is arguably not fair, because a thread can release
 * the semaphore and then acquire it again without blocking, even if there
 * are waiters. But this has better performance (fewer context switches).
 * If a thread is worried about monopolizing the semaphore, it can call
 * pt_yield() just before the sem_acquire() (that's always safe since the
 * sem_acquire() can cause a context break anyway).
 *
 * Semaphore-acquire could be implemented as a macro, which would allow it
 * to use the caller's context and not require one of its own.
 */

static inline pt_t
pt_sem_acquire_f(pt_sem_env_t *c, unsigned int *value)
{
    pt_resume(c) ;
    while (!(*value)) {
        pt_wait(c, value) ;
    }
    (*value) -- ;
    return PT_DONE ;
}
#define pt_sem_acquire(c, sem_env, value) pt_call(c, pt_sem_acquire_f, sem_env, value)

/* guaranteed not to break context */
static inline void
pt_sem_release(pt_sem_env_t *c, unsigned int *value)
{
    (*value) ++ ;
    pt_broadcast(pt_get_pt(c), value) ;
}

#endif /* PROTOTHREAD_SEM_H */
