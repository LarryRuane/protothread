/**************************************************************/
/* PROTOTHREAD.H */
/* Copyright (c) 2008, Larry Ruane, LeftHand Networks Inc. */
/* See license.txt */
/**************************************************************/
#ifndef PROTOTHREAD_H
#define PROTOTHREAD_H

#include <pthread.h>

#ifndef PT_DEBUG
#define PT_DEBUG 1  /* enabled (else 0) */
#endif
#define pt_assert(condition) do { if (PT_DEBUG) assert(condition) ; } while (0)

/* standard definitions */
typedef enum bool_e { FALSE, TRUE } bool_t ;
typedef void * env_t ;

/* Number of wait queues (size of wait hash table), power of 2 */
#define PT_NWAIT (1 << 12)

/* Usually there is one instance of struct protothread_s for
 * the overall system.
 */
typedef struct pt_thread_s pt_thread_t ;
typedef struct protothread_s {
    unsigned int nrunning ;         /* number of current running protothreads */
    unsigned int nthread ;          /* total number of protothreads */
    unsigned int nresume ;          /* number of protothread resumes */

    /* posix pthreads */
    unsigned int npthread_max ;     /* max number of pthreads we will create */
    unsigned int npthread ;         /* total number of posix pthreads */
    unsigned int npthread_running ; /* number of pthreads running protothreads */
    pthread_t *tid ;                /* posix pthread ids (array npthread_max) */
    pthread_cond_t cond ;

    pt_thread_t *ready ;            /* ready to run list (points to newest) */
    pt_thread_t *wait[PT_NWAIT] ;   /* waiting for an event (points to newest) */
    pthread_mutex_t mutex ;

    bool_t quiescing ;
    bool_t closing ;
} *protothread_t ;

/* Dynamic creation of the protothread system:
 *
 * To allocate the protothread system dynamically, use
 * protothread_create() to create and initialize the
 * protothread system.
 *
 * protothread_free() can be used to free the memory
 * allocated by protothread_create().
 */
protothread_t protothread_create(void) ;
protothread_t protothread_create_maxpt(unsigned int maxpt) ;
void protothread_free(protothread_t protothread) ;

/* Static creation of the protothread system:
 *
 * To allocate the protothread system statically,
 * create a struct protothread_s and initialize it
 * with protothread_init().
 *
 * protothread_deinit() should be called to
 * safely deinitialize the protothread system.
 */
void protothread_init(protothread_t) ;
void protothread_init_maxpt(protothread_t, int maxpt) ;
void protothread_deinit(protothread_t) ;
void protothread_quiesce(protothread_t) ;

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
    protothread_t s ;                   /* pointer to state */
#if PT_DEBUG
    struct pt_func_s * pt_func ;        /* top-level function's pt_func_t */
#endif
} ;


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
#define pt_resume(c) \
    do { if ((c)->pt_func.label) goto *(c)->pt_func.label ; } while (0)

/* These should only be called by the following pt macros */
void pt_enqueue_yield(pt_thread_t * t) ;
void pt_enqueue_wait(pt_thread_t * t, void * channel) ;

/* Construct goto labels using the current line number (so they are unique). */
#define PT_LABEL_HELP2(line) pt_label_ ## line
#define PT_LABEL_HELP(line) PT_LABEL_HELP2(line)
#define PT_LABEL PT_LABEL_HELP(__LINE__)

#if !PT_DEBUG
#define pt_debug_save(env) do { /*nop*/ } while (0)
#define pt_debug_wait(env) do { /*nop*/ } while (0)
#define pt_debug_call(env, child_env) do { /*nop*/ } while (0)
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

static inline bool_t
pt_mutex_is_locked(pthread_mutex_t * mu)
{
    if (pthread_mutex_trylock(mu)) {
        /* mutex is already held */
        return TRUE ;
    }
    /* we took the mutex, so now free it */
    pthread_mutex_unlock(mu) ;
    return FALSE ;
}

/* Wait for a channel to be signaled */
#define pt_wait(env, channel, mu) \
    do { \
        (env)->pt_func.label = &&PT_LABEL ; \
        pt_assert(pt_mutex_is_locked(mu)) ; \
        pthread_mutex_lock(&(env)->pt_func.thread->s->mutex) ; \
        pthread_mutex_unlock(mu) ; \
        pt_enqueue_wait((env)->pt_func.thread, channel) ; \
        pt_debug_wait(env) ; \
        return PT_WAIT ; \
      PT_LABEL: ; \
        pthread_mutex_lock(mu) ; \
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

void pt_create_thread(protothread_t, pt_thread_t *, pt_func_t *, pt_f_t, env_t) ;
#define pt_create(pt, thr, func, env) \
    pt_create_thread(pt, thr, &(env)->pt_func, func, env) ;

void pt_signal(protothread_t pt, void * channel) ;
void pt_broadcast(protothread_t pt, void * channel) ;

/* This allows protothreads (which might not have an explicit pointer to the
 * protothread object) to call pt_create(), pt_signal() or pt_broadcast().
 */
static inline protothread_t
pt_get_protothread(pt_func_t const * pt_func) {
    return pt_func->thread->s ;
}
#define pt_get_pt(env) pt_get_protothread(&(env)->pt_func)

#endif /* PROTOTHREAD_H */
