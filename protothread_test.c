/**************************************************************/
/* PROTOTHREAD_TEST.C */
/* Copyright (c) 2008, Larry Ruane, LeftHand Networks Inc. */
/* See license.txt */
/**************************************************************/
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "protothread.h"
#include "protothread_sem.h"
#include "protothread_lock.h"

/******************************************************************************/

static void
test_create(void)
{
    protothread_t const pt = protothread_create() ;
    protothread_run(pt) ;
    protothread_free(pt) ;
}

/******************************************************************************/

typedef struct create_context_s {
    int i ;
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
} create_context_t ;

static pt_t
create_thr(env_t const env)
{
    create_context_t * const c = env ;
    pt_resume(c) ;

    return PT_DONE ;
}

static void
test_thread_create(void)
{
    protothread_t const pt = protothread_create() ;
    int i ;
    create_context_t * const c = malloc(sizeof(*c)) ;

    for (i=0; i<1000; i++) {
        pt_create(pt, &c->pt_thread, create_thr, c) ;
        protothread_run(pt) ;
    }
    free(c) ;
    protothread_free(pt) ;
}

/******************************************************************************/

typedef struct yield_context_s {
    pt_func_t pt_func ;
    pt_thread_t pt_thread ;
    int i ;
} yield_context_t ;

static pt_t
yield_thr(env_t const env)
{
    yield_context_t * const c = env ;
    pt_resume(c) ;

    for (c->i = 0; c->i < 10; c->i++) {
        pt_yield(c) ;
    }
    return PT_DONE ;
}

static void
test_yield(void)
{
    protothread_t const pt = protothread_create() ;
    yield_context_t * const c = malloc(sizeof(*c)) ;
    int i ;

    c->i = -1 ;     /* invalid value */
    pt_create(pt, &c->pt_thread, yield_thr, c) ;

    /* it hasn't run yet at all, make it reach the yield */
    protothread_run(pt) ;

    for (i = 0; i < 10; i++) {
        /* make sure the protothread advances its loop */
        assert(i == c->i) ;
        protothread_run(pt) ;
    }
    free(c) ;
    protothread_free(pt) ;
}

/******************************************************************************/

typedef struct wait_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    int i ;
    void *wait;
} wait_context_t ;

static pt_t
wait_thr(env_t const env)
{
    wait_context_t * const c = env ;
    pt_resume(c) ;

    for (c->i=0; c->i<10; c->i++) {
        pt_wait(c, c->wait) ;
    }
    return PT_DONE ;
}

static void
test_wait(void)
{
    protothread_t const pt = protothread_create() ;
    wait_context_t * c[10];
    int i ;
    int j ;

    for (j = 0; j < 10; j++) {
        c[j] = malloc(sizeof(*(c[j]))) ;
        c[j]->i = -1;
        c[j]->wait = (void *)&pt;
        pt_create(pt, &c[j]->pt_thread, wait_thr, c[j]) ;
    }

    /* it hasn't run yet at all, make it reach the wait */
    for (j = 0; j < 10; j++) {
        protothread_run(pt) ;
    }

    for (i = 0; i < 10; i++) {
        for (j = 0; j < 10; j++) {
            assert(i == c[j]->i) ;
        }
        pt_broadcast(pt, (void *)&pt) ;
        for (j = 0; j < 10; j++) {
            assert(i == c[j]->i) ;
        }
        for (j = 0; j < 10; j++) {
            protothread_run(pt) ;
        }
        for (j = 0; j < 10; j++) {
            assert(i+1 == c[j]->i) ;
        }

        /* extra steps and wrong signals shouldn't advance the thread */
        protothread_run(pt) ;
        pt_broadcast(pt, c[0]) ;
        protothread_run(pt) ;
        for (j = 0; j < 10; j++) {
            assert(i+1 == c[j]->i) ;
        }
    }

    for (j = 0; j < 10; j++) {
        free(c[j]) ;
    }
    protothread_free(pt) ;
}

/******************************************************************************/

/* Producer-comsumer
 *
 * Every thread needs a pt_thread_t; every thread function (including
 * the top-level function) needs a pt_func_t with the name pt_func.
 * These can be anywhere in the structure.
 */
typedef struct pc_thread_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    int * mailbox;      /* pointer to (shared) mailbox */
    int i;              /* next value to send or expect to receive */
} pc_thread_context_t ;

static int N = 1000 ;

/* The producer thread waits until the mailbox is empty, and then writes 
 * the next value to the mailbox and pokes the consumer.
 */
static pt_t
producer_thr(env_t const env)
{
    pc_thread_context_t * const c = env ;
    pt_resume(c) ;

    for (c->i = 1; c->i <= N; c->i++) {
        while (*c->mailbox) {
            /* mailbox is full */
            pt_wait(c, c->mailbox) ;
        }
        *c->mailbox = c->i ;
        pt_signal(pt_get_pt(c), c->mailbox) ;
    }
    return PT_DONE ;
}

/* The consumer thread waits until something (non-zero) appears in the
 * mailbox, verifies that it's the expected value, writes a zero to
 * signify that the mailbox is empty, and wakes up the producer.
 */
static pt_t
consumer_thr(env_t const env)
{
    pc_thread_context_t * const c = env ;
    pt_resume(c) ;

    for (c->i = 1; c->i <= N; c->i++) {
        while (*c->mailbox == 0) {
            /* mailbox is empty */
            pt_wait(c, c->mailbox) ;
        }
        assert(*c->mailbox == c->i) ;
        *c->mailbox = 0 ;   /* remove the item */
        pt_signal(pt_get_pt(c), c->mailbox) ;
    }
    return PT_DONE ;
}

static void
test_pc(void)
{
    protothread_t const pt = protothread_create() ;
    pc_thread_context_t * const cc = malloc(sizeof(*cc)) ;
    pc_thread_context_t * const pc = malloc(sizeof(*pc)) ;
    int mailbox = 0 ;

    /* set up consumer context, start consumer thread */
    cc->mailbox = &mailbox ;
    cc->i = 0 ;
    pt_create(pt, &cc->pt_thread, consumer_thr, cc) ;

    /* set up producer context, start producer thread */
    pc->mailbox = &mailbox ;
    pc->i = 0 ;
    pt_create(pt, &pc->pt_thread, producer_thr, pc) ;

    /* while threads are available to run ... */
    while (protothread_run(pt)) ;

    /* threads have completed */
    assert(cc->i == N+1);
    assert(pc->i == N+1);

    free(cc) ;
    free(pc) ;
    protothread_free(pt) ;
}

static void
test_pc_big(void)
{
    protothread_t const pt = protothread_create() ;
    int mailbox[400] ;
    int i ;

    /* Start 400 independent pairs of threads, each pair sharing a
     * mailbox.
     */
    for (i = 0; i < 400; i++) {
        pc_thread_context_t * const cc = malloc(sizeof(*cc)) ;
        pc_thread_context_t * const pc = malloc(sizeof(*pc)) ;

        mailbox[i] = 0 ;

        cc->mailbox = &mailbox[i] ;
        cc->i = 0 ;
        pt_create(pt, &cc->pt_thread, consumer_thr, cc) ;

        pc->mailbox = &mailbox[i] ;
        pc->i = 0 ;
        pt_create(pt, &pc->pt_thread, producer_thr, pc) ;
    }

    /* as long as there is work to do */
    while (protothread_run(pt)) ;

    protothread_free(pt) ;
}

/******************************************************************************/

/* count to 4096 the hard way */

#define DEPTH 12
#define NODES (1 << DEPTH)
#define CHANS (NODES/8)

typedef struct recursive_call_global_context_s {
    bool_t seen[NODES] ;
    int nseen ;
} recursive_call_global_context_t ;

typedef struct recursive_call_context_s {
    int level ;     /* zero is top-level function */
    int value ;
    pt_func_t pt_func ;
    pt_thread_t pt_thread ;
    recursive_call_global_context_t * gc ;
    struct recursive_call_context_s * child_c ;
} recursive_call_context_t ;

static pt_t
recursive_thr(env_t const env)
{
    recursive_call_context_t * const c = env ;
    recursive_call_global_context_t * const gc = c->gc ;
    pt_resume(c) ;

    pt_wait(c, &gc[rand() % CHANS]) ;
    if (c->level >= DEPTH) {
        /* leaf */
        assert(c->value < NODES) ;
        assert(!gc->seen[c->value]) ;
        gc->seen[c->value] = TRUE ;
        pt_wait(c, &gc[rand() % CHANS]) ;
        gc->nseen ++ ;
        free(c) ;
        return PT_DONE ;
    }

    /* create the "left" (0) child; it will free this */
    c->child_c = malloc(sizeof(*c->child_c)) ;
    *c->child_c = *c ;
    c->child_c->level ++ ;
    c->child_c->value <<= 1 ;
    if ((rand() % 4)) {
        /* usually make a synchronous function call */
        pt_call(c, recursive_thr, c->child_c) ;
    } else {
        /* once in a while create a new thread (asynchronous) */
        pt_create(pt_get_pt(c), &c->child_c->pt_thread, recursive_thr, c->child_c) ;
    }
    pt_wait(c, &gc[rand() % CHANS]) ;

    /* create the "right" (1) child; it will free this */
    c->child_c = malloc(sizeof(*c->child_c)) ;
    *c->child_c = *c ;
    c->child_c->level ++ ;
    c->child_c->value <<= 1 ;
    c->child_c->value ++ ;
    if ((rand() % 4)) {
        pt_call(c, recursive_thr, c->child_c) ;
    } else {
        pt_create(pt_get_pt(c), &c->child_c->pt_thread, recursive_thr, c->child_c) ;
    }

    free(c) ;
    return PT_DONE ;
}

static void
test_recursive_once(void)
{
    protothread_t const pt = protothread_create() ;
    recursive_call_global_context_t * gc = malloc(sizeof(*gc)) ;
    recursive_call_context_t * top_c = malloc(sizeof(*top_c)) ;
    int i ;

    memset(gc, 0, sizeof(*gc)) ;
    memset(top_c, 0, sizeof(*top_c)) ;

    top_c->gc = gc ;
    pt_create(pt, &top_c->pt_thread, recursive_thr, top_c) ;

    /* it hasn't run yet at all, make it reach the call */
    i = 0 ;
    while (gc->nseen < NODES) {
        if (protothread_run(pt)) {
            i++ ;
        }
        /* make sure it is not taking too long (the 10 is cushion) */
        assert(i < NODES*4*10) ;
        pt_broadcast(pt, &gc[rand() % CHANS]) ;
    }
    for (i = 0; i < NODES; i++) {
        assert(gc->seen[i]) ;
    }
    free(gc) ;
    protothread_free(pt) ;
}

static void
test_recursive(void)
{
    int i ;

    /* because we're using random numbers, can run this multiple times
     */
    srand(0) ;
    for (i = 0; i < 10; i++) {
        test_recursive_once() ;
    }
}

/******************************************************************************/

typedef struct sem_global_context_s {
    int owner ;             /* zero, or thread who is in the critical section */
    unsigned int sem_value ;
} sem_global_context_t ;

typedef struct sem_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    sem_global_context_t *gc ;
    int i ;                 /* loop index */
    int id ;                /* thread id (>= 1) */
    pt_sem_env_t sem_env ;
} sem_context_t ;

/* implement mutual exclusion using a semaphore */
static pt_t
sem_thr(env_t const env)
{
    sem_context_t * const c = env ;
    pt_resume(c) ;

    for (c->i = 0; c->i < 100; c->i++) {
        /* enter critical section */
        pt_sem_acquire(c, &c->sem_env, &c->gc->sem_value) ;
        assert(c->gc->owner == 0) ;
        c->gc->owner = c->id ;
        pt_yield(c) ;
        assert(c->gc->owner == c->id) ;
        c->gc->owner = 0 ;
        pt_sem_release(&c->sem_env, &c->gc->sem_value) ;
        pt_yield(c) ;
    }
    free(c) ;
    return PT_DONE ;
}

static void
test_sem(void)
{
    protothread_t const pt = protothread_create() ;
    sem_global_context_t * gc = malloc(sizeof(*gc)) ;
    int i ;

    gc->owner = 0 ;     /* invalid ID (no one in critical section) */

    /* for mutual exclusion, init the semaphore count to 1 */
    gc->sem_value = 1 ;

    for (i = 0; i < 100; i++) {
        sem_context_t * const c = malloc(sizeof(*c)) ;
        c->gc = gc ;
        c->id = i+1 ;
        pt_create(pt, &c->pt_thread, sem_thr, c) ;
    }

    /* as long as there is work to do */
    while (protothread_run(pt)) ;

    free(gc) ;
    protothread_free(pt) ;
}

/******************************************************************************/

typedef struct lock_global_context_s {
    pt_lock_t lock ;
    unsigned int nthreads ;
} lock_global_context_t ;

/* static state is easier to debug */
static lock_global_context_t lock_gc ;

typedef struct lock_context_s {
    lock_global_context_t *gc ;
    pt_func_t pt_func ;
    pt_thread_t pt_thread ;
    int i ;                 /* loop index */
    int yi ;                /* yield loop index */
    int id ;                /* thread id (>= 1) */
    pt_lock_env_t lock_env ;
} lock_context_t ;

#define LOCK_NTHREADS 10
static lock_context_t lock_r_tc[LOCK_NTHREADS] ;
static lock_context_t lock_w_tc[LOCK_NTHREADS] ;

static void
lock_trc(char const * str, int id) {
    static int n ;
    if (0) {
        printf("%s%d ", str, id-1) ;
        if (++n > 16) {
            n = 0 ;
            printf("\n");
        }
    }
}

static pt_t
read_thr(env_t const env)
{
    lock_context_t * const c = env ;
    pt_resume(c) ;

    for (c->i = 0; c->i < 10000; c->i++) {
        /* enter critical section */
        pt_lock_acquire_read(c, &c->lock_env, &c->gc->lock) ;
        lock_trc("rs", c->id) ;
        for (c->yi = random() % 100; c->yi; c->yi--) {
            assert(c->gc->lock.nwriters == 0) ;
            assert(c->gc->lock.nreaders > 0) ;
            pt_yield(c) ;
            assert(c->gc->lock.nwriters == 0) ;
            assert(c->gc->lock.nreaders > 0) ;
        }
        lock_trc("re", c->id) ;
        pt_lock_release_read(&c->lock_env, &c->gc->lock) ;
        for (c->yi = random() % 200; c->yi; c->yi--) {
            pt_yield(c) ;
        }
    }
    c->gc->nthreads -- ;
    return PT_DONE ;
}

static pt_t
write_thr(env_t const env)
{
    lock_context_t * const c = env ;
    pt_resume(c) ;

    for (c->i = 0; c->i < 1000; c->i++) {
        /* enter critical section */
        pt_lock_acquire_write(c, &c->lock_env, &c->gc->lock) ;
        lock_trc("ws", c->id) ;
        for (c->yi = rand() % 100; c->yi; c->yi--) {
            assert(c->gc->lock.nreaders == 0) ;
            assert(c->gc->lock.nwriters == 1) ;
            pt_yield(c) ;
            assert(c->gc->lock.nreaders == 0) ;
            assert(c->gc->lock.nwriters == 1) ;
        }
        lock_trc("we", c->id) ;
        pt_lock_release_write(&c->lock_env, &c->gc->lock) ;
        for (c->yi = rand() % 100; c->yi; c->yi--) {
            pt_yield(c) ;
        }
    }
    c->gc->nthreads -- ;
    return PT_DONE ;
}

/* This test does not verify fairness; that's hard to do */
static void
test_lock(void)
{
    protothread_t const pt = protothread_create() ;
    int i ;

    srand(0) ;
    pt_lock_init(&lock_gc.lock) ;

    for (i = 0; i < LOCK_NTHREADS; i++) {
        lock_context_t * c = &lock_r_tc[i] ;
        c->gc = &lock_gc ;
        c->id = i+1 ;
        pt_create(pt, &c->pt_thread, read_thr, c) ;
        lock_gc.nthreads ++ ;
    }

    for (i = 0; i < LOCK_NTHREADS; i++) {
        lock_context_t * c = &lock_w_tc[i] ;
        c->gc = &lock_gc ;
        c->id = i+1 ;
        pt_create(pt, &c->pt_thread, write_thr, c) ;
        lock_gc.nthreads ++ ;
    }

    /* as long as there is work to do */
    while (protothread_run(pt)) ;

    assert(lock_gc.nthreads == 0) ;
    protothread_free(pt) ;
}

/******************************************************************************/

typedef struct func_pointer_context_s {
    int i ;
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    struct level2_s {
        pt_func_t pt_func ;
        bool_t ran ;
    } level2 ;
} func_pointer_context_t ;

static pt_t
func_pointer_level2(env_t const env)
{
    struct level2_s * const c = env ;
    pt_resume(c) ;
    pt_yield(c) ;
    c->ran = TRUE ;
    return PT_DONE ;
}

static pt_t
func_pointer_thr(env_t const env)
{
    func_pointer_context_t * const c = env ;
    pt_f_t const func_ptr = func_pointer_level2 ;

    /* pt_call() can take a function pointer */
    pt_resume(c) ;
    pt_call(c, func_ptr, &c->level2) ;
    return PT_DONE ;
}

static void
test_func_pointer(void)
{
    protothread_t const pt = protothread_create() ;
    func_pointer_context_t c ;
    pt_f_t const func_ptr = func_pointer_thr ;

    /* pt_create() can take a function pointer */
    c.level2.ran = FALSE ;
    pt_create(pt, &c.pt_thread, func_ptr, &c) ;
    protothread_run(pt) ;
    assert(!c.level2.ran) ;
    protothread_run(pt) ;
    assert(c.level2.ran) ;

    protothread_free(pt) ;
}

/******************************************************************************/

static bool_t ready ;
static void
set_ready(env_t env)
{
    assert(env == &ready) ;
    assert(!ready) ;
    ready = TRUE ;
}

typedef struct ready_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
} ready_context_t ;

static pt_t
ready_thr(env_t const env)
{
    ready_context_t * const c = env ;
    pt_resume(c) ;

    pt_wait(c, c) ;
    pt_yield(c) ;

    return PT_DONE ;
}

static void
test_ready(void)
{
    protothread_t const pt = protothread_create() ;
    protothread_set_ready_function(pt, set_ready, &ready) ;
    bool_t more ;   /* more work to do */
    ready_context_t * const c = malloc(sizeof(*c)) ;

    /* nothing to run */
    more = protothread_run(pt) ;
    assert(!more) ;
    assert(!ready) ;

    pt_create(pt, &c->pt_thread, ready_thr, c) ;
    assert(ready) ;
    ready = FALSE ;

    /* advance thread to the wait */
    more = protothread_run(pt) ;
    assert(!more) ;
    assert(!ready) ;

    /* make the thread runnable */
    pt_signal(pt, c) ;
    assert(ready) ;
    ready = FALSE ;

    /* should advance the thread to the pt_yield() */
    more = protothread_run(pt) ;
    assert(more) ;
    assert(!ready) ;

    /* advance the thread to its exit, nothing ready to run */
    more = protothread_run(pt) ;
    assert(!more) ;
    assert(!ready) ;

    free(c) ;
    protothread_free(pt) ;
}

/******************************************************************************/

int
main(int argc, char **argv)
{
    test_create() ;
    test_thread_create() ;
    test_yield() ;
    test_wait() ;
    test_pc() ;
    test_pc_big() ;
    test_recursive() ;
    test_sem() ;
    test_lock() ;
    test_func_pointer() ;
    test_ready() ;

    return 0 ;
}
