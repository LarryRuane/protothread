/**************************************************************/
/* DEMO/HELPER_THREAD.C */
/* https://github.com/LarryRuane/protothread */
/* Copyright (c) 2008-present Larry Ruane */
/* Distributed under the MIT software license, see the accompanying */
/* file LICENSE or https://opensource.org/licenses/MIT. */
/* SPDX-License-Identifier: MIT */
/**************************************************************/

/* One POSIX thread per blocking call: a temporary extension of a protothread.
 *
 * pool.c keeps a fixed set of worker threads and a work queue, which is what
 * you want when the offloaded work is constant and hot. This demo is for the
 * opposite case: a call that blocks, that has no asynchronous form, and that
 * happens rarely enough that pthread_create() and pthread_join() cost less
 * than owning and tuning a pool. The helper thread exists only for the
 * duration of one call, and the protothread that started it joins it.
 *
 * The rule from pool.c still holds: the helper must not call pt_signal()
 * itself, however tempting, because the protothread may not have finished
 * enqueuing yet and the wakeup would be lost. It writes its id to a
 * self-pipe instead. That makes this demo a combination of the other two --
 * POSIX threads for the part that blocks, and the poll() loop from
 * async_io.c to turn completions into signals in scheduler context.
 *
 * A third arrangement sits between this one and pool.c: give every
 * protothread a helper of its own, created at startup and parked between
 * calls. The protothreads still share the scheduler thread, so switching
 * between them is still a computed goto. Over a pool it buys two things --
 * no protothread ever queues behind another waiting for a free worker, and
 * a helper can hold state across calls, such as a connection or a
 * thread-bound library handle that a stateless pool worker cannot keep. For
 * some synchronous libraries that second point is a requirement rather than
 * an optimization. It costs about 8 kB resident per parked helper against
 * 64 bytes for the protothread itself, which suits tens or hundreds of
 * long-lived protothreads and not the tens of thousands this library is
 * otherwise happy to run. (The default 8 MB stack is virtual and lazily
 * committed, so pthread_attr_setstacksize() governs address space, not
 * footprint.)
 *
 * What none of the three extends to is running the protothreads themselves
 * on separate threads: that is POSIX threads with extra steps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <pthread.h>

#include "protothread.h"

#define NREQ 64

typedef struct helper_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    pthread_t tid ;
    int id ;            /* index into ctx[], the token sent down the pipe */
    long value ;        /* in: the argument */
    long result ;       /* out: written by the helper, read after the join */
    int done ;          /* set by the completion handler, below */
} helper_context_t ;

static int notify[2] ;  /* self-pipe, helper threads -> scheduler */
static long checksum ;
static int completed ;

static void
fail(const char * const what)
{
    perror(what) ;
    exit(1) ;
}

static void *
helper(void * const arg)
{
    helper_context_t * const c = arg ;
    const struct timespec ts = { 0, 1000000 } ;

    /* Stand-in for the synchronous call we came here to make: something
     * with no non-blocking form, that would otherwise stall every
     * protothread for as long as it ran.
     */
    nanosleep(&ts, NULL) ;
    c->result = c->value * c->value ;

    /* Report completion; do NOT pt_signal() from here. A write this small
     * is atomic, so helpers need no lock between them.
     */
    if (write(notify[1], &c->id, sizeof(c->id)) != (ssize_t)sizeof(c->id)) {
        fail("write") ;
    }
    return NULL ;
}

static pt_t
helper_thr(env_t const env)
{
    helper_context_t * const c = env ;
    pt_resume(c) ;

    /* hand the blocking call to a thread of its own, then get out of the way */
    c->done = 0 ;
    if (pthread_create(&c->tid, NULL, helper, c) != 0) {
        fail("pthread_create") ;
    }
    while (!c->done) {
        pt_wait(c, &c->done) ;
    }

    /* The helper wrote to the pipe as its last act, so it is already on its
     * way out: this joins an all-but-dead thread rather than blocking on a
     * running one. It is also what makes c->result safe to read.
     */
    pthread_join(c->tid, NULL) ;

    checksum += c->result ;
    completed++ ;
    return PT_DONE ;
}

int
main(void)
{
    struct protothread_s state ;
    static helper_context_t ctx[NREQ] ;
    struct pollfd pfd ;
    long expect = 0 ;
    int i ;

    if (pipe(notify) < 0) {
        fail("pipe") ;
    }
    protothread_init(&state) ;
    for (i = 0; i < NREQ; i++) {
        ctx[i].id = i ;
        ctx[i].value = i ;
        pt_create(&state, &ctx[i].pt_thread, helper_thr, &ctx[i]) ;
        expect += (long)i * i ;
    }

    pfd.fd = notify[0] ;
    pfd.events = POLLIN ;
    while (completed < NREQ) {
        int ids[NREQ] ;
        ssize_t n ;
        int nid ;

        /* run everything that can run; each one spawns a helper and blocks */
        while (protothread_run(&state)) {
        }
        if (completed == NREQ) {
            break ;
        }

        /* no protothread can run, so sleep until a helper reports in */
        if (poll(&pfd, 1, -1) < 0) {
            fail("poll") ;
        }

        /* the completion handler: scheduler context, between runs */
        n = read(notify[0], ids, sizeof(ids)) ;
        if (n < 0) {
            fail("read") ;
        }
        nid = (int)(n / (ssize_t)sizeof(ids[0])) ;
        for (i = 0; i < nid; i++) {
            ctx[ids[i]].done = 1 ;
            pt_signal(&state, &ctx[ids[i]].done) ;
        }
    }

    close(notify[0]) ;
    close(notify[1]) ;
    protothread_deinit(&state) ;

    printf("%d protothreads, one helper thread each, checksum %ld (expected %ld): %s\n",
           NREQ, checksum, expect,
           checksum == expect ? "OK" : "MISMATCH") ;
    return checksum != expect ;
}
