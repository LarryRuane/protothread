/**************************************************************/
/* PROTOTHREAD_TEST.C */
/* Copyright (c) 2008, Larry Ruane, LeftHand Networks Inc. */
/* See license.txt */
/**************************************************************/
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

#include "protothread.h"

/******************************************************************************/

static void
test_create_dynamic(void)
{
    protothread_t const pt = protothread_create() ;
    protothread_run(pt) ;
    protothread_free(pt) ;
}

/******************************************************************************/

static void
test_create_static(void)
{
    struct protothread_s static_pt ;
    protothread_t const pt = &static_pt ;
    protothread_init(pt) ;
    protothread_run(pt) ;
    protothread_deinit(pt) ;
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

    for (i = 0; i < 1000; i++) {
        pt_create(pt, &c->pt_thread, create_thr, c) ;
        /* this will run the thread to completion, so can reuse c */
        protothread_run(pt) ;
        assert(!pt->nthread) ;
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

pthread_mutex_t app_mutex ;

typedef struct wait_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    int i ;
} wait_context_t ;

static pt_t
wait_thr(env_t const env)
{
    wait_context_t * const c = env ;
    pt_resume(c) ;

    pthread_mutex_lock(&app_mutex) ;
    for (c->i = 0; c->i < 10; c->i++) {
        pt_wait(c, NULL, &app_mutex) ;
    }
    pthread_mutex_unlock(&app_mutex) ;
    return PT_DONE ;
}

static void
test_wait(void)
{
    protothread_t const pt = protothread_create() ;
    wait_context_t * c[10] ;
    int i ;
    int j ;

    for (j = 0; j < 10; j++) {
        c[j] = malloc(sizeof(*(c[j]))) ;
        c[j]->i = -1 ;
        pt_create(pt, &c[j]->pt_thread, wait_thr, c[j]) ;
    }

    /* threads haven't run yet at all, make them reach the wait */
    for (j = 0; j < 10; j++) {
        protothread_run(pt) ;
    }

    for (i = 0; i < 10; i++) {
        for (j = 0; j < 10; j++) {
            assert(i == c[j]->i) ;
        }

        /* make threads runnable, but do not actually run the threads */
        pt_broadcast(pt, NULL) ;
        for (j = 0; j < 10; j++) {
            assert(i == c[j]->i) ;
        }

        /* run each thread once */
        for (j = 0; j < 10; j++) {
            protothread_run(pt) ;
        }
        for (j = 0; j < 10; j++) {
            assert(i+1 == c[j]->i) ;
        }

        /* extra steps and wrong signals shouldn't advance the threads */
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

/* make sure that broadcast wakes up all the threads it should,
 * none of the threads it shouldn't
 */
#define N 10000

typedef struct broadcast_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    struct broadcast_global_context_s * gc ;
    void * chan ;
    bool_t run ;
} broadcast_context_t ;

typedef struct broadcast_global_context_s {
    struct broadcast_context_s c[N] ;
    int count ;
    bool_t done ;
} broadcast_global_context_t ;

static pt_t
broadcast_thr(env_t const env)
{
    broadcast_context_t * const c = env ;
    broadcast_global_context_t * const gc = c->gc ;
    pt_resume(c) ;

    pthread_mutex_lock(&app_mutex) ;
    while (TRUE) {
        /* the /3 ensures multiple threads wait on the same chan */
        c->chan = &gc->c[(random() % N)/3] ;
        pt_wait(c, c->chan, &app_mutex) ;
	if (gc->done) {
	    break ;
	}
	assert(c->run) ;
	c->run = FALSE ;
    }
    pthread_mutex_unlock(&app_mutex) ;
    return PT_DONE ;
}

static void
test_broadcast(void)
{
    protothread_t const pt = protothread_create() ;
    broadcast_global_context_t gc ;
    int i, j ;

    srand(0) ;
    memset(&gc, 0, sizeof(gc)) ;
    for (j = 0; j < N; j++) {
        gc.c[j].gc = &gc ;
        pt_create(pt, &gc.c[j].pt_thread, broadcast_thr, &gc.c[j]) ;
    }

    /* threads haven't run yet at all, make them reach the wait */
    for (j = 0; j < N; j++) {
        protothread_run(pt) ;
    }

    for (i = 0; i < 100; i++) {
        broadcast_context_t * const chan = &gc.c[(random() % N)/3] ;
        pt_broadcast(pt, chan) ;
        for (j = 0; j < N; j++) {
            if (gc.c[j].chan == chan) {
		/* should run */
		gc.c[j].run = TRUE ;
	    }
	}
        while (protothread_run(pt)) ;

	/* make sure every tread that should have run did run */
        for (j = 0; j < N; j++) {
            assert(!gc.c[j].run) ;
        } 
    }
    gc.done = TRUE ;
    for (j = 0; j < N; j++) {
        pt_broadcast(pt, &gc.c[j]) ;
    }
    while (pt->nthread) {
        protothread_run(pt) ;
    }
    protothread_free(pt) ;
}

#undef N

/******************************************************************************/

/* Producer-consumer
 *
 * Every thread needs a pt_thread_t; every thread function (including
 * the top-level function) needs a pt_func_t with the name pt_func.
 * These can be anywhere in the structure.
 */
#define NPOSIXTHREADS 4

typedef struct pc_thread_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    int * mailbox ;     /* pointer to (shared) mailbox */
    int i ;             /* next value to send or expect to receive */
} pc_thread_context_t ;

#define N 100000

/* The producer thread waits until the mailbox is empty, and then writes 
 * the next value to the mailbox and pokes the consumer.
 */
static pt_t
producer_thr(env_t const env)
{
    pc_thread_context_t * const c = env ;
    pt_resume(c) ;

    for (c->i = 1; c->i <= N; c->i++) {
        pthread_mutex_lock(&app_mutex) ;
        while (*c->mailbox) {
            /* mailbox is full */
            pt_wait(c, c->mailbox, &app_mutex) ;
        }
        *c->mailbox = c->i ;
        pthread_mutex_unlock(&app_mutex) ;
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
        pthread_mutex_lock(&app_mutex) ;
        while (*c->mailbox == 0) {
            /* mailbox is empty */
            pt_wait(c, c->mailbox, &app_mutex) ;
        }
        assert(*c->mailbox == c->i) ;
        *c->mailbox = 0 ;   /* remove the item */
        pthread_mutex_unlock(&app_mutex) ;
        pt_signal(pt_get_pt(c), c->mailbox) ;
    }
    return PT_DONE ;
}

static void *
pc_thr(void *a)
{
    protothread_t const pt = a ;
    unsigned int count = 0;
    while (pt->nthread) {
        /* detect infinite loop (missed wakeup) */
        count++ ;
        assert(count < N*100) ;
        protothread_run(pt) ;
    }
    return NULL ;
}

static void
test_pc(void)
{
    protothread_t const pt = protothread_create() ;
    pc_thread_context_t * const cc = malloc(sizeof(*cc)) ;
    pc_thread_context_t * const pc = malloc(sizeof(*pc)) ;
    int mailbox = 0 ;
    pthread_t t[NPOSIXTHREADS] ;
    int i ;

    /* set up consumer context, start consumer thread */
    cc->mailbox = &mailbox ;
    cc->i = 0 ;
    pt_create(pt, &cc->pt_thread, consumer_thr, cc) ;

    /* set up producer context, start producer thread */
    pc->mailbox = &mailbox ;
    pc->i = 0 ;
    pt_create(pt, &pc->pt_thread, producer_thr, pc) ;

    /* create an arbitrary number of pthreads;
     * ideal would be the number of cores
     */
    for (i = 0; i < NPOSIXTHREADS; i++) {
	pthread_create(&t[i], NULL, pc_thr, pt) ;
    }
    for (i = 0; i < NPOSIXTHREADS; i++) {
	pthread_join(t[i], NULL) ;
    }

    /* threads have completed */
    assert(cc->i == N+1) ;
    assert(pc->i == N+1) ;

    free(cc) ;
    free(pc) ;
    protothread_free(pt) ;
}

static void
test_pc_big(void)
{
    protothread_t const pt = protothread_create() ;
    int mailbox[400] ;
    pc_thread_context_t * const pc = malloc(sizeof(*pc) * 400) ;
    pc_thread_context_t * const cc = malloc(sizeof(*cc) * 400) ;
    int i ;

    /* Start 400 independent pairs of threads, each pair sharing a
     * mailbox.
     */
    for (i = 0; i < 400; i++) {
        mailbox[i] = 0 ;

        cc[i].mailbox = &mailbox[i] ;
        cc[i].i = 0 ;
        pt_create(pt, &cc[i].pt_thread, consumer_thr, &cc[i]) ;

        pc[i].mailbox = &mailbox[i] ;
        pc[i].i = 0 ;
        pt_create(pt, &pc[i].pt_thread, producer_thr, &pc[i]) ;
    }

    /* as long as there is work to do */
    while (pt->nthread) {
        protothread_run(pt) ;
    }

    free(cc) ;
    free(pc) ;
    protothread_free(pt) ;
}

#undef N

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

    pthread_mutex_lock(&app_mutex) ;
    pt_wait(c, &gc[rand() % CHANS], &app_mutex) ;
    pthread_mutex_unlock(&app_mutex) ;
    if (c->level >= DEPTH) {
        /* leaf */
        assert(c->value < NODES) ;
        assert(!gc->seen[c->value]) ;
        gc->seen[c->value] = TRUE ;
        pthread_mutex_lock(&app_mutex) ;
        pt_wait(c, &gc[rand() % CHANS], &app_mutex) ;
        pthread_mutex_unlock(&app_mutex) ;
        gc->nseen ++ ;
        free(c) ;
        pthread_mutex_unlock(&app_mutex) ;
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
    pthread_mutex_lock(&app_mutex) ;
    pt_wait(c, &gc[rand() % CHANS], &app_mutex) ;
    pthread_mutex_unlock(&app_mutex) ;

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

    pthread_mutex_lock(&app_mutex) ;
    pt_wait(c, c, &app_mutex) ;
    pthread_mutex_unlock(&app_mutex) ;
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
    assert(more) ;
    assert(ready) ;
    ready = FALSE ;
    assert(pt->nthread == 0) ;

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
    assert(more) ;
    assert(ready) ;
    assert(pt->nthread == 0) ;

    free(c) ;
    protothread_free(pt) ;
}

/******************************************************************************/

typedef struct kill_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
} kill_context_t ;

static pt_t
kill_thr(env_t const env)
{
    kill_context_t * const c = env ;
    pt_resume(c) ;

    pt_yield(c) ;
    pthread_mutex_lock(&app_mutex) ;
    pt_wait(c, c, &app_mutex) ;
    pthread_mutex_unlock(&app_mutex) ;

    while (1) {
        pt_yield(c) ;
    }

    return PT_DONE ;
}

static void
test_kill(void)
{
    protothread_t const pt = protothread_create() ;
    kill_context_t * const c = calloc(2, sizeof(*c)) ;
    bool_t more ;
    bool_t killed ;

    /* Create the thread, kill it while it is in the run queue and make
     * sure it didn't run.
     */
    pt_create(pt, &c[0].pt_thread, kill_thr, &c[0]) ;
    pt_kill(&c[0].pt_thread) ;
    more = protothread_run(pt) ;
    assert(more) ;
    assert(pt->nthread == 0) ;

    /* Try to kill it one more time, just for giggles.  This may not cause any
     * apparent problems, but memory-checker tools like valgrind will flag
     * any problems created here.
     */
    killed = pt_kill(&c[0].pt_thread) ;
    assert(!killed) ;

    /* Create the thread, wait until it is in the wait queue, wake it,
     * kill it and make sure it isn't scheduled any longer.
     *
     * This actually tests killing while in the run queue (thus the same
     * test as above), but helps justify the following test.
     */
    pt_create(pt, &c[0].pt_thread, kill_thr, &c[0]) ;
    more = protothread_run(pt) ;
    assert(more) ;
    more = protothread_run(pt) ;
    assert(!more) ;
    pt_broadcast(pt, &c[0]) ;
    more = protothread_run(pt) ;
    assert(more) ;
    killed = pt_kill(&c[0].pt_thread) ;
    assert(killed) ;
    more = protothread_run(pt) ;
    assert(more) ;
    assert(pt->nthread == 0) ;

    /* Create the thread, wait until it is in the wait queue, kill it,
     * wake it and make sure it never scheduled again.
     */
    pt_create(pt, &c[0].pt_thread, kill_thr, &c[0]) ;
    more = protothread_run(pt) ;
    assert(more) ;
    more = protothread_run(pt) ;
    assert(!more) ;
    killed = pt_kill(&c[0].pt_thread) ;
    assert(killed) ;
    pt_broadcast(pt, &c[0]) ;
    more = protothread_run(pt) ;
    assert(more) ;
    assert(pt->nthread == 0) ;

    /* Create two threads, delete them one way and then
     * delete them the other way.
     */
    pt_create(pt, &c[0].pt_thread, kill_thr, &c[0]) ;
    pt_create(pt, &c[1].pt_thread, kill_thr, &c[1]) ;
    killed = pt_kill(&c[0].pt_thread) ;
    assert(killed) ;
    killed = pt_kill(&c[1].pt_thread) ;
    assert(killed) ;
    more = protothread_run(pt) ;
    assert(more) ;
    assert(pt->nthread == 0) ;

    pt_create(pt, &c[0].pt_thread, kill_thr, &c[0]) ;
    pt_create(pt, &c[1].pt_thread, kill_thr, &c[1]) ;
    killed = pt_kill(&c[1].pt_thread) ;
    assert(killed) ;
    killed = pt_kill(&c[0].pt_thread) ;
    assert(killed) ;
    more = protothread_run(pt) ;
    assert(more) ;
    assert(pt->nthread == 0) ;

    free(c) ;
    protothread_free(pt) ;
}

/******************************************************************************/

typedef struct reset_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    unsigned i ;
} reset_context_t ;

static pt_t
reset_thr(env_t const env)
{
    reset_context_t * const c = env ;

    pt_resume(c) ;

    for (c->i = 0; c->i < 5; c->i++) {
        pt_yield(c) ;
    }

    return PT_DONE ;
}

static void
test_reset(void)
{
    protothread_t const pt = protothread_create() ;
    reset_context_t * const c = calloc(1, sizeof(*c)) ;

    /* Start running the thread and then make sure we
     * can really reset the thread location
     */
    pt_create(pt, &c->pt_thread, reset_thr, c) ;

    protothread_run(pt) ;
    assert(c->i == 0) ;

    protothread_run(pt) ;
    assert(c->i == 1) ;
    pt_reset(c) ;

    protothread_run(pt) ;
    assert(c->i == 0) ;

    while (pt->nthread) {
        protothread_run(pt) ;
    }

    free(c) ;
    protothread_free(pt) ;
}

/******************************************************************************/

/* increment a global variable using several threads; make sure
 * threads synchronize correctly
 */
#define NPROTOTHREADS 1000
#define NWAITS 1000

typedef struct mt_global_context_s {
    protothread_t pt ;
    int counter ;
} mt_global_context_t ;

typedef struct mt_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    int * counter ;
    int i ;
} mt_context_t ;

static pt_t
mt_thr(env_t const env)
{
    mt_context_t * const c = env ;
    pt_resume(c) ;

    for (c->i = 0; c->i < NWAITS; c->i++) {
        /* all threads accessing this common counter */
        pthread_mutex_lock(&app_mutex) ;
        pt_wait(c, NULL, &app_mutex) ;
        (*c->counter) ++ ;
        pthread_mutex_unlock(&app_mutex) ;
        int i ;
        for (i = random() % 1000; i; i--) {
            /* spin for a while to randomize timing */
        }
    }

    free(c) ;
    return PT_DONE ;
}

/* each posix thread runs this function */
static void *
pthr(void *a)
{
    mt_global_context_t * const mt_global_c = a ;

    /* run the system until all protothreads exit */
    while (mt_global_c->pt->nthread) {
        protothread_run(mt_global_c->pt) ;
        pt_signal(mt_global_c->pt, NULL) ;
    }
    return NULL ;
}

static void
test_mt(void)
{
    pthread_t t[6] ;
    int i ;
    mt_global_context_t mt_global_c ;
    mt_global_c.pt = protothread_create() ;
    mt_global_c.counter = 0 ;

    for (i = 0; i < NPROTOTHREADS; i++) {
        mt_context_t * const c = malloc(sizeof(*c)) ;
        c->counter = &mt_global_c.counter ;
        pt_create(mt_global_c.pt, &c->pt_thread, mt_thr, c) ;
    }
    for (i = 0; i < 6; i++) {
	pthread_create(&t[i], NULL, pthr, &mt_global_c) ;
    }
    for (i = 0; i < 6; i++) {
	pthread_join(t[i], NULL) ;
    }
    protothread_free(mt_global_c.pt) ;
    assert(mt_global_c.counter == NPROTOTHREADS * NWAITS) ;
}

#undef NPROTOTHREADS
#undef NPOSIXTHREADS
#undef NWAITS

/******************************************************************************/

/* Most realistic example: many protothreads, all running
 * protothread_thr(), which wakes up a few other (random)
 * protothreads, waits, then does some simulated (cpu) work,
 * repeat until NSTEPS amount of work has been done (overall).
 *
 * The protothread system is run by a small number of posix
 * threads (preferrably equal to the number of cores), all
 * running posix_thr(). These are like virtual CPUs.
 */
#define NTHREADS 10000
#define NSTEPS 1000000
#define NPOSIXTHREADS 4

typedef struct protothread_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    struct protothread_global_context_s * gc ;
} protothread_context_t ;

typedef struct protothread_global_context_s {
    protothread_t pt ;
    pthread_cond_t cond ;
    unsigned int nsteps ;
    protothread_context_t c[NTHREADS] ;
} protothread_global_context_t ;

static pt_t
protothread_thr(env_t const env)
{
    protothread_context_t * const c = env ;
    protothread_global_context_t * gc = c->gc ;
    pt_resume(c) ;

    pthread_mutex_lock(&app_mutex) ;
    while (gc->nsteps < NSTEPS) {
        gc->nsteps++ ;
        /* this will wake up on average 4 threads */
        pt_broadcast(gc->pt, &gc->c[random() % (NTHREADS/4)]) ;
        pt_wait(c, &gc->c[random() % (NTHREADS/4)], &app_mutex) ;
        pthread_mutex_unlock(&app_mutex) ;

        /* simulate random amount of work */
        {
            unsigned int i ;
            for (i = random() % 10; i; i--) {
                random() ;
            }
        }
        pthread_mutex_lock(&app_mutex) ;
    }

    /* make sure no threads are left waiting forever */
    {
        unsigned int i ;
        for (i = 0; i < NTHREADS/4; i++) {
            pt_broadcast(gc->pt, &gc->c[i]) ;
        }
    }
    pthread_mutex_unlock(&app_mutex) ;
    return PT_DONE ;
}

/* each posix thread runs this function
 * (should this function be part of the product? it's pretty generic,
 * although that would bring in the use of condition variables.)
 */
static void *
posix_thr(void *a)
{
    protothread_global_context_t * const gc = a ;

    /* as long as there are protothreads ... */
    pthread_mutex_lock(&gc->pt->mutex) ;
    while (gc->pt->nthread) {
        if (!protothread_run_locked(gc->pt)) {
            /* there is no protothread to run at this time */
            pthread_cond_wait(&gc->cond, &gc->pt->mutex) ;
        }
    }
    pthread_mutex_unlock(&gc->pt->mutex) ;
    return NULL ;
}

/* this is called when a protothread becomes ready to run */
static void
ready_f(env_t env)
{
    protothread_global_context_t * const gc = env ;
    /* this has to be a broadcast instead of a signal... */
    pthread_cond_broadcast(&gc->cond) ;
}

static void
test_protothread(void)
{
    pthread_t t[NPOSIXTHREADS] ;
    unsigned int i ;
    protothread_global_context_t gc ;
    memset(&gc, 0, sizeof(gc)) ;
    gc.pt = protothread_create() ;
    protothread_set_ready_function(gc.pt, ready_f, &gc) ;
    {
        int const r = pthread_cond_init(&gc.cond, NULL) ;
        assert(!r) ;
    }
    for (i = 0; i < NTHREADS; i++) {
        protothread_context_t * const c = &gc.c[i] ;
        c->gc = &gc ;
        pt_create(gc.pt, &c->pt_thread, protothread_thr, c) ;
    }
    for (i = 0; i < NPOSIXTHREADS; i++) {
	pthread_create(&t[i], NULL, posix_thr, &gc) ;
    }
    for (i = 0; i < NPOSIXTHREADS; i++) {
	pthread_join(t[i], NULL) ;
    }
}

#undef NPOSIXTHREADS

/******************************************************************************/

/* Same example as immediately above, but implemented entirely
 * using posix threads. Here each protothread is replaced by a
 * posix thread. There are no posix threads to act as virtual CPUs.
 */

typedef struct posixthread_global_context_s {
    unsigned int nsteps ;
    pthread_cond_t c[NTHREADS/4] ;
} posixthread_global_context_t ;

static void *
posixthread_thr(void * a)
{
    posixthread_global_context_t * gc = a ;

    pthread_mutex_lock(&app_mutex) ;
    while (gc->nsteps < NSTEPS) {
        gc->nsteps++ ;
        /* this will wake up on average 4 threads */
        pthread_cond_broadcast(&gc->c[random() % (NTHREADS/4)]) ;
        pthread_cond_wait(&gc->c[random() % (NTHREADS/4)], &app_mutex) ;
        pthread_mutex_unlock(&app_mutex) ;

        /* simulate random amount of work */
        {
            unsigned int i ;
            for (i = random() % 10; i; i--) {
                random() ;
            }
        }
        pthread_mutex_lock(&app_mutex) ;
    }

    /* make sure no threads are left waiting forever */
    {
        unsigned int i ;
        for (i = 0; i < NTHREADS/4; i++) {
            pthread_cond_broadcast(&gc->c[i]) ;
        }
    }
    pthread_mutex_unlock(&app_mutex) ;
    return NULL ;
}

static void
test_posixthread(void)
{
    pthread_t t[NTHREADS] ;
    unsigned int i ;
    posixthread_global_context_t gc ;
    memset(&gc, 0, sizeof(gc)) ;

    for (i = 0; i < NTHREADS/4; i++) {
        int const r = pthread_cond_init(&gc.c[i], NULL) ;
        assert(!r) ;
    }

    for (i = 0; i < NTHREADS; i++) {
	pthread_create(&t[i], NULL, posixthread_thr, &gc) ;
    }
    for (i = 0; i < NTHREADS; i++) {
	pthread_join(t[i], NULL) ;
    }
}

#undef NTHREADS
#undef NSTEPS

/******************************************************************************/

int
main(int argc, char **argv)
{
    /* global application mutex */
    {
        int const r = pthread_mutex_init(&app_mutex, NULL) ;
        assert(!r) ;
    }

    test_create_dynamic() ;
    test_create_static() ;
    test_thread_create() ;
    test_yield() ;
    test_wait() ;
    test_broadcast() ;
    test_pc() ;
    test_pc_big() ;
    test_recursive() ;
    test_func_pointer() ;
    test_ready() ;
    test_kill() ;
    test_reset() ;
    test_mt() ;
    test_protothread() ;
    test_posixthread() ;

    return 0 ;
}
