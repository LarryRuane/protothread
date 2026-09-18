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

/* THE PUBLIC API, in full. Internal names all carry a pt_i_ or PT_I_ prefix
 * and may change in any release; a name without that prefix is one you can
 * call and rely on.
 *
 * The scheduler
 *   protothread_init(s)              initialize a protothread_t the caller allocated
 *   protothread_deinit(s)            check nothing is still scheduled (PT_DEBUG only)
 *   protothread_create()             allocate and initialize one (needs malloc)
 *   protothread_free(s)              deinitialize and free it
 *   protothread_run(s)               run one ready protothread; true if more remain
 *   protothread_set_ready_function(s, f, env)
 *                                    called when the run list becomes non-empty
 *
 * Inside a protothread function; c is the context, and all of these are macros
 *   pt_resume(c)                     first statement of every protothread function
 *   pt_wait(c, channel)              block until channel is signalled
 *   pt_yield(c)                      let other ready protothreads run, then continue
 *   pt_call(c, func, child_c, ...)   call a protothread function that may block
 *   pt_call_waited(c)                did that pt_call() block?
 *   pt_reset(c)                      forget the resume point; start again from the top
 *   pt_get_pt(c)                     the protothread_t this protothread belongs to
 *   PT_DONE                          the value a protothread function returns when done
 *
 * Creating, waking and killing
 *   pt_create(s, thread, func, env)  create a protothread and make it ready
 *   pt_signal(s, channel)            make the oldest waiter on channel ready
 *   pt_broadcast(s, channel)         make every waiter on channel ready
 *   pt_kill(thread)                  unschedule one; true if it was still scheduled
 *
 * Types            protothread_t, pt_thread_t, pt_func_t, pt_t, pt_f_t, env_t, bool_t
 * Configuration    PT_DEBUG, PT_NWAIT, PT_NO_MALLOC, PT_CRITICAL_*, PT_SIGNAL_WAKES_ALL, pt_assert
 * Version          PT_VERSION_{MAJOR,MINOR,PATCH,NUMBER,STRING}, PT_VERSION_AT_LEAST
 * Companions       protothread_sem.h, protothread_lock.h, protothread_timer.h
 */

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
#define PT_I_STRINGIFY_HELP(x) #x
#define PT_I_STRINGIFY(x) PT_I_STRINGIFY_HELP(x)
#define PT_VERSION_STRING \
    PT_I_STRINGIFY(PT_VERSION_MAJOR) "." \
    PT_I_STRINGIFY(PT_VERSION_MINOR) "." \
    PT_I_STRINGIFY(PT_VERSION_PATCH)

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
#if (PT_NWAIT) < 1 || ((PT_NWAIT) & ((PT_NWAIT) - 1)) != 0
#error PT_NWAIT must be a power of two, and at least 1
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
 * The critical sections are short and O(1), except that pt_signal(),
 * pt_broadcast() and pt_kill() walk one wait list.
 */
#ifndef PT_CRITICAL_T
#define PT_CRITICAL_T int
#endif
typedef PT_CRITICAL_T pt_i_critical_t ;

#ifndef PT_CRITICAL_ENTER
#define PT_CRITICAL_ENTER() 0
#endif
#ifndef PT_CRITICAL_EXIT
#define PT_CRITICAL_EXIT(saved) ((void)(saved))
#endif

/* Several internal functions require the caller to already be in a critical
 * section. That is a comment and nothing more, since the macros above are
 * no-ops by default. Define this to check it -- on a target where entering a
 * critical section is observable, or as CI does, with a depth counter.
 */
#ifndef PT_CRITICAL_ASSERT
#define PT_CRITICAL_ASSERT() do { } while (0)
#endif

/* Define as 1 to make pt_signal() wake every waiter; correct code must still work. */
#ifndef PT_SIGNAL_WAKES_ALL
#define PT_SIGNAL_WAKES_ALL 0
#endif

/* Function return values; hide things a bit so user can't
 * accidentally return a NULL or an integer.
 */
enum pt_i_return_e {
    PT_I_RETURN_WAIT,
    PT_I_RETURN_DONE,
} ;

typedef struct pt_i_return_s {
    enum pt_i_return_e pt_rv ;
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
    bool_t joinable ;                   /* pt_join() may be called on this */
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


static inline pt_t
pt_i_return_wait(void) {
    pt_t p ;
    p.pt_rv = PT_I_RETURN_WAIT ;
    return p ;
}

static inline pt_t
pt_i_return_done(void) {
    pt_t p ;
    p.pt_rv = PT_I_RETURN_DONE ;
    return p ;
}

#define PT_I_WAIT pt_i_return_wait()
#define PT_DONE pt_i_return_done()


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

/* This should be at the beginning of every protothread function.
 *
 * The dead branch exists for clang, which rejects an indirect goto in a
 * function that contains no address-of-label expression: without it, a
 * protothread function that never blocks (so has no pt_wait(), pt_yield()
 * or pt_call() to supply one) fails to compile. gcc accepts either form.
 */
#define pt_resume(c) do { \
    __label__ pt_never ; \
    if (0) { pt_never: (void)&&pt_never ; } \
    if ((c)->pt_func.label) goto *(c)->pt_func.label ; \
} while (0)

/* This can be used to reset a thread or thread function */
#define pt_reset(c) do { (c)->pt_func.label = NULL ; } while (0)

/* Link thread as the newest in the given (ready or wait) list.
 * The caller must already be in a critical section.
 */
static inline void
pt_i_link(pt_thread_t ** const head, pt_thread_t * const n)
{
    PT_CRITICAL_ASSERT() ;
    if (*head) {
        n->next = (*head)->next ;
        (*head)->next = n ;
    } else {
        n->next = n ;
    }
    *head = n ;
}

/* Unlink and return the thread following prev, updating head if necessary.
 * The caller must already be in a critical section.
 */
static inline pt_thread_t *
pt_i_unlink(pt_thread_t ** const head, pt_thread_t * const prev)
{
    PT_CRITICAL_ASSERT() ;
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

/* Unlink and return the oldest (last) thread.
 * The caller must already be in a critical section.
 */
static inline pt_thread_t *
pt_i_unlink_oldest(pt_thread_t ** const head)
{
    return pt_i_unlink(head, *head) ;
}

/* Finds thread <n> in list <head> and unlinks it. Returns TRUE if
 * it was found. The caller must already be in a critical section.
 */
static inline bool_t
pt_i_find_and_unlink(pt_thread_t ** const head, pt_thread_t * const n)
{
    PT_CRITICAL_ASSERT() ;
    pt_thread_t * prev = *head ;

    while (*head) {
        pt_thread_t * const t = prev->next ;
        if (n == t) {
            pt_i_unlink(head, prev) ;
            return true ;
        }
        /* Advance to next thread */
        prev = t ;
        /* looped back to start? finished */
        if (prev == *head) {
            break ;
        }
    }
    return false ;
}

/* Unlike the list primitives above, this takes its own critical section:
 * it is reached both from thread context and from inside one. The ready
 * function is called outside, so it may do arbitrary work.
 */
static inline void
pt_i_add_ready(protothread_t const s, pt_thread_t * const t)
{
    const pt_i_critical_t saved = PT_CRITICAL_ENTER() ;
    const bool_t notify = (s->ready_function && !s->ready && !s->running) ;
    pt_i_link(&s->ready, t) ;
    PT_CRITICAL_EXIT(saved) ;

    if (notify) {
        /* this should schedule protothread_run() */
        s->ready_function(s->ready_env) ;
    }
}

/* This is called by pt_create(), not by user code directly */
static inline void
pt_i_create_thread(
        protothread_t const s,
        pt_thread_t * const t,
        pt_func_t * const pt_func,
        pt_f_t const func,
        env_t env,
        bool_t const joinable
) {
    pt_func->thread = t ;
    pt_func->label = NULL ;
    t->func = func ;
    t->env = env ;
    t->s = s ;
    t->channel = NULL ;
    t->joinable = joinable ;
#if PT_DEBUG
    t->pt_func = pt_func ;
    t->next = NULL ;
#endif

    /* add the new thread to the ready list */
    pt_i_add_ready(s, t) ;
}

/* should only be called by the macro pt_yield() */
static inline void
pt_i_enqueue_yield(pt_thread_t * const t)
{
    protothread_t const s = t->s ;
    pt_assert(s->running == t) ;
    pt_i_add_ready(s, t) ;
}

/* Return which wait list to use (hash table) */
static inline pt_thread_t **
pt_i_get_wait_list(protothread_t const s, void * chan)
{
    return &s->wait[((uintptr_t)chan >> 4) & (PT_NWAIT-1)] ;
}

/* should only be called by the macro pt_wait() */
static inline void
pt_i_enqueue_wait(pt_thread_t * const t, void * const channel)
{
    protothread_t const s = t->s ;
    pt_thread_t ** const wq = pt_i_get_wait_list(s, channel) ;
    const pt_i_critical_t saved = PT_CRITICAL_ENTER() ;
    pt_assert(s->running == t) ;
    t->channel = channel ;
    pt_i_link(wq, t) ;
    PT_CRITICAL_EXIT(saved) ;
}

/* Construct goto labels using the current line number (so they are unique). */
#define PT_I_LABEL_HELP2(line) pt_i_label_ ## line
#define PT_I_LABEL_HELP(line) PT_I_LABEL_HELP2(line)
#define PT_I_LABEL PT_I_LABEL_HELP(__LINE__)

#if !PT_DEBUG
#define pt_i_debug_save(env)
#define pt_i_debug_wait(env)
#define pt_i_debug_call(env, child_env)
#else
#define pt_i_debug_save(env) do { \
    (env)->pt_func.file = __FILE__ ; \
    (env)->pt_func.line = __LINE__ ; \
    (env)->pt_func.function = __func__ ; \
} while (0)

#define pt_i_debug_wait(env) do { \
    pt_i_debug_save(env) ; \
    (env)->pt_func.next = NULL ; \
} while (0)

#define pt_i_debug_call(env, child_env) do { \
    pt_i_debug_save(env) ; \
    (env)->pt_func.next = &(child_env)->pt_func ; \
} while (0)

#endif

/* Wait for a channel to be signaled */
#define pt_wait(env, channel) \
    do { \
        (env)->pt_func.label = &&PT_I_LABEL ; \
        pt_i_enqueue_wait((env)->pt_func.thread, channel) ; \
        pt_i_debug_wait(env) ; \
        return PT_I_WAIT ; \
      PT_I_LABEL: ; \
    } while (0)

/* Let other ready protothreads run, then resume this thread */
#define pt_yield(env) \
    do { \
        (env)->pt_func.label = &&PT_I_LABEL ; \
        pt_i_enqueue_yield((env)->pt_func.thread) ; \
        pt_i_debug_wait(env) ; \
        return PT_I_WAIT ; \
      PT_I_LABEL: ; \
    } while (0)

/* Call a function (which may wait) */
#define pt_call(env, child_func, child_env, ...) \
    do { \
        (child_env)->pt_func.thread = (env)->pt_func.thread ; \
        (child_env)->pt_func.label = NULL ; \
        (env)->pt_func.label = NULL ; \
        pt_i_debug_call(env, child_env) ; \
      PT_I_LABEL: \
        if (child_func(child_env, ##__VA_ARGS__).pt_rv == PT_I_WAIT.pt_rv) { \
            (env)->pt_func.label = &&PT_I_LABEL ; \
            return PT_I_WAIT ; \
        } \
    } while (0)

/* Did the most recent pt_call() block (break context)? */
#define pt_call_waited(env) ((env)->pt_func.label != NULL)

/* Block until the given protothread has exited or been killed. */
#define pt_join(env, thr) \
    do { \
        pt_assert((thr) != (env)->pt_func.thread) ; \
        pt_assert((thr)->joinable) ; \
        while ((thr)->func) { \
            pt_wait(env, thr) ; \
        } \
    } while (0)

#define pt_create(pt, thr, func, env) \
    pt_i_create_thread(pt, thr, &(env)->pt_func, func, env, false)

/* Like pt_create(), but pt_join() may be used on the thread.  Its context
 * must stay allocated until it has been joined.
 */
#define pt_create_joinable(pt, thr, func, env) \
    pt_i_create_thread(pt, thr, &(env)->pt_func, func, env, true)

/* This allows protothreads (which might not have an explicit pointer to the
 * protothread object) to call pt_create(), pt_signal() or pt_broadcast().
 */
static inline protothread_t
pt_i_get_protothread(pt_func_t const * pt_func) {
    return pt_func->thread->s ;
}
#define pt_get_pt(env) pt_i_get_protothread(&(env)->pt_func)

static inline void
protothread_init(protothread_t const s)
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
protothread_deinit(protothread_t const s)
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

static inline protothread_t
protothread_create(void)
{
    /* the cast is redundant in C, but C++ will not convert void* implicitly */
    protothread_t const s = (protothread_t)malloc(sizeof(*s)) ;
    if (s) {
        protothread_init(s) ;
    }
    return s ;
}

static inline void
protothread_free(protothread_t const s)
{
    protothread_deinit(s) ;
    free(s) ;
}
#endif /* PT_NO_MALLOC */

static inline void pt_i_wake(protothread_t const s, void * const channel, bool_t const wake_one) ;

static inline bool_t
protothread_run(protothread_t const s)
{
    pt_i_critical_t saved ;

    pt_assert(s->running == NULL) ;
    saved = PT_CRITICAL_ENTER() ;
    if (s->ready == NULL) {
        PT_CRITICAL_EXIT(saved) ;
        return false ;
    }

    /* unlink the oldest ready thread */
    s->running = pt_i_unlink_oldest(&s->ready) ;
    PT_CRITICAL_EXIT(saved) ;

    /* run the thread */
    {
        pt_thread_t * const t = s->running ;
        bool_t const joinable = t->joinable ;
        pt_t const ret = t->func(t->env) ;

        s->running = NULL ;
        if (joinable && ret.pt_rv == PT_I_RETURN_DONE) {
            /* a NULL func marks an exited thread; see pt_join() */
            t->func = NULL ;
            pt_i_wake(s, t, false) ;
        }
    }

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
protothread_set_ready_function(protothread_t const s, void (*f)(env_t), env_t env)
{
    s->ready_function = f ;
    s->ready_env = env ;
}

/* Make the thread or threads that are waiting on the given
 * channel (if any) runnable.
 */
static inline void
pt_i_wake(protothread_t const s, void * const channel, bool_t const wake_one)
{
    pt_thread_t ** const wq = pt_i_get_wait_list(s, channel) ;
    const pt_i_critical_t saved = PT_CRITICAL_ENTER() ;
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
            pt_i_unlink(wq, prev) ;
            pt_i_add_ready(s, t) ;
            if (wake_one) {
                /* wake only the first found thread */
                break ;
            }
        }
    }
    PT_CRITICAL_EXIT(saved) ;
}

static inline void
pt_signal(protothread_t const s, void * const channel)
{
    pt_i_wake(s, channel, !PT_SIGNAL_WAKES_ALL) ;
}

static inline void
pt_broadcast(protothread_t const s, void * const channel)
{
    pt_i_wake(s, channel, false) ;
}

/* This is used to prevent a thread from scheduling again. This can be
 * very dangerous if the thread in question isn't written to expect this
 * operation. Any cleanup is the caller's, when this returns true.
 */
static inline bool_t
pt_kill(pt_thread_t * const t)
{
    protothread_t const s = t->s ;
    pt_i_critical_t saved ;
    bool_t killed ;

    pt_assert(s->running != t) ;

    saved = PT_CRITICAL_ENTER() ;
    killed = pt_i_find_and_unlink(&s->ready, t) ;
    if (!killed) {
        killed = pt_i_find_and_unlink(pt_i_get_wait_list(s, t->channel), t) ;
    }
    PT_CRITICAL_EXIT(saved) ;

    if (killed && t->joinable) {
        t->func = NULL ;
        pt_i_wake(s, t, false) ;
    }
    return killed ;
}
#endif
