/**************************************************************/
/* PROTOTHREAD_TIMER.H */
/* https://github.com/LarryRuane/protothread */
/* Copyright (c) 2008-present Larry Ruane */
/* Distributed under the MIT software license, see the accompanying */
/* file LICENSE or https://opensource.org/licenses/MIT. */
/* SPDX-License-Identifier: MIT */
/**************************************************************/
#ifndef PROTOTHREAD_TIMER_H
#define PROTOTHREAD_TIMER_H

#include "protothread.h"

/* Sleeping. The public API; pt_i_ names are internal.
 *   pt_timers_init(timers, now)             initialize a timer set
 *   pt_sleep(c, timer_env, timers, ticks)   block for <ticks> of your clock
 *   pt_timer_run(s, timers, now)            wake all now due; call on a tick
 *   pt_timer_cancel(timers, timer_env)      forget a sleeper; true if pending
 *   pt_timer_next(timers, deadline)         soonest deadline, for an idle loop
 *   pt_time_after(a, b)                     wraparound-safe time comparison
 *   pt_timers_t, pt_timer_env_t, pt_time_t  set, env per sleeper, clock
 */

/* Sleeping for a while.
 *
 * This library never reads a clock; there is no portable one, and depending
 * on it would cost the freestanding property. Instead you drive it: call
 * pt_timer_run() from wherever you already know the time (a tick interrupt,
 * a SysTick handler, or the idle loop), and it wakes whatever is due.
 *
 * A sleep is measured from the most recent pt_timer_run() call, so the
 * resolution of pt_sleep() is your tick period.
 *
 * pt_timer_run() is safe to call from an interrupt handler if the
 * PT_CRITICAL_* macros are defined (see protothread.h).
 */

/* The clock's type. It may wrap; see pt_time_after() below. Override both
 * of these together, as a matched unsigned/signed pair.
 */
#ifndef PT_TIME_T
#define PT_TIME_T uint32_t
#endif
#ifndef PT_TIME_DIFF_T
#define PT_TIME_DIFF_T int32_t
#endif
typedef PT_TIME_T pt_time_t ;
typedef PT_TIME_DIFF_T pt_i_time_diff_t ;

/* Compare two times, correctly across a counter wraparound. The subtraction
 * wraps, and reading the result as signed recovers the true ordering. This
 * requires that no live deadline is more than half the counter range in the
 * future: about 24 days for a 32-bit millisecond clock.
 */
static inline bool_t
pt_time_after(pt_time_t a, pt_time_t b)
{
    return (pt_i_time_diff_t)(a - b) > 0 ;
}

/* Most protothreads that can sleep at once on one pt_timers_t; a sleeper
 * that finds them all in use waits for one to free up.
 */
#ifndef PT_NTIMERS
#define PT_NTIMERS 16
#endif
#if (PT_NTIMERS) < 1
#error PT_NTIMERS must be at least 1
#endif

/* One per sleeping protothread, in its context structure (like pt_func_t) */
typedef struct pt_timer_env_s {
    pt_func_t pt_func ;
    pt_time_t deadline ;
    unsigned int slot ;             /* index in the heap while pending */
    bool_t expired ;
} pt_timer_env_t ;

/* One per clock; usually one for the whole system */
typedef struct pt_timers_s {
    pt_time_t now ;                 /* time of the last pt_timer_run() */
    unsigned int n ;                /* sleepers in the heap */
    bool_t full_wait ;              /* a sleeper is waiting for a slot */
    pt_timer_env_t *heap[PT_NTIMERS] ;  /* binary min-heap by deadline */
} pt_timers_t ;

static inline void
pt_timers_init(pt_timers_t *timers, pt_time_t now)
{
    timers->now = now ;
    timers->n = 0 ;
    timers->full_wait = false ;
}

/* The heap helpers below are called with interrupts masked */

static inline void
pt_i_timer_place(pt_timers_t *timers, unsigned int i, pt_timer_env_t *c)
{
    timers->heap[i] = c ;
    c->slot = i ;
}

static inline void
pt_i_timer_sift_up(pt_timers_t *timers, unsigned int i)
{
    pt_timer_env_t * const c = timers->heap[i] ;

    while (i > 0) {
        unsigned int const parent = (i - 1) / 2 ;
        if (!pt_time_after(timers->heap[parent]->deadline, c->deadline)) {
            break ;
        }
        pt_i_timer_place(timers, i, timers->heap[parent]) ;
        i = parent ;
    }
    pt_i_timer_place(timers, i, c) ;
}

static inline void
pt_i_timer_sift_down(pt_timers_t *timers, unsigned int i)
{
    pt_timer_env_t * const c = timers->heap[i] ;

    for (;;) {
        unsigned int child = 2 * i + 1 ;
        if (child >= timers->n) {
            break ;
        }
        if (child + 1 < timers->n &&
                pt_time_after(timers->heap[child]->deadline,
                              timers->heap[child + 1]->deadline)) {
            child++ ;
        }
        if (!pt_time_after(c->deadline, timers->heap[child]->deadline)) {
            break ;
        }
        pt_i_timer_place(timers, i, timers->heap[child]) ;
        i = child ;
    }
    pt_i_timer_place(timers, i, c) ;
}

static inline void
pt_i_timer_remove(pt_timers_t *timers, unsigned int i)
{
    timers->n-- ;
    if (i < timers->n) {
        pt_i_timer_place(timers, i, timers->heap[timers->n]) ;
        if (i > 0 && pt_time_after(timers->heap[(i - 1) / 2]->deadline,
                                   timers->heap[i]->deadline)) {
            pt_i_timer_sift_up(timers, i) ;
        } else {
            pt_i_timer_sift_down(timers, i) ;
        }
    }
}

/* the heap is full; tell pt_timer_run() someone is waiting for a slot */
static inline bool_t
pt_i_timers_full(pt_timers_t *timers)
{
    if (timers->n < PT_NTIMERS) {
        return false ;
    }
    timers->full_wait = true ;
    return true ;
}

static inline void
pt_i_timer_insert(pt_timers_t *timers, pt_timer_env_t *c)
{
    const pt_i_critical_t saved = PT_CRITICAL_ENTER() ;

    timers->heap[timers->n] = c ;
    timers->n++ ;
    pt_i_timer_sift_up(timers, timers->n - 1) ;
    PT_CRITICAL_EXIT(saved) ;
}

/* Take a sleeper off the heap WITHOUT waking it; it stays blocked. Returns
 * TRUE if it was still pending. Needed before freeing or pt_kill()ing a
 * protothread that might be sleeping.
 */
static inline bool_t
pt_timer_cancel(pt_timers_t *timers, pt_timer_env_t *c)
{
    bool_t found = false ;
    const pt_i_critical_t saved = PT_CRITICAL_ENTER() ;

    /* c->slot may be stale or never set; the identity check covers both */
    if (c->slot < timers->n && timers->heap[c->slot] == c) {
        pt_i_timer_remove(timers, c->slot) ;
        found = true ;
    }
    PT_CRITICAL_EXIT(saved) ;
    return found ;
}

/* Wake every protothread whose deadline has arrived. Call this whenever
 * the time changes; <now> need not advance by only one tick.
 */
static inline void
pt_timer_run(protothread_t const s, pt_timers_t *timers, pt_time_t now)
{
    bool_t slot_free ;
    pt_i_critical_t saved ;

    timers->now = now ;
    for (;;) {
        pt_timer_env_t *c ;
        saved = PT_CRITICAL_ENTER() ;

        if (timers->n == 0 || pt_time_after(timers->heap[0]->deadline, now)) {
            PT_CRITICAL_EXIT(saved) ;
            break ;
        }
        c = timers->heap[0] ;
        pt_i_timer_remove(timers, 0) ;
        c->expired = true ;
        PT_CRITICAL_EXIT(saved) ;

        /* outside the critical section: this walks a wait list */
        pt_broadcast(s, c) ;
    }

    /* a slot freed by this run or by pt_timer_cancel() */
    saved = PT_CRITICAL_ENTER() ;
    slot_free = timers->full_wait && timers->n < PT_NTIMERS ;
    if (slot_free) {
        timers->full_wait = false ;
    }
    PT_CRITICAL_EXIT(saved) ;
    if (slot_free) {
        pt_broadcast(s, &timers->full_wait) ;
    }
}

/* The soonest deadline, or FALSE if nothing is sleeping. Use this in an
 * idle loop to decide how long the CPU can be stopped.
 */
static inline bool_t
pt_timer_next(pt_timers_t const *timers, pt_time_t *deadline)
{
    bool_t pending ;
    const pt_i_critical_t saved = PT_CRITICAL_ENTER() ;

    pending = (timers->n > 0) ;
    if (pending) {
        *deadline = timers->heap[0]->deadline ;
    }
    PT_CRITICAL_EXIT(saved) ;
    return pending ;
}

static inline pt_t
pt_i_sleep(pt_timer_env_t *c, pt_timers_t *timers, pt_time_t ticks)
{
    pt_resume(c) ;
    /* pt_timer_run() may be called from a tick interrupt */
    pt_i_wait_while(c, &timers->full_wait, pt_i_timers_full(timers)) ;
    c->expired = false ;
    c->deadline = timers->now + ticks ;
    pt_i_timer_insert(timers, c) ;
    pt_i_wait_while(c, c, !c->expired) ;
    return PT_DONE ;
}

/* Block for <ticks>, measured from the last pt_timer_run() */
#define pt_sleep(c, timer_env, timers, ticks) \
    pt_call(c, pt_i_sleep, timer_env, timers, ticks)

#endif /* PROTOTHREAD_TIMER_H */
