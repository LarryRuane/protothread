/**************************************************************/
/* PROTOTHREAD.C */
/* Copyright (c) 2008, Larry Ruane, LeftHand Networks Inc. */
/* See license.txt */
/**************************************************************/
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>

#include "protothread.h"

typedef struct protothread_s *state_t ;

state_t
protothread_create(void)
{
    state_t const s = malloc(sizeof(*s)) ;
    protothread_init(s) ;
    return s ;
}

void
protothread_free(state_t const s)
{
    protothread_deinit(s) ;
    free(s) ;
}

void
protothread_init(state_t const s)
{
    memset(s, 0, sizeof(*s)) ;
    pthread_mutex_init(&s->mutex, NULL) ;
}

void
protothread_deinit(state_t const s)
{
    if (PT_DEBUG) {
        int i ;
        for (i = 0; i < PT_NWAIT; i++) {
            pt_assert(s->wait[i] == NULL) ;
        }
        pt_assert(s->ready == NULL) ;
        pt_assert(s->nrunning == 0) ;
        pt_assert(s->nthread == 0) ;
    }
    pthread_mutex_destroy(&s->mutex) ;
}

/* link thread as the newest in the given (ready or wait) list */
static inline void
pt_link(pt_thread_t ** const head, pt_thread_t * const n)
{
    if (*head) {
        n->next = (*head)->next ;
        (*head)->next = n ;
    } else {
        n->next = n ;
    }
    *head = n ;
}

/* unlink and return the thread following prev, updating head if necessary */
static inline pt_thread_t *
pt_unlink(pt_thread_t ** const head, pt_thread_t * const prev)
{
    pt_thread_t * const next = prev->next ;
    prev->next = next->next ;
    if (next == prev) {
        *head = NULL ;
    } else if (next == *head) {
        *head = prev ;
    }
    if (PT_DEBUG) {
        next->next = NULL ;
    }
    return next ;
}

/* unlink and return the oldest (last) thread */
static inline pt_thread_t *
pt_unlink_oldest(pt_thread_t ** const head)
{
    return pt_unlink(head, *head) ;
}

/* finds thread <n> in list <head> and unlinks it.  Returns TRUE if
 * it was found.
 */
static inline bool_t
pt_find_and_unlink(pt_thread_t ** const head, pt_thread_t * const n)
{
    pt_thread_t * prev = *head ;

    while (*head) {
        pt_thread_t * const t = prev->next ;
        if (n != t) {
            /* Advance to next thread */
            prev = t ;
            /* looped back to start? finished */
            if (prev == *head) {
                break ;
            }
        } else {
            pt_unlink(head, prev) ;
            return TRUE ;
        }
    }

    return FALSE ;
}

bool_t
protothread_run_locked(state_t const s)
{
    pt_thread_t * run ;
    pt_t ret ;

    pt_assert(pt_mutex_is_locked(&s->mutex)) ;

    if (s->ready) {
        /* unlink the oldest ready thread */
        run = pt_unlink_oldest(&s->ready) ;
        s->nrunning++ ;
        pt_assert(s->nrunning > 0) ;
        pthread_mutex_unlock(&s->mutex) ;

        /* run the thread */
        ret = run->func(run->env) ;
        pthread_mutex_lock(&s->mutex) ;
        pt_assert(s->nrunning > 0) ;
        s->nrunning-- ;
        if (ret.pt_rv == PT_RETURN_DONE) {
            assert(s->nthread) ;
            s->nthread-- ;
        }
    }
    if (s->nthread == 0 && s->ready_function) {
        /* no protothreads left; posix thread may want to exit */
        s->ready_function(s->ready_env) ;
    }

    /* there are more threads to run (or there are no more threads) */
    return s->ready != NULL || s->nthread == 0 ;
}

bool_t
protothread_run(state_t const s)
{
    pthread_mutex_lock(&s->mutex) ;
    bool_t const r = protothread_run_locked(s) ;
    pthread_mutex_unlock(&s->mutex) ;
    return r ;
}

static void
pt_add_ready(state_t const s, pt_thread_t * const t)
{
    pt_assert(pt_mutex_is_locked(&s->mutex)) ;
    pt_link(&s->ready, t) ;
    if (s->ready_function) {
        /* this should schedule protothread_run() */
        s->ready_function(s->ready_env) ;
    }
}

void
protothread_set_ready_function(state_t const s, void (*f)(env_t), env_t env)
{
    s->ready_function = f ;
    s->ready_env = env ;
}

/* This is called by pt_create(), not by user code directly */
void
pt_create_thread(
        state_t const s,
        pt_thread_t * const t,
        pt_func_t * const pt_func,
        pt_f_t const func,
        env_t env
) {
    pt_func->thread = t ;
    pt_func->label = NULL ;
    t->func = func ;
    t->env = env ;
    t->s = s ;
    t->channel = NULL ;
#if PT_DEBUG
    t->pt_func = pt_func ;
    t->next = NULL ;
#endif

    /* add the new thread to the ready list */
    pthread_mutex_lock(&s->mutex) ;
    s->nthread ++ ;
    assert(s->nthread) ;
    pt_add_ready(s, t) ;
    pthread_mutex_unlock(&s->mutex) ;
}

/* Return which wait list to use (hash table) */
static inline pt_thread_t **
pt_get_wait_list(state_t const s, void * chan)
{
    return &s->wait[((uintptr_t)chan >> 4) & (PT_NWAIT-1)] ;
}

/* Make the thread or threads that are waiting on the given
 * channel (if any) runnable.
 */
static void
pt_wake(state_t const s, void * const channel, bool_t const wake_one)
{
    pt_thread_t ** const wq = pt_get_wait_list(s, channel) ;
    pthread_mutex_lock(&s->mutex) ;
    pt_thread_t * prev = *wq ;  /* one before the oldest waiting thread */

    while (*wq) {
        pt_thread_t * const t = prev->next ;
        if (t->channel != channel) {
            /* advance to next thread on wait list */
            prev = t ;
            /* looped back to start? done */
            if (prev == *wq) {
                break ;
            }
        } else {
            /* wake up this thread (link to the ready list) */
            pt_unlink(wq, prev) ;
            pt_add_ready(s, t) ;
            if (wake_one) {
                /* wake only the first found thread */
                break ;
            }
        }
    }
    pthread_mutex_unlock(&s->mutex) ;
}

void
pt_signal(state_t const s, void * const channel)
{
    pt_wake(s, channel, TRUE) ;
}

void
pt_broadcast(state_t const s, void * const channel)
{
    pt_wake(s, channel, FALSE) ;
}

bool_t
pt_kill(pt_thread_t * const t)
{
    state_t const s = t->s ;

    pthread_mutex_lock(&s->mutex) ;
    if (!pt_find_and_unlink(&s->ready, t)) {
        pt_thread_t ** const wq = pt_get_wait_list(s, t->channel) ;
        if (!pt_find_and_unlink(wq, t)) {
            pthread_mutex_unlock(&s->mutex) ;
            return FALSE ;
        }
    }
    pt_assert(s->nthread) ;
    s->nthread -- ;
    pthread_mutex_unlock(&s->mutex) ;
    return TRUE ;
}

/* should only be called by the macro pt_yield() */
void
pt_enqueue_yield(pt_thread_t * const t)
{
    state_t const s = t->s ;
    pthread_mutex_lock(&s->mutex) ;
    /* the current protothread, at least, should be running */
    pt_assert(s->nrunning > 0) ;
    pt_link(&s->ready, t) ;
    pthread_mutex_unlock(&s->mutex) ;
}

/* should only be called by the macro pt_wait() */
void
pt_enqueue_wait(pt_thread_t * const t, void * const channel)
{
    state_t const s = t->s ;
    pt_thread_t ** const wq = pt_get_wait_list(s, channel) ;
    pt_assert(s->nrunning > 0) ;
    t->channel = channel ;
    pt_link(wq, t) ;
    pthread_mutex_unlock(&s->mutex) ;
}
