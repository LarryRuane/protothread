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

/* One per sleeping protothread, in its context structure (like pt_func_t) */
typedef struct pt_timer_env_s {
    pt_func_t pt_func ;
    struct pt_timer_env_s *next ;  /* deadline-sorted list, soonest first */
    pt_time_t deadline ;
    bool_t expired ;
} pt_timer_env_t ;

/* One per clock; usually one for the whole system */
typedef struct pt_timers_s {
    pt_timer_env_t *head ;          /* soonest deadline, or NULL */
    pt_time_t now ;                 /* time of the last pt_timer_run() */
} pt_timers_t ;

static inline void
pt_timers_init(pt_timers_t *timers, pt_time_t now)
{
    timers->head = NULL ;
    timers->now = now ;
}

/* insert into the deadline-sorted list, behind any equal deadlines */
static inline void
pt_i_timer_insert(pt_timers_t *timers, pt_timer_env_t *c)
{
    pt_timer_env_t **pp = &timers->head ;
    const pt_i_critical_t saved = PT_CRITICAL_ENTER() ;

    while (*pp && !pt_time_after((*pp)->deadline, c->deadline)) {
        pp = &(*pp)->next ;
    }
    c->next = *pp ;
    *pp = c ;
    PT_CRITICAL_EXIT(saved) ;
}

/* Take a sleeper off the list WITHOUT waking it; it stays blocked. Returns
 * TRUE if it was still pending. Needed before freeing or pt_kill()ing a
 * protothread that might be sleeping.
 */
static inline bool_t
pt_timer_cancel(pt_timers_t *timers, pt_timer_env_t *c)
{
    pt_timer_env_t **pp ;
    bool_t found = false ;
    const pt_i_critical_t saved = PT_CRITICAL_ENTER() ;

    for (pp = &timers->head; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next ;
            c->next = NULL ;
            found = true ;
            break ;
        }
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
    timers->now = now ;
    for (;;) {
        pt_timer_env_t *c ;
        const pt_i_critical_t saved = PT_CRITICAL_ENTER() ;

        c = timers->head ;
        if (c == NULL || pt_time_after(c->deadline, now)) {
            PT_CRITICAL_EXIT(saved) ;
            break ;
        }
        timers->head = c->next ;
        c->next = NULL ;
        c->expired = true ;
        PT_CRITICAL_EXIT(saved) ;

        /* outside the critical section: this walks a wait list */
        pt_broadcast(s, c) ;
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

    pending = (timers->head != NULL) ;
    if (pending) {
        *deadline = timers->head->deadline ;
    }
    PT_CRITICAL_EXIT(saved) ;
    return pending ;
}

static inline pt_t
pt_i_sleep(pt_timer_env_t *c, pt_timers_t *timers, pt_time_t ticks)
{
    pt_resume(c) ;
    c->expired = false ;
    c->deadline = timers->now + ticks ;
    pt_i_timer_insert(timers, c) ;
    /* pt_timer_run() may be called from a tick interrupt */
    pt_i_wait_while(c, c, !c->expired) ;
    return PT_DONE ;
}

/* Block for <ticks>, measured from the last pt_timer_run() */
#define pt_sleep(c, timer_env, timers, ticks) \
    pt_call(c, pt_i_sleep, timer_env, timers, ticks)

#endif /* PROTOTHREAD_TIMER_H */
