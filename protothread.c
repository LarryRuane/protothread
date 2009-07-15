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

/* Number of wait queues (size of wait hash table), power of 2 */
#define NWAIT (1 << 4)
int pt_nwait = NWAIT ;	/* for gdb macros */

/* Usually there is one instance for the overall system. */
typedef struct protothread_s {
    void (*ready_function)(env_t) ; /* function to call when a thread becomes ready */
    env_t ready_env ;		    /* environment to pass to ready_function() */
    pt_thread_t *running ;	    /* current running protothread (if non-NULL) */
    pt_thread_t *ready ;	    /* ready to run list (points to newest) */
    pt_thread_t *wait[NWAIT] ;	    /* waiting for an event (points to newest) */
} *state_t ;

state_t
protothread_create(void)
{
    state_t const s = malloc(sizeof(*s)) ;
    memset(s, 0, sizeof(*s)) ;
    return s ;
}

void
protothread_free(state_t const s)
{
    if (PT_DEBUG) {
	int i ;
	for (i = 0; i < NWAIT; i++) {
	    pt_assert(s->wait[i] == NULL) ;
	}
	pt_assert(s->ready == NULL) ;
	pt_assert(s->running == NULL) ;
    }
    free(s) ;
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

bool_t
protothread_run(state_t const s)
{
    pt_assert(s->running == NULL) ;
    if (s->ready == NULL) {
	return FALSE ;
    }

    /* unlink the oldest ready thread */
    s->running = pt_unlink_oldest(&s->ready) ;

    /* run the thread */
    s->running->func(s->running->env) ;
    s->running = NULL ;

    /* return true if there are more threads to run */
    return s->ready != NULL ;
}

static void
pt_add_ready(state_t const s, pt_thread_t * const t)
{
    if (s->ready_function && !s->ready && !s->running) {
	/* this should schedule protothread_run() */
	s->ready_function(s->ready_env) ;
    }
    pt_link(&s->ready, t) ;
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
#if PT_DEBUG
    t->pt_func = pt_func ;
    t->next = NULL ;
#endif

    /* add the new thread to the ready list */
    pt_add_ready(s, t) ;
}

/* Return which wait list to use (hash table) */
static inline pt_thread_t **
pt_get_wait_list(state_t const s, void * chan)
{
    return &s->wait[((uintptr_t)chan >> 4) & (NWAIT-1)] ;
}

/* Make the thread or threads that are waiting on the given
 * channel (if any) runnable.
 */
static void
pt_wake(state_t const s, void * const channel, bool_t wake_one)
{
    pt_thread_t ** const wq = pt_get_wait_list(s, channel) ;
    pt_thread_t * prev = *wq ;	/* one before the oldest waiting thread */

    if (prev == NULL) {
	return ;
    }
    do {
	pt_thread_t * const t = prev->next ;
	if (t->channel != channel) {
	    /* advance to next thread on wait list */
	    prev = t ;
	    continue ;
	}
	/* wake up this thread (link to the ready list) */
	pt_unlink(wq, prev) ;
	pt_add_ready(s, t) ;
	if (wake_one) {
	    /* wake only the first found thread */
	    break ;
	}
    } while (*wq && prev != *wq) ;
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
    pt_assert(s->running == t) ;
    pt_add_ready(s, t) ;
}

/* should only be called by the macro pt_wait() */
void
pt_enqueue_wait(pt_thread_t * const t, void * const channel)
{
    state_t const s = t->s ;
    pt_thread_t ** const wq = pt_get_wait_list(s, channel) ;
    pt_assert(s->running == t) ;
    t->channel = channel ;
    pt_link(wq, t) ;
}

/* This allows protothreads (which might not have an explicit pointer to the
 * protothread object) to call pt_create(), pt_signal() or pt_broadcast().
 */
protothread_t
pt_get_protothread(pt_func_t const pt_func) {
    return pt_func.thread->s ;
}
