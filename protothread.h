/**************************************************************/
/* PROTOTHREAD.H */
/* https://github.com/LarryRuane/protothread */
/* Copyright (c) 2008-present Larry Ruane */
/* Distributed under the MIT software license, see the accompanying */
/* file LICENSE or https://opensource.org/licenses/MIT. */
/* SPDX-License-Identifier: MIT */
/**************************************************************/
#ifndef PROTOTHREAD_H
#define PROTOTHREAD_H 1
/* Only freestanding headers are included unconditionally, so this library
 * can be used on a bare-metal target with no C library at all. See the
 * "Configuration" section of README.md.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Version, semantic versioning (https://semver.org). This is the single
 * source of truth; the build reads it from here. PT_VERSION_NUMBER is
 * ordered, so a vendored copy can be tested at compile time:
 *
 *   #if !defined(PT_VERSION_NUMBER) || !PT_VERSION_AT_LEAST(2, 0, 0)
 *   #error protothread 2.0.0 or later is required
 *   #endif
 */
#define PT_VERSION_MAJOR 2
#define PT_VERSION_MINOR 0
#define PT_VERSION_PATCH 0

#define PT_VERSION_NUMBER \
    (PT_VERSION_MAJOR * 10000 + PT_VERSION_MINOR * 100 + PT_VERSION_PATCH)
#define PT_VERSION_AT_LEAST(major, minor, patch) \
    (PT_VERSION_NUMBER >= ((major) * 10000 + (minor) * 100 + (patch)))

/* derived, so the string can never drift from the numbers */
#define PT_STRINGIFY_HELP(x) #x
#define PT_STRINGIFY(x) PT_STRINGIFY_HELP(x)
#define PT_VERSION_STRING \
    PT_STRINGIFY(PT_VERSION_MAJOR) "." \
    PT_STRINGIFY(PT_VERSION_MINOR) "." \
    PT_STRINGIFY(PT_VERSION_PATCH)

#ifndef PT_DEBUG
#define PT_DEBUG 1  /* enabled (else 0) */
#endif

/* Define pt_assert() yourself to avoid <assert.h> entirely. The PT_DEBUG=0
 * form still type-checks the expression without evaluating it.
 */
#ifndef pt_assert
#if PT_DEBUG
#include <assert.h>
#define pt_assert(condition) assert(condition)
#else
#define pt_assert(condition) do { (void)sizeof((condition)) ; } while (0)
#endif
#endif

/* standard definitions */
typedef bool bool_t ;
typedef void * env_t ;

/* Number of wait queues (size of wait hash table), power of 2. Each entry
 * costs one pointer, so the default costs 8KB (64-bit) per protothread_t.
 * Set PT_NWAIT to 1 on a memory-constrained system: a single linear wait
 * list beats hashing when there are only a handful of waiters.
 */
#ifndef PT_NWAIT
#define PT_NWAIT (1 << 10)
#endif

/* Interrupt safety.
 *
 * The scheduler's lists are updated with several stores that are not atomic
 * with respect to an interrupt handler. By default this library is NOT
 * interrupt-safe: pt_signal(), pt_broadcast() and pt_kill() must be called
 * from thread context only. Calling them from an interrupt handler can
 * silently and permanently orphan a protothread.
 *
 * Define these to disable and restore interrupts to make those calls safe.
 * They nest, so EXIT must restore the saved state rather than
 * unconditionally enable. On Cortex-M with CMSIS:
 *
 *   static inline uint32_t pt_critical_enter(void) {
 *       uint32_t s = __get_PRIMASK() ; __disable_irq() ; return s ; }
 *   #define PT_CRITICAL_T       uint32_t
 *   #define PT_CRITICAL_ENTER() pt_critical_enter()
 *   #define PT_CRITICAL_EXIT(s) __set_PRIMASK(s)
 *
 * The critical sections are short and O(1), except that pt_wake() and
 * pt_kill() walk one wait list.
 */
#ifndef PT_CRITICAL_T
#define PT_CRITICAL_T int
#endif
typedef PT_CRITICAL_T pt_critical_t ;

#ifndef PT_CRITICAL_ENTER
#define PT_CRITICAL_ENTER() 0
#endif
#ifndef PT_CRITICAL_EXIT
#define PT_CRITICAL_EXIT(saved) ((void)(saved))
#endif

/* Function return values; hide things a bit so user can't
 * accidentally return a NULL or an integer.
 */
enum pt_return_e {
    PT_RETURN_WAIT,
    PT_RETURN_DONE,
} ;

typedef struct pt_return_s {
    enum pt_return_e pt_rv ;
} pt_t ;

/* pointer to a top-level protothread function
 */
typedef pt_t (*pt_f_t)(env_t) ;

/* One per thread:
 */
struct pt_thread_s {
    struct pt_thread_s * next ;         /* next thread in wait or run list */
    pt_f_t func ;                       /* top level function */
    env_t env ;                         /* top level function's context */
    void *channel ;                     /* if waiting (never dereferenced) */
    struct protothread_s * s ;          /* pointer to state */
    void (*atexit)(env_t env) ;         /* optional user defined destructor */
#if PT_DEBUG
    struct pt_func_s * pt_func ;        /* top-level function's pt_func_t */
#endif
} ;
typedef struct pt_thread_s pt_thread_t ;

/* Usually there is one instance of struct protothread_s for
 * the overall system.
 */
typedef struct protothread_s {
    void (*ready_function)(env_t) ; /* function to call when a thread becomes ready */
    env_t ready_env ;               /* environment to pass to ready_function() */
    pt_thread_t *running ;          /* current running protothread (if non-NULL) */
    pt_thread_t *ready ;            /* ready to run list (points to newest) */
    pt_thread_t *wait[PT_NWAIT] ;   /* waiting for an event (points to newest) */
} *protothread_t ;

typedef struct protothread_s *state_t ;

static inline pt_t
pt_return_wait(void) {
    pt_t p ;
    p.pt_rv = PT_RETURN_WAIT ;
    return p ;
}

static inline pt_t
pt_return_done(void) {
    pt_t p ;
    p.pt_rv = PT_RETURN_DONE ;
    return p ;
}

#define PT_WAIT pt_return_wait()
#define PT_DONE pt_return_done()


/* One of these per nested function (call frame); every function environment
 * struct must contain one of these.
 */
typedef struct pt_func_s {
    pt_thread_t * thread ;
    void *label ;                   /* function resume point (goto target) */
#if PT_DEBUG
    struct pt_func_s * next ;       /* pt_func of function that we called */
    char const * file ;             /* __FILE__ */
    int line ;                      /* __LINE__ */
    char const * function ;         /* __FUNCTION__ */
#endif
} pt_func_t ;

/* This should be at the beginning of every protothread function */
#define pt_resume(c) do { if ((c)->pt_func.label) goto *(c)->pt_func.label ; } while (0)

/* This can be used to reset a thread or thread function */
#define pt_reset(c) do { (c)->pt_func.label = NULL ; } while (0)

/* link thread as the newest in the given (ready or wait) list */
static inline void
pt_link(pt_thread_t ** const head, pt_thread_t * const n)
{
    const pt_critical_t saved = PT_CRITICAL_ENTER() ;
    if (*head) {
        n->next = (*head)->next ;
        (*head)->next = n ;
    } else {
        n->next = n ;
    }
    *head = n ;
    PT_CRITICAL_EXIT(saved) ;
}

/* unlink and return the thread following prev, updating head if necessary */
static inline pt_thread_t *
pt_unlink(pt_thread_t ** const head, pt_thread_t * const prev)
{
    const pt_critical_t saved = PT_CRITICAL_ENTER() ;
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
    PT_CRITICAL_EXIT(saved) ;
    return next ;
}

/* unlink and return the oldest (last) thread */
static inline pt_thread_t *
pt_unlink_oldest(pt_thread_t ** const head)
{
    return pt_unlink(head, *head) ;
}

/* finds thread <n> in list <head> and unlinks it. Returns TRUE if
 * it was found.
 */
static inline bool_t
pt_find_and_unlink(pt_thread_t ** const head, pt_thread_t * const n)
{
    const pt_critical_t saved = PT_CRITICAL_ENTER() ;
    pt_thread_t * prev = *head ;

    while (*head) {
        pt_thread_t * const t = prev->next ;
        if (n == t) {
            pt_unlink(head, prev) ;
            PT_CRITICAL_EXIT(saved) ;
            return true ;
        }
        /* Advance to next thread */
        prev = t ;
        /* looped back to start? finished */
        if (prev == *head) {
            break ;
        }
    }
    PT_CRITICAL_EXIT(saved) ;
    return false ;
}

static inline void
pt_add_ready(state_t const s, pt_thread_t * const t)
{
    const pt_critical_t saved = PT_CRITICAL_ENTER() ;
    const bool_t notify = (s->ready_function && !s->ready && !s->running) ;
    pt_link(&s->ready, t) ;
    PT_CRITICAL_EXIT(saved) ;

    if (notify) {
        /* this should schedule protothread_run() */
        s->ready_function(s->ready_env) ;
    }
}

/* This is called by pt_create(), not by user code directly */
static inline void
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
    t->atexit = NULL ;
#if PT_DEBUG
    t->pt_func = pt_func ;
    t->next = NULL ;
#endif

    /* add the new thread to the ready list */
    pt_add_ready(s, t) ;
}

/* sets a user defined callback for finalization at the end of pt_kill() */
static inline void
pt_set_atexit(pt_thread_t * pt, void (*func)(env_t)) {
    pt->atexit = func ;
}

/* should only be called by the macro pt_yield() */
static inline void
pt_enqueue_yield(pt_thread_t * const t)
{
    state_t const s = t->s ;
    pt_assert(s->running == t) ;
    pt_add_ready(s, t) ;
}

/* Return which wait list to use (hash table) */
static inline pt_thread_t **
pt_get_wait_list(state_t const s, void * chan)
{
    return &s->wait[((uintptr_t)chan >> 4) & (PT_NWAIT-1)] ;
}

/* should only be called by the macro pt_wait() */
static inline void
pt_enqueue_wait(pt_thread_t * const t, void * const channel)
{
    state_t const s = t->s ;
    pt_thread_t ** const wq = pt_get_wait_list(s, channel) ;
    const pt_critical_t saved = PT_CRITICAL_ENTER() ;
    pt_assert(s->running == t) ;
    t->channel = channel ;
    pt_link(wq, t) ;
    PT_CRITICAL_EXIT(saved) ;
}

/* Construct goto labels using the current line number (so they are unique). */
#define PT_LABEL_HELP2(line) pt_label_ ## line
#define PT_LABEL_HELP(line) PT_LABEL_HELP2(line)
#define PT_LABEL PT_LABEL_HELP(__LINE__)

#if !PT_DEBUG
#define pt_debug_save(env)
#define pt_debug_wait(env)
#define pt_debug_call(env, child_env)
#else
#define pt_debug_save(env) do { \
    (env)->pt_func.file = __FILE__ ; \
    (env)->pt_func.line = __LINE__ ; \
    (env)->pt_func.function = __func__ ; \
} while (0)

#define pt_debug_wait(env) do { \
    pt_debug_save(env) ; \
    (env)->pt_func.next = NULL ; \
} while (0)

#define pt_debug_call(env, child_env) do { \
    pt_debug_save(env) ; \
    (env)->pt_func.next = &(child_env)->pt_func ; \
} while (0)

#endif

/* Wait for a channel to be signaled */
#define pt_wait(env, channel) \
    do { \
        (env)->pt_func.label = &&PT_LABEL ; \
        pt_enqueue_wait((env)->pt_func.thread, channel) ; \
        pt_debug_wait(env) ; \
        return PT_WAIT ; \
      PT_LABEL: ; \
    } while (0)

/* Let other ready protothreads run, then resume this thread */
#define pt_yield(env) \
    do { \
        (env)->pt_func.label = &&PT_LABEL ; \
        pt_enqueue_yield((env)->pt_func.thread) ; \
        pt_debug_wait(env) ; \
        return PT_WAIT ; \
      PT_LABEL: ; \
    } while (0)

/* Call a function (which may wait) */
#define pt_call(env, child_func, child_env, ...) \
    do { \
        (child_env)->pt_func.thread = (env)->pt_func.thread ; \
        (child_env)->pt_func.label = NULL ; \
        (env)->pt_func.label = NULL ; \
        pt_debug_call(env, child_env) ; \
      PT_LABEL: \
        if (child_func(child_env, ##__VA_ARGS__).pt_rv == PT_WAIT.pt_rv) { \
            (env)->pt_func.label = &&PT_LABEL ; \
            return PT_WAIT ; \
        } \
    } while (0)

/* Did the most recent pt_call() block (break context)? */
#define pt_call_waited(env) ((env)->pt_func.label != NULL)

#define pt_create(pt, thr, func, env) \
    pt_create_thread(pt, thr, &(env)->pt_func, func, env)

/* This allows protothreads (which might not have an explicit pointer to the
 * protothread object) to call pt_create(), pt_signal() or pt_broadcast().
 */
static inline protothread_t
pt_get_protothread(pt_func_t const * pt_func) {
    return pt_func->thread->s ;
}
#define pt_get_pt(env) pt_get_protothread(&(env)->pt_func)

static inline void
protothread_init(state_t const s)
{
    int i ;
    s->ready_function = NULL ;
    s->ready_env = NULL ;
    s->running = NULL ;
    s->ready = NULL ;
    for (i = 0; i < PT_NWAIT; i++) {
        s->wait[i] = NULL ;
    }
}

static inline void
protothread_deinit(state_t const s)
{
    (void)s ;
    if (PT_DEBUG) {
        int i ;
        for (i = 0; i < PT_NWAIT; i++) {
            pt_assert(s->wait[i] == NULL) ;
        }
        pt_assert(s->ready == NULL) ;
        pt_assert(s->running == NULL) ;
    }
}

/* Dynamic allocation is optional; define PT_NO_MALLOC to drop <stdlib.h>
 * and these two functions, and use protothread_init() on static storage.
 */
#ifndef PT_NO_MALLOC
#include <stdlib.h>

static inline state_t
protothread_create(void)
{
    state_t const s = malloc(sizeof(*s)) ;
    if (s) {
        protothread_init(s) ;
    }
    return s ;
}

static inline void
protothread_free(state_t const s)
{
    protothread_deinit(s) ;
    free(s) ;
}
#endif /* PT_NO_MALLOC */

static inline bool_t
protothread_run(state_t const s)
{
    pt_critical_t saved ;

    pt_assert(s->running == NULL) ;
    saved = PT_CRITICAL_ENTER() ;
    if (s->ready == NULL) {
        PT_CRITICAL_EXIT(saved) ;
        return false ;
    }

    /* unlink the oldest ready thread */
    s->running = pt_unlink_oldest(&s->ready) ;
    PT_CRITICAL_EXIT(saved) ;

    /* run the thread */
    s->running->func(s->running->env) ;
    s->running = NULL ;

    /* return true if there are more threads to run */
    return s->ready != NULL ;
}

/* Set a function to call when a protothread becomes ready. 
 * This is optional. The passed function will generally
 * schedule a function that will call prothread_run() repeatedly
 * until it returns FALSE (or, if it limits the number of calls
 * and the last call to protothread_run() returned TRUE, it
 * must reschedule itself).
 */
static inline void
protothread_set_ready_function(state_t const s, void (*f)(env_t), env_t env)
{
    s->ready_function = f ;
    s->ready_env = env ;
}

/* Make the thread or threads that are waiting on the given
 * channel (if any) runnable.
 */
static inline void
pt_wake(state_t const s, void * const channel, bool_t const wake_one)
{
    pt_thread_t ** const wq = pt_get_wait_list(s, channel) ;
    const pt_critical_t saved = PT_CRITICAL_ENTER() ;
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
    PT_CRITICAL_EXIT(saved) ;
}

static inline void
pt_signal(state_t const s, void * const channel)
{
    pt_wake(s, channel, true) ;
}

static inline void
pt_broadcast(state_t const s, void * const channel)
{
    pt_wake(s, channel, false) ;
}

/* This is used to prevent a thread from scheduling again. This can be
 * very dangerous if the thread in question isn't written to expect this
 * operation.
 */
static inline bool_t
pt_kill(pt_thread_t * const t)
{
    state_t const s = t->s ;
    pt_critical_t saved ;
    bool_t killed ;

    pt_assert(s->running != t) ;

    saved = PT_CRITICAL_ENTER() ;
    killed = pt_find_and_unlink(&s->ready, t) ;
    if (!killed) {
        killed = pt_find_and_unlink(pt_get_wait_list(s, t->channel), t) ;
    }
    PT_CRITICAL_EXIT(saved) ;

    if (killed && t->atexit) {
        t->atexit(t->env) ;
    }
    return killed ;
}
#endif
