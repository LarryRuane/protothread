/**************************************************************/
/* PROTOTHREAD_LOCK.H */
/* Copyright (c) 2008, Larry Ruane, LeftHand Networks Inc. */
/* See license.txt */
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

/* per lock */
typedef struct _pt_lock_t {
    unsigned int nreaders ;             /* number of current readers */
    unsigned int nwriters ;             /* number of current writers (zero or 1) */
    pt_lock_env_t *waiting ;            /* waiting threads (environments) */
} pt_lock_t ;

void pt_lock_init(pt_lock_t *lock) ;

pt_t pt_lock_acquire_read_f(pt_lock_env_t *c, pt_lock_t *lock) ;
#define pt_lock_acquire_read(c, lock_env, lock) \
    pt_call(c, pt_lock_acquire_read_f, lock_env, lock)

pt_t pt_lock_acquire_write_f(pt_lock_env_t *c, pt_lock_t *lock) ;
#define pt_lock_acquire_write(c, lock_env, lock)\
    pt_call(c, pt_lock_acquire_write_f, lock_env, lock)

/* guaranteed not to break context */
void pt_lock_release_read(pt_lock_env_t *c, pt_lock_t *lock) ;
void pt_lock_release_write(pt_lock_env_t *c, pt_lock_t *lock) ;

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
