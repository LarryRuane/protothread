/**************************************************************/
/* DEMO/ASYNC_IO.C */
/* https://github.com/LarryRuane/protothread */
/* Copyright (c) 2008-present Larry Ruane */
/* Distributed under the MIT software license, see the accompanying */
/* file LICENSE or https://opensource.org/licenses/MIT. */
/* SPDX-License-Identifier: MIT */
/**************************************************************/

/* Protothreads driving asynchronous I/O, with no POSIX threads at all.
 *
 * This is the arrangement most event-driven programs want, and the one
 * protothreads suit best. Each protothread runs a short sequence of I/Os,
 * calling pt_wait() between them; the event loop blocks in poll() only when
 * no protothread can run. Every protothread reads as straight-line code --
 * start the I/O, wait for it, use the result, do the next one -- with no
 * callbacks and no hand-written state machine, even though every protothread
 * is somewhere different in its own sequence at any moment.
 *
 * Run with -v to watch that happen. The submissions and completions come out
 * thoroughly interleaved and in no useful order, which is the point: no
 * protothread is written to cope with that, and none has to be.
 *
 * That order also varies from run to run, which is worth being clear about,
 * because protothreads are advertised as deterministic and they still are.
 * The scheduling here is fully determined by the completion events; what
 * varies is when the simulated device delivers them, because its deadlines
 * are anchored to the wall clock. A test that wanted a reproducible order
 * would simulate time as well -- exactly the point made under "Deterministic
 * execution" in the README. A demo of asynchronous I/O is more honest with a
 * device that behaves like one.
 *
 * The device is simulated, and it is worth being exact about which part.
 * The pipes, the poll(), the blocking and the completion handler are all
 * real. What is simulated is the latency: a request is handed to the device
 * with a deadline, and the loop writes the reply into the pipe once that
 * deadline passes, so replies arrive late and out of order the way a real
 * device's do. Merging timers and I/O readiness into one poll() call like
 * this is itself what real event loops do.
 *
 * Completions become pt_signal() calls on the thread that owns
 * protothread_run(), and only between runs -- the rule explained at length
 * in pool.c. Here it costs nothing: there is only one thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

#include "protothread.h"

#define NIO    20       /* protothreads, each holding a pipe open */
#define MAXOP   4       /* most I/Os one protothread will do */
#define MAXLAT 20       /* most milliseconds the device will take */

typedef struct io_context_s {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    int id ;
    int fd[2] ;         /* fd[0] is the completion end, fd[1] the submit end */
    int op ;            /* which I/O of this protothread's sequence */
    int nop ;           /* how many it will do */
    int pending ;       /* an I/O is outstanding, so deadline is meaningful */
    unsigned deadline ; /* when the device will finish the current I/O */
    long value ;        /* in: the request; out: the reply */
    int done ;          /* set by the completion handler, below */
} io_context_t ;

static io_context_t ctx[NIO] ;
static long checksum ;
static int completed ;
static int verbose ;
static unsigned start ;

static void
fail(const char * const what)
{
    perror(what) ;
    exit(1) ;
}

static unsigned
now_ms(void)
{
    struct timespec ts ;

    clock_gettime(CLOCK_MONOTONIC, &ts) ;
    return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000) ;
}

/* Any small generator will do; this one just needs to vary the latencies. */
static unsigned
next_random(unsigned const limit)
{
    static unsigned state = 12345 ;

    state ^= state << 13 ;
    state ^= state >> 17 ;
    state ^= state << 5 ;
    return state % limit ;
}

static void
trace(const char * const what, const io_context_t * const c)
{
    if (verbose) {
        printf("%4u ms  %-6s  protothread %2d  op %d/%d\n",
               now_ms() - start, what, c->id, c->op + 1, c->nop) ;
    }
}

/* The device. A reply whose deadline has arrived is written into the pipe,
 * which is how the completion handler below will find out about it.
 */
static void
device_deliver(unsigned const now)
{
    int i ;

    for (i = 0; i < NIO; i++) {
        io_context_t * const c = &ctx[i] ;
        if (c->pending && (int)(now - c->deadline) >= 0) {
            c->pending = 0 ;
            if (write(c->fd[1], &c->value, sizeof(c->value)) !=
                    (ssize_t)sizeof(c->value)) {
                fail("write") ;
            }
        }
    }
}

/* How long poll() may sleep: until the device owes us the earliest reply. */
static int
next_timeout(unsigned const now)
{
    int timeout = -1 ;
    int i ;

    for (i = 0; i < NIO; i++) {
        const io_context_t * const c = &ctx[i] ;
        if (c->pending) {
            const int ms = (int)(c->deadline - now) ;
            if (timeout < 0 || ms < timeout) {
                timeout = ms < 0 ? 0 : ms ;
            }
        }
    }
    return timeout ;
}

static pt_t
io_thr(env_t const env)
{
    io_context_t * const c = env ;
    pt_resume(c) ;

    /* c->op lives in the context, not on the stack, so the loop survives
     * the pt_wait() in its body -- see "Local variables" in the README.
     */
    for (c->op = 0; c->op < c->nop; c->op++) {
        /* start the I/O; this returns at once, the reply arrives later */
        c->value = c->id * 100 + c->op ;
        c->deadline = now_ms() + next_random(MAXLAT) ;
        c->pending = 1 ;
        c->done = 0 ;
        trace("submit", c) ;

        /* every other protothread runs, at its own point in its own
         * sequence, while this one waits
         */
        while (!c->done) {
            pt_wait(c, &c->done) ;
        }

        if (read(c->fd[0], &c->value, sizeof(c->value)) !=
                (ssize_t)sizeof(c->value)) {
            fail("read") ;
        }
        trace("done", c) ;
        checksum += c->value ;
    }
    completed++ ;
    return PT_DONE ;
}

int
main(int argc, char **argv)
{
    struct protothread_s state ;
    struct pollfd pfd[NIO] ;
    long expect = 0 ;
    int i ;

    verbose = argc > 1 && strcmp(argv[1], "-v") == 0 ;
    start = now_ms() ;

    protothread_init(&state) ;
    for (i = 0; i < NIO; i++) {
        int op ;

        if (pipe(ctx[i].fd) < 0) {
            fail("pipe") ;
        }
        ctx[i].id = i ;
        ctx[i].nop = 1 + (int)next_random(MAXOP) ;
        pfd[i].fd = ctx[i].fd[0] ;
        pfd[i].events = POLLIN ;
        pt_create(&state, &ctx[i].pt_thread, io_thr, &ctx[i]) ;
        for (op = 0; op < ctx[i].nop; op++) {
            expect += i * 100 + op ;
        }
    }

    while (completed < NIO) {
        /* run everything that can run; each one starts an I/O and blocks */
        while (protothread_run(&state)) {
        }
        if (completed == NIO) {
            break ;
        }

        /* no protothread can run, so let the device deliver what it owes,
         * then sleep until it owes the next one (or a reply is readable)
         */
        device_deliver(now_ms()) ;
        if (poll(pfd, NIO, next_timeout(now_ms())) < 0) {
            fail("poll") ;
        }

        /* the completion handler: scheduler context, between runs */
        for (i = 0; i < NIO; i++) {
            if (pfd[i].revents & POLLIN) {
                ctx[i].done = 1 ;
                pt_signal(&state, &ctx[i].done) ;
            }
        }
    }

    for (i = 0; i < NIO; i++) {
        close(ctx[i].fd[0]) ;
        close(ctx[i].fd[1]) ;
    }
    protothread_deinit(&state) ;

    printf("%d protothreads, no POSIX threads, "
           "checksum %ld (expected %ld): %s\n",
           NIO, checksum, expect,
           checksum == expect ? "OK" : "MISMATCH") ;
    return checksum != expect ;
}
