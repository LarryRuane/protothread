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
protothread_create_maxpt(unsigned int maxpt)
{
    state_t const s = malloc(sizeof(*s)) ;
    protothread_init_maxpt(s, maxpt) ;
    return s ;
}

state_t
protothread_create(void)
{
    return protothread_create_maxpt(1) ;
}

void
protothread_free(state_t const s)
{
    protothread_deinit(s) ;
    free(s) ;
}

void
protothread_init_maxpt(state_t const s, int maxpt)
{
    memset(s, 0, sizeof(*s)) ;
    pthread_mutex_init(&s->mutex, NULL) ;
    s->tid = calloc(maxpt, sizeof(pthread_t)) ;
    s->npthread_max = maxpt ;
}

void
protothread_init(state_t const s)
{
    protothread_init_maxpt(s, 0) ;
}

void
protothread_deinit(state_t const s)
{
    pthread_mutex_lock(&s->mutex) ;
    s->closing = TRUE ;
    /* awaken all idle pthreads so they see closing and exit */
    pthread_cond_broadcast(&s->cond) ;
    pthread_mutex_unlock(&s->mutex) ;
    {
        unsigned int i ;
        for (i = 0; i < s->npthread; i++) {
            pthread_join(s->tid[i], NULL) ;
        }
    }
    free(s->tid) ;
    pt_assert(!s->nthread) ;
    pt_assert(!s->npthread_running) ;

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
        if (n == t) {
            pt_unlink(head, prev) ;
            return TRUE ;
        }
        /* Advance to next thread */
        prev = t ;
        /* looped back to start? finished */
        if (prev == *head) {
            break ;
        }
    }

    return FALSE ;
}

static bool_t
protothread_run(state_t const s)
{
    pt_thread_t * run ;
    pt_t ret ;

    pt_assert(pt_mutex_is_locked(&s->mutex)) ;

    if (s->ready) {
        /* unlink the oldest ready thread */
        run = pt_unlink_oldest(&s->ready) ;
        s->nrunning++ ;
        pt_assert(s->nrunning > 0) ;
        s->nresume++ ; /* debugging/testing */
        pthread_mutex_unlock(&s->mutex) ;

        /* run (resume) the thread */
        ret = run->func(run->env) ;
        pthread_mutex_lock(&s->mutex) ;
        pt_assert(s->nrunning > 0) ;
        s->nrunning-- ;
        if (ret.pt_rv == PT_RETURN_DONE) {
            assert(s->nthread) ;
            s->nthread-- ;
        }
    }
    /* there are more threads to run */
    return s->ready != NULL ;
}

/* wait for the protothread system to be quiesced (no active or ready threads,
 * but there can be threads blocked in pt_wait())
 */
void
protothread_quiesce(state_t const s)
{
    pthread_mutex_lock(&s->mutex) ;
    s->quiescing = TRUE ;
    if (s->npthread_max) {
        while (s->ready || s->nrunning > 0) {
            pthread_cond_wait(&s->cond, &s->mutex) ;
            /* in case we don't need that signal, pass it on */
            pthread_cond_signal(&s->cond) ;
        }
    } else {
        /* there are no pthreads to run protothreads, so we'll do it */
        while (protothread_run(s)) ;
    }
    s->quiescing = FALSE ;
    pthread_mutex_unlock(&s->mutex) ;
}

/* posix pthread thread entry point */
static void *
pt_pthread(env_t env)
{
    state_t const s = env ;
    pthread_mutex_lock(&s->mutex) ;
    s->npthread_running++ ;
    while (TRUE) {
        while (protothread_run(s)) ;
        /* nothing to do, idle */
        pt_assert(s->npthread_running > 0) ;
        s->npthread_running-- ;
        if (s->closing) {
            break ;
        }
        if (s->quiescing) {
            pthread_cond_signal(&s->cond) ;
        }
        pthread_cond_wait(&s->cond, &s->mutex) ;
        s->npthread_running++ ;
    }
    pthread_mutex_unlock(&s->mutex) ;
    return NULL ;
}

/* Make this pt runnable, also create another posix pthread if necessary */
static void
pt_add_ready(state_t const s, pt_thread_t * const t)
{
    pt_assert(pt_mutex_is_locked(&s->mutex)) ;
    pt_link(&s->ready, t) ;

    if (s->npthread_running < s->npthread) {
        /* there are available (idle) pthreads, no need to create another */
        pthread_cond_signal(&s->cond) ;
        return ;
    }
    if (s->npthread >= s->npthread_max) {
        /* we are at the limit, don't create more pthreads */
        pt_assert(s->npthread == s->npthread_max) ;
        return ;
    }
    /* may want to consider creating this pthread without holding mutex */
    pthread_create(&s->tid[s->npthread++], NULL, pt_pthread, s) ;
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
            continue ;
        }
        /* wake up this thread (link to the ready list) */
        pt_unlink(wq, prev) ;
        pt_add_ready(s, t) ;
        if (wake_one) {
            /* wake only the first found thread */
            break ;
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
