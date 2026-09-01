/**************************************************************/
/* PROTOTHREAD_LOCK.H */
/* https://github.com/LarryRuane/protothread */
/* Copyright (c) 2008-present Larry Ruane */
/* Distributed under the MIT software license, see the accompanying */
/* file LICENSE or https://opensource.org/licenses/MIT. */
/* SPDX-License-Identifier: MIT */
/* Reader-writer (shared-exclusive) locks */
/**************************************************************/
#ifndef PROTOTHREAD_LOCK_H
#define PROTOTHREAD_LOCK_H

#include "protothread.h"

typedef enum {
    PT_LOCK_READ,
    PT_LOCK_WRITE,
    PT_LOCK_READING,
    PT_LOCK_WRITING,
} pt_lock_state_t ;

/* per-thread */
typedef struct _pt_lock_env_t {
    pt_func_t pt_func ;
    pt_lock_state_t state ;
    struct _pt_lock_env_t *next ;
} pt_lock_env_t ;

/* per lock; waiting is a circular FIFO that points to the NEWEST waiter,
 * so waiting->next is the oldest -- the same convention protothread.h uses
 * for its run and wait lists. Requests are granted in arrival order, so a
 * steady stream of readers cannot starve a waiting writer.
 */
typedef struct _pt_lock_t {
    unsigned int nreaders ;             /* number of current readers */
    unsigned int nwriters ;             /* number of current writers (zero or 1) */
    pt_lock_env_t *waiting ;            /* newest waiting thread (environment) */
} pt_lock_t ;

static inline void
pt_lock_init(pt_lock_t *lock)
{
    lock->nreaders = 0 ;
    lock->nwriters = 0 ;
    lock->waiting = NULL ;
}

/* the oldest waiter, or NULL */
static inline pt_lock_env_t *
pt_lock_oldest(pt_lock_t const *lock)
{
    return lock->waiting ? lock->waiting->next : NULL ;
}

/* append to the tail of the FIFO */
static inline void
pt_lock_enqueue(pt_lock_t *lock, pt_lock_env_t *c)
{
    if (lock->waiting) {
        c->next = lock->waiting->next ;
        lock->waiting->next = c ;
    } else {
        c->next = c ;
    }
    lock->waiting = c ;
}

/* remove the oldest waiter (which must exist) */
static inline void
pt_lock_dequeue(pt_lock_t *lock)
{
    pt_lock_env_t * const oldest = lock->waiting->next ;
    if (oldest == lock->waiting) {
        lock->waiting = NULL ;
    } else {
        lock->waiting->next = oldest->next ;
    }
}

/* start as many requests as possible, in arrival order
 */
static inline void
pt_lock_update(pt_lock_t *lock)
{
    pt_lock_env_t *w = pt_lock_oldest(lock) ;

    if (w == NULL) {
        /* nothing to do */
        return ;
    }

    switch (w->state) {
    case PT_LOCK_READ:
        if (lock->nwriters) {
            pt_assert(lock->nwriters == 1) ;
            return ;
        }
        /* start the first and every consecutive additional reader */
        while ((w = pt_lock_oldest(lock)) != NULL && w->state == PT_LOCK_READ) {
            lock->nreaders ++ ;
            pt_lock_dequeue(lock) ;
            w->state = PT_LOCK_READING ;
            pt_broadcast(pt_get_pt(w), w) ;
        }
        break ;
    case PT_LOCK_WRITE:
        if (lock->nreaders || lock->nwriters) {
            break ;
        }
        lock->nwriters ++ ;
        pt_lock_dequeue(lock) ;
        w->state = PT_LOCK_WRITING ;
        pt_broadcast(pt_get_pt(w), w) ;
        break ;
    case PT_LOCK_READING:
    case PT_LOCK_WRITING:
        /* this request is already active! */
        pt_assert(0) ;
        break ;
    }
}

static inline pt_t
pt_lock_acquire_read_f(pt_lock_env_t *c, pt_lock_t *lock)
{
    pt_resume(c) ;
    c->state = PT_LOCK_READ ;
    pt_lock_enqueue(lock, c) ;
    pt_lock_update(lock) ;
    while (c->state == PT_LOCK_READ) {
        pt_wait(c, c) ;
    }
    pt_assert(c->state == PT_LOCK_READING) ;
    return PT_DONE ;
}
#define pt_lock_acquire_read(c, lock_env, lock) \
    pt_call(c, pt_lock_acquire_read_f, lock_env, lock)

static inline pt_t
pt_lock_acquire_write_f(pt_lock_env_t *c, pt_lock_t *lock)
{
    pt_resume(c) ;
    c->state = PT_LOCK_WRITE ;
    pt_lock_enqueue(lock, c) ;
    pt_lock_update(lock) ;
    while (c->state == PT_LOCK_WRITE) {
        pt_wait(c, c) ;
    }
    pt_assert(c->state == PT_LOCK_WRITING) ;
    return PT_DONE ;
}
#define pt_lock_acquire_write(c, lock_env, lock)\
    pt_call(c, pt_lock_acquire_write_f, lock_env, lock)

/* guaranteed not to break context */
static inline void
pt_lock_release_read(pt_lock_env_t *c, pt_lock_t *lock)
{
    (void)c ;   /* only referenced by pt_assert() */
    pt_assert(c->state == PT_LOCK_READING) ;
    pt_assert(!lock->nwriters) ;
    pt_assert(lock->nreaders) ;
    lock->nreaders -- ;
    pt_lock_update(lock) ;
}

static inline void
pt_lock_release_write(pt_lock_env_t *c, pt_lock_t *lock)
{
    (void)c ;   /* only referenced by pt_assert() */
    pt_assert(c->state == PT_LOCK_WRITING) ;
    pt_assert(!lock->nreaders) ;
    pt_assert(lock->nwriters == 1) ;
    lock->nwriters -- ;
    pt_lock_update(lock) ;
}

/* TODO: "try" routines (cannot block, return bool_t)
 *
 * TODO: upgrades
 *
 * Various 'force' levels; either have routines that do these or
 * return an error if they can't, or have predicates and have the
 * routines assert if they can't do it:
 *
 * - fairly without context break (no other active readers, no waiters)
 * - unfairly without context break (no other active readers, waiting writer)
 * - fairly with context break (no waiting writer; wait for readers to drain)
 * - unfairly with context break (waiting writer; wait for readers to drain)
 *
 * Even that last one fails if there's a pending upgrade.
 */

#endif /* PROTOTHREAD_LOCK_H */
