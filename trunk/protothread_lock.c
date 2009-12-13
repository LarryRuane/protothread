/**************************************************************/
/* PROTOTHREAD_LOCK.C */
/* Copyright (c) 2008, Larry Ruane, LeftHand Networks Inc. */
/* See license.txt */
/* Reader-writer (shared-exclusive) locks */
/**************************************************************/
#include <string.h>
#include <assert.h>

#include "protothread_lock.h"

void
pt_lock_init(pt_lock_t *lock)
{
    memset(lock, 0, sizeof(*lock)) ;
}

/* start as many requests as possible
 */
static void
pt_lock_update(pt_lock_t *lock)
{
    pt_lock_env_t *pt_lock_env ;

    if (lock->waiting == NULL) {
        /* nothing to do */
        return ;
    }

    pt_lock_env = lock->waiting ;
    switch (pt_lock_env->state) {
    case PT_LOCK_READ:
        if (lock->nwriters) {
            assert(lock->nwriters == 1) ;
            return ;
        }
        /* start the first or additional readers */
        while (lock->waiting) {
            pt_lock_env = lock->waiting ;
            if (pt_lock_env->state != PT_LOCK_READ) {
                break ;
            }
            lock->nreaders ++ ;
            lock->waiting = pt_lock_env->next ;
            pt_lock_env->state = PT_LOCK_READING ;
            pt_broadcast(pt_get_pt(pt_lock_env), pt_lock_env) ;
        }
        break ;
    case PT_LOCK_WRITE:
        if (lock->nreaders || lock->nwriters) {
            break ;
        }
        lock->nwriters ++ ;
        lock->waiting = pt_lock_env->next ;
        pt_lock_env->state = PT_LOCK_WRITING ;
        pt_broadcast(pt_get_pt(pt_lock_env), pt_lock_env) ;
        break ;
    case PT_LOCK_READING:
    case PT_LOCK_WRITING:
        /* this request is already active! */
        assert(0) ;
    }
}

pt_t pt_lock_acquire_read_f(pt_lock_env_t *c, pt_lock_t *lock)
{
    pt_resume(c) ;
    c->next = lock->waiting ;
    lock->waiting = c ;
    c->state = PT_LOCK_READ ;
    pt_lock_update(lock) ;
    while (c->state == PT_LOCK_READ) {
        pt_wait(c, c) ;
    }
    assert(c->state == PT_LOCK_READING) ;
    return PT_DONE ;
}

void pt_lock_release_read(pt_lock_env_t *c, pt_lock_t *lock)
{
    assert(c->state == PT_LOCK_READING) ;
    assert(!lock->nwriters) ;
    assert(lock->nreaders) ;
    lock->nreaders -- ;
    pt_lock_update(lock) ;
}

pt_t pt_lock_acquire_write_f(pt_lock_env_t *c, pt_lock_t *lock)
{
    pt_resume(c) ;
    c->next = lock->waiting ;
    lock->waiting = c ;
    c->state = PT_LOCK_WRITE ;
    pt_lock_update(lock) ;
    while (c->state == PT_LOCK_WRITE) {
        pt_wait(c, c) ;
    }
    assert(c->state == PT_LOCK_WRITING) ;
    return PT_DONE ;
}

void pt_lock_release_write(pt_lock_env_t *c, pt_lock_t *lock)
{
    assert(c->state == PT_LOCK_WRITING) ;
    assert(!lock->nreaders) ;
    assert(lock->nwriters == 1) ;
    lock->nwriters -- ;
    pt_lock_update(lock) ;
}
