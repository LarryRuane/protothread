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
 * PT_QUEUE_DEFINE(name, type) generates a queue of <type> named name_t, and
 * these functions, which never block:
 *   name_init(q, slots, capacity)       use slots[capacity] as storage
 *   name_try_send(s, q, item_p)         append if room; true if sent
 *   name_try_receive(s, q, item_p)      take oldest if any; true if taken
 *   name_count(q)                       number of items queued
 *   name_at(q, i)                       pointer to the i-th oldest item
 *   name_remove(s, q, i, item_p)        take the i-th oldest, keep order
 *   name_sent(q)                        count of items ever sent; wraps
 *
 * The blocking calls are macros, like pt_wait(), and work on any queue:
 *   pt_queue_send(c, q, item_p)         block while full, then append
 *   pt_queue_receive(c, q, item_p)      block while empty, then take oldest
 *   pt_queue_wait_send(c, q, seen)      block until name_sent(q) != seen
 *
 * pt_queue_t and pt_queue_*() are a predefined queue of void *.
 *
 * Items go in and come out through pointers, so each is copied exactly once
 * each way, by assignment, inside a critical section; for a large structure,
 * consider queueing a pointer to it instead. try_send() may be called from an
 * interrupt handler when the PT_CRITICAL_* macros are defined; everything
 * else is for thread context.
 */

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

/* read with interrupts masked, since on a small target this can be two loads */
static inline unsigned int
pt_i_queue_sent(pt_i_queue_t const * const q)
{
    pt_i_critical_t const saved = PT_CRITICAL_ENTER() ;
    unsigned int const sent = q->sent ;
    PT_CRITICAL_EXIT(saved) ;
    return sent ;
}

/* Append *item_p if there is room, and set ok to whether it was. The copy is
 * by assignment from the queue's own element type, so it is type-checked.
 */
#define pt_i_queue_put(queue, item_p, ok) \
    do { \
        pt_i_critical_t const pt_i_qsaved = PT_CRITICAL_ENTER() ; \
        (ok) = (queue)->q.count < (queue)->q.capacity ; \
        if (ok) { \
            (queue)->slots[pt_i_queue_slot(&(queue)->q, (queue)->q.count)] = \
                *(item_p) ; \
            pt_i_queue_appended(&(queue)->q) ; \
        } \
        PT_CRITICAL_EXIT(pt_i_qsaved) ; \
    } while (0)

/* Take the oldest item into *item_p, if there is one, and set ok to whether
 * there was.
 */
#define pt_i_queue_take(queue, item_p, ok) \
    do { \
        pt_i_critical_t const pt_i_qsaved = PT_CRITICAL_ENTER() ; \
        (ok) = (queue)->q.count > 0 ; \
        if (ok) { \
            *(item_p) = (queue)->slots[(queue)->q.head] ; \
            pt_i_queue_took_oldest(&(queue)->q) ; \
        } \
        PT_CRITICAL_EXIT(pt_i_qsaved) ; \
    } while (0)

/* An instance needn't use every function it generates, and clang warns about
 * unused static functions expanded outside a header.
 */
#define PT_I_MAYBE_UNUSED __attribute__((__unused__))

/* Generate a queue of <type> named name_t, and its non-blocking functions */
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
static inline PT_I_MAYBE_UNUSED unsigned int \
name ## _sent(name ## _t const * const q) \
{ \
    return pt_i_queue_sent(&q->q) ; \
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
    bool_t sent ; \
    pt_i_queue_put(q, item, sent) ; \
    if (sent) { \
        pt_broadcast(s, &q->q.sent) ; \
    } \
    return sent ; \
} \
\
static inline PT_I_MAYBE_UNUSED bool_t \
name ## _try_receive(protothread_t const s, name ## _t * const q, \
        type * const item) \
{ \
    bool_t taken ; \
    pt_i_queue_take(q, item, taken) ; \
    if (taken) { \
        pt_broadcast(s, &q->q.head) ; \
    } \
    return taken ; \
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
}

/* The blocking calls wait in the caller's context, as pt_wait() does, and
 * retry if another protothread or an interrupt got there first. q and item_p
 * are evaluated more than once, so they must not have side effects.
 */
#define pt_queue_send(c, queue, item_p) \
    do { \
        bool_t pt_i_ok ; \
        do { \
            pt_wait_until(c, &(queue)->q.head, \
                (queue)->q.count < (queue)->q.capacity) ; \
            pt_i_queue_put(queue, item_p, pt_i_ok) ; \
        } while (!pt_i_ok) ; \
        pt_broadcast(pt_get_pt(c), &(queue)->q.sent) ; \
    } while (0)

#define pt_queue_receive(c, queue, item_p) \
    do { \
        bool_t pt_i_ok ; \
        do { \
            pt_wait_until(c, &(queue)->q.sent, (queue)->q.count > 0) ; \
            pt_i_queue_take(queue, item_p, pt_i_ok) ; \
        } while (!pt_i_ok) ; \
        pt_broadcast(pt_get_pt(c), &(queue)->q.head) ; \
    } while (0)

/* Read seen from name_sent() before looking at the queue, so that anything
 * sent after that, even by an interrupt, ends the wait.
 */
#define pt_queue_wait_send(c, queue, seen) \
    pt_wait_until(c, &(queue)->q.sent, (queue)->q.sent != (seen))

PT_QUEUE_DEFINE(pt_queue, void *)

#endif /* PROTOTHREAD_QUEUE_H */
