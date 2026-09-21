/**************************************************************/
/* DEMO/POOL.C */
/* https://github.com/LarryRuane/protothread */
/* Copyright (c) 2008-present Larry Ruane */
/* Distributed under the MIT software license, see the accompanying */
/* file LICENSE or https://opensource.org/licenses/MIT. */
/* SPDX-License-Identifier: MIT */
/**************************************************************/

/* Using protothreads on a multi-core machine.
 *
 * Protothreads run one at a time on a single thread, so they are the wrong
 * tool for parallel computation and the right tool for concurrent control
 * flow. The two compose: a pool of POSIX threads does the heavy lifting on
 * every core, while protothreads own the sequencing. A protothread submits
 * work, blocks, and resumes when the result is ready -- other protothreads
 * run in the meantime.
 *
 * The one thing to get right is how a worker wakes a protothread. It must
 * NOT call pt_signal() itself, even with the PT_CRITICAL_* macros defined.
 * Those keep the scheduler's lists intact, but they cannot close this race:
 *
 *      protothread                     worker thread
 *      -----------                     -------------
 *      while (!job->done)     <--- tests the predicate: false
 *                                      job->done = 1
 *                                      pt_signal(pt, job)  <-- nothing is
 *                                          waiting yet, so this is LOST
 *          pt_wait(c, job)    <--- enqueues, and sleeps forever
 *
 * The test and the enqueue are not atomic with respect to another thread.
 * So the worker posts to a completion queue instead, and the thread that
 * owns the scheduler turns that into a pt_signal() between protothread
 * runs, where nothing can interleave. A protothread has always finished
 * enqueuing by then.
 *
 * The same reasoning, and the same fix, applies to an interrupt handler.
 *
 * A pleasant consequence: because pt_signal() is only ever called from the
 * scheduler's own thread, the protothread lists are never touched
 * concurrently, and PT_CRITICAL_* is not needed at all.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "protothread.h"

#define NJOBS    500
#define NWORKERS 4

typedef struct job_s {
    long input ;
    long result ;
    int done ;              /* only ever written by the scheduler thread */
} job_t ;

/* ---- the work queue: scheduler -> workers ---- */
static pthread_mutex_t work_mutex = PTHREAD_MUTEX_INITIALIZER ;
static pthread_cond_t work_cond = PTHREAD_COND_INITIALIZER ;
static job_t * work_queue[NJOBS] ;
static int work_n ;
static int shutting_down ;

/* ---- the completion queue: workers -> scheduler ---- */
static pthread_mutex_t done_mutex = PTHREAD_MUTEX_INITIALIZER ;
static pthread_cond_t done_cond = PTHREAD_COND_INITIALIZER ;
static job_t * done_queue[NJOBS] ;
static int done_n ;

static void
submit(job_t *j)
{
    pthread_mutex_lock(&work_mutex) ;
    work_queue[work_n++] = j ;
    pthread_cond_signal(&work_cond) ;
    pthread_mutex_unlock(&work_mutex) ;
}

static void *
worker(void *arg)
{
    (void)arg ;
    for (;;) {
        job_t *j ;

        pthread_mutex_lock(&work_mutex) ;
        while (work_n == 0 && !shutting_down) {
            pthread_cond_wait(&work_cond, &work_mutex) ;
        }
        if (work_n == 0) {
            pthread_mutex_unlock(&work_mutex) ;
            return NULL ;       /* shutting down */
        }
        j = work_queue[--work_n] ;
        pthread_mutex_unlock(&work_mutex) ;

        /* the compute-intensive part, on whatever core this thread is on */
        j->result = j->input * j->input ;

        /* publish the finished job; do NOT pt_signal() from here */
        pthread_mutex_lock(&done_mutex) ;
        done_queue[done_n++] = j ;
        pthread_cond_signal(&done_cond) ;
        pthread_mutex_unlock(&done_mutex) ;
    }
}

/* ---- the protothread side ---- */
typedef struct pool_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    job_t job ;
    long id ;
} pool_context_t ;

static long checksum ;
static int completed ;

static pt_t
pool_thr(env_t const env)
{
    pool_context_t * const c = env ;
    pt_resume(c) ;

    c->job.input = c->id ;
    c->job.done = 0 ;
    submit(&c->job) ;

    /* other protothreads run while this one is blocked */
    while (!c->job.done) {
        pt_wait(c, &c->job) ;
    }

    checksum += c->job.result ;
    completed++ ;
    return PT_DONE ;
}

/* Turn finished jobs into pt_signal()s. Called only from the scheduler
 * thread, and only between protothread runs, which is what makes it safe.
 */
static int
drain_completions(protothread_t const s)
{
    job_t * batch[NJOBS] ;
    int n = 0 ;
    int i ;

    pthread_mutex_lock(&done_mutex) ;
    while (done_n > 0) {
        batch[n++] = done_queue[--done_n] ;
    }
    pthread_mutex_unlock(&done_mutex) ;

    for (i = 0; i < n; i++) {
        batch[i]->done = 1 ;
        pt_signal(s, batch[i]) ;
    }
    return n ;
}

int
main(void)
{
    struct protothread_s state ;
    static pool_context_t ctx[NJOBS] ;
    pthread_t workers[NWORKERS] ;
    long expect = 0 ;
    int i ;

    protothread_init(&state) ;
    for (i = 0; i < NWORKERS; i++) {
        pthread_create(&workers[i], NULL, worker, NULL) ;
    }
    for (i = 0; i < NJOBS; i++) {
        ctx[i].id = i ;
        pt_create(&state, &ctx[i].pt_thread, pool_thr, &ctx[i]) ;
        expect += (long)i * i ;
    }

    /* The scheduler loop: hand back results, run whatever that made
     * runnable, then sleep until a worker has something new.
     */
    while (completed < NJOBS) {
        drain_completions(&state) ;
        while (protothread_run(&state)) {
            /* run every ready protothread */
        }
        if (completed == NJOBS) {
            break ;
        }
        pthread_mutex_lock(&done_mutex) ;
        while (done_n == 0) {
            pthread_cond_wait(&done_cond, &done_mutex) ;
        }
        pthread_mutex_unlock(&done_mutex) ;
    }

    pthread_mutex_lock(&work_mutex) ;
    shutting_down = 1 ;
    pthread_cond_broadcast(&work_cond) ;
    pthread_mutex_unlock(&work_mutex) ;
    for (i = 0; i < NWORKERS; i++) {
        pthread_join(workers[i], NULL) ;
    }
    protothread_deinit(&state) ;

    printf("%d protothreads, %d worker threads, "
           "checksum %ld (expected %ld): %s\n",
           NJOBS, NWORKERS, checksum, expect,
           checksum == expect ? "OK" : "MISMATCH") ;
    return checksum != expect ;
}
