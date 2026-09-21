/**************************************************************/
/* PROTOTHREAD_BENCH.C */
/* https://github.com/LarryRuane/protothread */
/* Copyright (c) 2008-present Larry Ruane */
/* Distributed under the MIT software license, see the accompanying */
/* file LICENSE or https://opensource.org/licenses/MIT. */
/* SPDX-License-Identifier: MIT */
/**************************************************************/

/* Protothreads vs. POSIX threads.
 *
 * This is not an apples-to-apples comparison, and it is not meant to be.
 * POSIX threads give you preemption and real parallelism across cores;
 * protothreads give you neither. What these numbers measure is the cost of
 * the mechanism itself: what you pay, per operation, for the ability to
 * write code that blocks. Protothreads are faster here because they do
 * enormously less -- no kernel transition, no scheduler, no stack.
 *
 * Where that trade is a good one -- a state machine per connection, an
 * event loop, an embedded system with no MMU -- these ratios are the reason
 * to care. Where you need to use four cores, they are irrelevant.
 *
 * Build with -O2 and PT_DEBUG=0 (see CMakeLists.txt); PT_DEBUG adds
 * per-wait bookkeeping that you would not ship.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <limits.h>

#include "protothread.h"

static double
now_ns(void)
{
    struct timespec ts ;
    clock_gettime(CLOCK_MONOTONIC, &ts) ;
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec ;
}

static void
report(char const *what, char const *unit, double pt_ns, double posix_ns)
{
    printf("  %-26s %10.1f %10.1f %10.0fx   (%s)\n",
           what, pt_ns, posix_ns, posix_ns / pt_ns, unit) ;
}

/******************************************************************************/
/* 1. Context switch: two threads handing a token back and forth.             */
/******************************************************************************/

typedef struct pp_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    int * turn ;
    int me ;
    long i ;
    long iters ;
} pp_context_t ;

static pt_t
pp_thr(env_t const env)
{
    pp_context_t * const c = env ;
    pt_resume(c) ;

    for (c->i = 0; c->i < c->iters; c->i++) {
        while (*c->turn != c->me) {
            pt_wait(c, c->turn) ;
        }
        *c->turn = 1 - c->me ;
        pt_signal(pt_get_pt(c), c->turn) ;
    }
    return PT_DONE ;
}

static double
bench_pt_switch(long iters)
{
    protothread_t const pt = protothread_create() ;
    pp_context_t c[2] ;
    int turn = 0 ;
    double t0, t1 ;
    int i ;

    for (i = 0; i < 2; i++) {
        c[i].turn = &turn ;
        c[i].me = i ;
        c[i].iters = iters ;
        pt_create(pt, &c[i].pt_thread, pp_thr, &c[i]) ;
    }
    t0 = now_ns() ;
    while (protothread_run(pt)) ;
    t1 = now_ns() ;

    if (c[0].i != iters || c[1].i != iters) {
        fprintf(stderr, "benchmark did not run to completion\n") ;
        exit(1) ;
    }
    protothread_free(pt) ;
    /* each iteration blocks each thread once: 2 switches per iteration */
    return (t1 - t0) / (double)(iters * 2) ;
}

static pthread_mutex_t pp_mutex = PTHREAD_MUTEX_INITIALIZER ;
static pthread_cond_t pp_cond[2] = {
    PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER
} ;
static int pp_turn ;
static long pp_iters ;

static void *
pp_posix_thr(void *arg)
{
    long const me = (long)arg ;
    long i ;

    pthread_mutex_lock(&pp_mutex) ;
    for (i = 0; i < pp_iters; i++) {
        while (pp_turn != me) {
            pthread_cond_wait(&pp_cond[me], &pp_mutex) ;
        }
        pp_turn = 1 - (int)me ;
        /* one condvar per thread, so there are no spurious wakeups to
         * handle; this is the fast way to write it, in fairness to pthreads
         */
        pthread_cond_signal(&pp_cond[1 - me]) ;
    }
    pthread_mutex_unlock(&pp_mutex) ;
    return NULL ;
}

static double
bench_posix_switch(long iters)
{
    pthread_t th[2] ;
    double t0, t1 ;
    long i ;

    pp_turn = 0 ;
    pp_iters = iters ;
    t0 = now_ns() ;
    for (i = 0; i < 2; i++) {
        pthread_create(&th[i], NULL, pp_posix_thr, (void *)i) ;
    }
    for (i = 0; i < 2; i++) {
        pthread_join(th[i], NULL) ;
    }
    t1 = now_ns() ;
    return (t1 - t0) / (double)(iters * 2) ;
}

/******************************************************************************/
/* 2. Thread creation and destruction of a thread that does nothing.          */
/******************************************************************************/

/* volatile so the compiler cannot decide the whole benchmark is dead code */
static volatile long work_done ;

typedef struct create_bench_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
} create_bench_t ;

static pt_t
create_bench_thr(env_t const env)
{
    create_bench_t * const c = env ;
    pt_resume(c) ;
    work_done++ ;
    return PT_DONE ;
}

static double
bench_pt_create(long iters)
{
    protothread_t const pt = protothread_create() ;
    create_bench_t c ;
    double t0, t1 ;
    long i ;

    work_done = 0 ;
    t0 = now_ns() ;
    for (i = 0; i < iters; i++) {
        pt_create(pt, &c.pt_thread, create_bench_thr, &c) ;
        protothread_run(pt) ;
    }
    t1 = now_ns() ;
    protothread_free(pt) ;
    if (work_done != iters) {
        fprintf(stderr, "benchmark did not run: %ld of %ld\n",
                work_done, iters) ;
        exit(1) ;
    }
    return (t1 - t0) / (double)iters ;
}

static void *
noop_posix_thr(void *arg)
{
    (void)arg ;
    work_done++ ;
    return NULL ;
}

static double
bench_posix_create(long iters)
{
    double t0, t1 ;
    long i ;

    work_done = 0 ;
    t0 = now_ns() ;
    for (i = 0; i < iters; i++) {
        pthread_t th ;
        pthread_create(&th, NULL, noop_posix_thr, NULL) ;
        pthread_join(th, NULL) ;
    }
    t1 = now_ns() ;
    if (work_done != iters) {
        fprintf(stderr, "benchmark did not run: %ld of %ld\n",
                work_done, iters) ;
        exit(1) ;
    }
    return (t1 - t0) / (double)iters ;
}

/******************************************************************************/

int
main(int argc, char **argv)
{
    long switch_iters = 1000000 ;
    long create_iters = 100000 ;
    size_t pt_bytes, posix_bytes ;
    pthread_attr_t attr ;

    if (argc > 1) {
        switch_iters = atol(argv[1]) ;
    }
    if (argc > 2) {
        create_iters = atol(argv[2]) ;
    }

    printf("protothread %s vs. POSIX threads, PT_DEBUG=%d\n\n",
           PT_VERSION_STRING, PT_DEBUG) ;
    printf("  %-26s %10s %10s %10s\n", "", "protothread", "pthread", "ratio") ;
    printf("  %-26s %10s %10s %10s\n", "", "-----------", "-------", "-----") ;

    /* warm up, so the first measurement does not pay for page faults */
    (void)bench_pt_switch(1000) ;
    (void)bench_posix_switch(1000) ;

    report("context switch", "ns each",
           bench_pt_switch(switch_iters), bench_posix_switch(switch_iters)) ;
    report("create + destroy", "ns each",
           bench_pt_create(create_iters), bench_posix_create(create_iters)) ;

    /* Memory is not timed, but it is the other half of the story. Compare
     * against PTHREAD_STACK_MIN, not the default stack: the default is
     * reserved address space, most of which is never resident. The minimum
     * is the honest floor on what a thread actually costs.
     */
    pt_bytes = sizeof(pt_thread_t) + sizeof(pt_func_t) ;
    posix_bytes = (size_t)PTHREAD_STACK_MIN ;
    printf("  %-26s %10zu %10zu %10.0fx   (bytes per thread)\n",
           "memory (minimum)", pt_bytes, posix_bytes,
           (double)posix_bytes / (double)pt_bytes) ;

    pthread_attr_init(&attr) ;
    pthread_attr_getstacksize(&attr, &posix_bytes) ;
    pthread_attr_destroy(&attr) ;

    printf("\n  %ld switch iterations, %ld create iterations\n",
           switch_iters, create_iters) ;
    printf("  protothread memory is pt_thread_t + pt_func_t: the entire\n"
           "  per-thread cost, all of it resident.  pthread memory is\n"
           "  PTHREAD_STACK_MIN, the smallest stack the platform allows;\n"
           "  the default here is %zu bytes of reserved address space,\n"
           "  of which only the touched pages become resident.\n",
           posix_bytes) ;
    return 0 ;
}
