/**************************************************************/
/* PROTOTHREAD_QUEUE.H */
/* https://github.com/LarryRuane/protothread */
/* Copyright (c) 2008-present Larry Ruane */
/* Distributed under the MIT software license, see the accompanying */
/* file LICENSE or https://opensource.org/licenses/MIT. */
/* SPDX-License-Identifier: MIT */
/* Message queues (what Go calls channels) */
/**************************************************************/
#ifndef PROTOTHREAD_QUEUE_H
#define PROTOTHREAD_QUEUE_H

#include "protothread.h"

/* Message queue. The public API; pt_i_ names are internal.
 *
 * PT_QUEUE_DEFINE(name, type) generates a queue of <type> named name_t.
 * The three blocking functions are protothread functions, so like any other
 * they are called through pt_call(), as pt_call(c, name_send, env, q, item):
 *   name_send(queue_env, q, item_p)     block while full, then append
 *   name_receive(queue_env, q, item_p)  block while empty, then take oldest
 *   name_wait_send(queue_env, q)        block until another item is sent
 * The rest never block, and are called directly:
 *   name_init(q, slots, capacity)       use slots[capacity] as storage
 *   name_try_send(s, q, item_p)         append if room; true if sent
 *   name_try_receive(s, q, item_p)      take oldest if any; true if taken
 *   name_count(q)                       number of items queued
 *   name_at(q, i)                       pointer to the i-th oldest item
 *   name_remove(s, q, i, item_p)        take the i-th oldest, keep order
 *   pt_queue_env_t                      one per protothread that blocks
 *
 * pt_queue_t and pt_queue_*() are a predefined queue of void *.
 *
 * Items go in and come out through pointers, so each is copied exactly once
 * each way, by assignment, inside a critical section; for a large structure,
 * consider queueing a pointer to it instead. try_send() may be called from an
 * interrupt handler when the PT_CRITICAL_* macros are defined; everything
 * else is for thread context.
 */

/* One per protothread that blocks on a queue, in its context structure */
typedef struct pt_queue_env_s {
    pt_func_t pt_func ;
    unsigned int seen ;             /* the send count wait_send() started at */
} pt_queue_env_t ;

/* The untyped part of every queue: indices and counts, never the items.
 * Receivers wait on &sent, senders wait on &head, so that a send wakes only
 * receivers and a receive wakes only senders.
 */
typedef struct pt_i_queue_s {
    unsigned int head ;             /* slot of the oldest item */
    unsigned int count ;            /* items queued */
    unsigned int capacity ;         /* slots */
    unsigned int sent ;             /* sends so far, wrapping */
} pt_i_queue_t ;

static inline void
pt_i_queue_init(pt_i_queue_t * const q, unsigned int const capacity)
{
    pt_assert(capacity > 0) ;
    q->head = 0 ;
    q->count = 0 ;
    q->capacity = capacity ;
    q->sent = 0 ;
}

/* the slot holding the i-th oldest item (or, at i == count, the next free) */
static inline unsigned int
pt_i_queue_slot(pt_i_queue_t const * const q, unsigned int const i)
{
    unsigned int const j = q->head + i ;
    return j < q->capacity ? j : j - q->capacity ;
}

/* the caller has just filled the next free slot */
static inline void
pt_i_queue_appended(pt_i_queue_t * const q)
{
    PT_CRITICAL_ASSERT() ;
    q->count++ ;
    q->sent++ ;
}

/* the caller has just closed a gap by moving the older items up by one */
static inline void
pt_i_queue_took_oldest(pt_i_queue_t * const q)
{
    PT_CRITICAL_ASSERT() ;
    q->head = pt_i_queue_slot(q, 1) ;
    q->count-- ;
}

/* the caller has just closed a gap by moving the newer items down by one */
static inline void
pt_i_queue_took_newest(pt_i_queue_t * const q)
{
    PT_CRITICAL_ASSERT() ;
    q->count-- ;
}

/* An instance needn't use every function it generates, and clang warns about
 * unused static functions expanded outside a header.
 */
#define PT_I_MAYBE_UNUSED __attribute__((__unused__))

/* Generate a queue of <type> named name_t, and its functions. The blocking
 * ones are a wait on the untyped part followed by the non-blocking one, and
 * retry if another protothread got there first.
 */
#define PT_QUEUE_DEFINE(name, type) \
\
typedef struct name ## _s { \
    pt_i_queue_t q ; \
    type * slots ; \
} name ## _t ; \
\
static inline PT_I_MAYBE_UNUSED void \
name ## _init(name ## _t * const q, type * const slots, \
        unsigned int const capacity) \
{ \
    pt_i_queue_init(&q->q, capacity) ; \
    q->slots = slots ; \
} \
\
static inline PT_I_MAYBE_UNUSED unsigned int \
name ## _count(name ## _t const * const q) \
{ \
    return q->q.count ; \
} \
\
static inline PT_I_MAYBE_UNUSED type * \
name ## _at(name ## _t * const q, unsigned int const i) \
{ \
    pt_assert(i < q->q.count) ; \
    return &q->slots[pt_i_queue_slot(&q->q, i)] ; \
} \
\
static inline PT_I_MAYBE_UNUSED bool_t \
name ## _try_send(protothread_t const s, name ## _t * const q, \
        type const * const item) \
{ \
    bool_t sent = false ; \
    pt_i_critical_t const saved = PT_CRITICAL_ENTER() ; \
    if (q->q.count < q->q.capacity) { \
        q->slots[pt_i_queue_slot(&q->q, q->q.count)] = *item ; \
        pt_i_queue_appended(&q->q) ; \
        sent = true ; \
    } \
    PT_CRITICAL_EXIT(saved) ; \
    if (sent) { \
        pt_broadcast(s, &q->q.sent) ; \
    } \
    return sent ; \
} \
\
static inline PT_I_MAYBE_UNUSED bool_t \
name ## _remove(protothread_t const s, name ## _t * const q, \
        unsigned int const i, type * const item) \
{ \
    bool_t taken = false ; \
    pt_i_critical_t const saved = PT_CRITICAL_ENTER() ; \
    if (i < q->q.count) { \
        unsigned int k ; \
        if (item) { \
            *item = q->slots[pt_i_queue_slot(&q->q, i)] ; \
        } \
        /* close the gap from whichever side has fewer items to move */ \
        if (i < q->q.count - 1 - i) { \
            for (k = i; k > 0; k--) { \
                q->slots[pt_i_queue_slot(&q->q, k)] = \
                    q->slots[pt_i_queue_slot(&q->q, k - 1)] ; \
            } \
            pt_i_queue_took_oldest(&q->q) ; \
        } else { \
            for (k = i + 1; k < q->q.count; k++) { \
                q->slots[pt_i_queue_slot(&q->q, k - 1)] = \
                    q->slots[pt_i_queue_slot(&q->q, k)] ; \
            } \
            pt_i_queue_took_newest(&q->q) ; \
        } \
        taken = true ; \
    } \
    PT_CRITICAL_EXIT(saved) ; \
    if (taken) { \
        pt_broadcast(s, &q->q.head) ; \
    } \
    return taken ; \
} \
\
static inline PT_I_MAYBE_UNUSED bool_t \
name ## _try_receive(protothread_t const s, name ## _t * const q, \
        type * const item) \
{ \
    return name ## _remove(s, q, 0, item) ; \
} \
\
static inline PT_I_MAYBE_UNUSED pt_t \
name ## _send(pt_queue_env_t * const c, name ## _t * const q, \
        type const * const item) \
{ \
    pt_resume(c) ; \
    for (;;) { \
        pt_wait_until(c, &q->q.head, q->q.count < q->q.capacity) ; \
        if (name ## _try_send(pt_get_pt(c), q, item)) { \
            return PT_DONE ; \
        } \
    } \
} \
\
static inline PT_I_MAYBE_UNUSED pt_t \
name ## _receive(pt_queue_env_t * const c, name ## _t * const q, \
        type * const item) \
{ \
    pt_resume(c) ; \
    for (;;) { \
        pt_wait_until(c, &q->q.sent, q->q.count > 0) ; \
        if (name ## _try_receive(pt_get_pt(c), q, item)) { \
            return PT_DONE ; \
        } \
    } \
} \
\
/* the send count is read with interrupts masked, since on a small target \
 * an unsigned int can take two loads */ \
static inline PT_I_MAYBE_UNUSED pt_t \
name ## _wait_send(pt_queue_env_t * const c, name ## _t * const q) \
{ \
    pt_resume(c) ; \
    { \
        pt_i_critical_t const saved = PT_CRITICAL_ENTER() ; \
        c->seen = q->q.sent ; \
        PT_CRITICAL_EXIT(saved) ; \
    } \
    pt_wait_until(c, &q->q.sent, q->q.sent != c->seen) ; \
    return PT_DONE ; \
}

PT_QUEUE_DEFINE(pt_queue, void *)

#endif /* PROTOTHREAD_QUEUE_H */
