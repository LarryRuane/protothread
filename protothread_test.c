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

pthread_mutex_t app_mutex ;

/* create some artificial cpu delay (the fprintf prevents the compiler from
 * optimizing away the entire loop)
 */
static void
test_cpu_delay(unsigned int count) {
    int i ;
    for (i = 0 ; i < count ; i++) {
        fprintf(stderr, "%s", "");
    }
}

/******************************************************************************/

static void
test_create_dynamic(void)
{
    protothread_t const pt = protothread_create() ;
    protothread_free(pt) ;
}

/******************************************************************************/

static void
test_create_static(void)
{
    struct protothread_s static_pt ;
    protothread_t const pt = &static_pt ;
    protothread_init(pt) ;
    protothread_quiesce(pt) ;
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
    protothread_t const pt = protothread_create_maxpt(20) ;
    int i ;
    create_context_t * const c = malloc(sizeof(*c)) ;

    for (i = 0; i < 100; i++) {
        pt_create(pt, &c->pt_thread, create_thr, c) ;
        /* this will run the thread to completion, so can reuse c */
        protothread_quiesce(pt) ;
        assert(!pt->nthread) ;
    }
    free(c) ;
    protothread_free(pt) ;
}

/******************************************************************************/

/* create a bunch of protothreads that simulate CPU work, which should
 * cause the max number of pthreads (20) to be created
 */
static pt_t
blocking_thr(env_t const env)
{
    create_context_t * const c = env ;
    pt_resume(c) ;
    test_cpu_delay(50*1000*1000) ;

    return PT_DONE ;
}

#define N 400

static void
test_blocking(void)
{
    protothread_t const pt = protothread_create_maxpt(20) ;
    create_context_t c[N] ;
    int i ;
    for (i = 0; i < N; i++) {
        test_cpu_delay(1000*1000) ;
        pt_create(pt, &c[i].pt_thread, blocking_thr, &c[i]) ;
    }
    protothread_quiesce(pt) ;
    assert(pt->npthread == pt->npthread_max) ;
    protothread_free(pt) ;
}
#undef N

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
        unsigned int nresume = pt_get_pt(c)->nresume ;
        pt_yield(c) ;
        assert(pt_get_pt(c)->nresume > nresume) ;
    }
    return PT_DONE ;
}

static void
test_yield(void)
{
    protothread_t const pt = protothread_create() ;
    yield_context_t * const c = malloc(sizeof(*c)) ;

    pt_create(pt, &c->pt_thread, yield_thr, c) ;
    protothread_quiesce(pt) ;
    assert(c->i == 10) ;
    free(c) ;
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

    /* let all threads reach pt_wait */
    protothread_quiesce(pt) ;

    for (i = 0; i < 100; i++) {
        broadcast_context_t * const chan = &gc.c[(random() % N)/3] ;
        for (j = 0; j < N; j++) {
            if (gc.c[j].chan == chan) {
                /* should run */
                gc.c[j].run = TRUE ;
            }
        }
        pt_broadcast(pt, chan) ;
        protothread_quiesce(pt) ;
        /* make sure every tread that should have run did run */
        for (j = 0; j < N; j++) {
            assert(!gc.c[j].run) ;
        }
    }
    gc.done = TRUE ;
    for (j = 0; j < N; j++) {
        pt_broadcast(pt, &gc.c[j]) ;
    }
    protothread_free(pt) ;
}

#undef N

/******************************************************************************/

/* Producer-consumer (one producer and one consumer protothread)
 *
 * Every thread needs a pt_thread_t; every thread function (including
 * the top-level function) needs a pt_func_t with the name pt_func.
 * These can be anywhere in the structure.
 */
typedef struct pc_thread_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    int * mailbox ;     /* pointer to (shared) mailbox */
    int i ;             /* next value to send or expect to receive */
} pc_thread_context_t ;

#define N (10*1000)

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
        while (*c->mailbox > 0) {
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

static void
test_pc(void)
{
    protothread_t const pt = protothread_create_maxpt(4) ;
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

    /* wait for both protothreads to complete */
    protothread_quiesce(pt) ;

    /* threads have completed */
    assert(cc->i == N+1) ;
    assert(pc->i == N+1) ;

    free(cc) ;
    free(pc) ;
    protothread_free(pt) ;
}

#define NMAILBOX 20
static void
test_pc_big(void)
{
    protothread_t const pt = protothread_create_maxpt(4) ;
    int mailbox[NMAILBOX] ;
    pc_thread_context_t * const pc = calloc(NMAILBOX, sizeof(*pc)) ;
    pc_thread_context_t * const cc = calloc(NMAILBOX, sizeof(*cc)) ;
    int i ;

    /* Start NMAILBOX independent pairs of threads, each pair sharing a
     * mailbox.
     */
    for (i = 0; i < NMAILBOX; i++) {
        mailbox[i] = 0 ;

        cc[i].mailbox = &mailbox[i] ;
        cc[i].i = 0 ;
        pt_create(pt, &cc[i].pt_thread, consumer_thr, &cc[i]) ;

        pc[i].mailbox = &mailbox[i] ;
        pc[i].i = 0 ;
        pt_create(pt, &pc[i].pt_thread, producer_thr, &pc[i]) ;
    }

    /* wait for no more work to do */
    protothread_quiesce(pt) ;

    free(cc) ;
    free(pc) ;
    protothread_free(pt) ;
}
#undef NMAILBOX
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
        pt_create(pt_get_pt(c), &c->child_c->pt_thread,
                recursive_thr, c->child_c) ;
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
        pt_create(pt_get_pt(c), &c->child_c->pt_thread,
                recursive_thr, c->child_c) ;
    }

    free(c) ;
    return PT_DONE ;
}

static void
test_recursive_once(void)
{
    protothread_t const pt = protothread_create_maxpt(16) ;
    recursive_call_global_context_t * gc = malloc(sizeof(*gc)) ;
    recursive_call_context_t * top_c = malloc(sizeof(*top_c)) ;
    int i ;

    memset(gc, 0, sizeof(*gc)) ;
    memset(top_c, 0, sizeof(*top_c)) ;

    top_c->gc = gc ;
    pt_create(pt, &top_c->pt_thread, recursive_thr, top_c) ;

    /* wait for all threads to complete */
    while (gc->nseen < NODES) {
        protothread_quiesce(pt) ;
        pt_signal(pt, &gc[rand() % CHANS]) ;
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
    protothread_quiesce(pt) ;
    assert(c.level2.ran) ;

    protothread_free(pt) ;
}

/******************************************************************************/

#define TEST_ITEMS() \
    item(test_create_dynamic) \
    item(test_create_static) \
    item(test_thread_create) \
    item(test_blocking) \
    item(test_yield) \
    item(test_broadcast) \
    item(test_pc) \
    item(test_pc_big) \
    item(test_recursive) \
    item(test_func_pointer) \

int
main(int argc, char **argv)
{
    /* global application mutex */
    {
        int const r = pthread_mutex_init(&app_mutex, NULL) ;
        assert(!r) ;
    }
#define item(test) \
    printf(#test " ...") ; \
    fflush(stdout) ; \
    test() ; \
    printf(" ok\n") ; \

    TEST_ITEMS()

#undef item

    return 0 ;
}
