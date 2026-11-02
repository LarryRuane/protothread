/**************************************************************/
/* DEMO/TPCC.C */
/* https://github.com/LarryRuane/protothread */
/* Copyright (c) 2008-present Larry Ruane */
/* Distributed under the MIT software license, see the accompanying */
/* file LICENSE or https://opensource.org/licenses/MIT. */
/* SPDX-License-Identifier: MIT */
/**************************************************************/

/* A load-generating client modeled on the TPC-C benchmark, run against a
 * database simulated in the same process.
 *
 * TPC-C models a wholesale supplier. Each warehouse supplies ten districts,
 * and each district has a terminal where a clerk enters transactions:
 * New-Order (45%), Payment (43%), and Order-Status, Delivery, and
 * Stock-Level (4% each). A clerk types (keying time), waits for the reply,
 * reads it (think time), and starts again, so a terminal is idle almost all
 * the time. A large run has hundreds of thousands of terminals.
 *
 * Here each terminal is one protothread, written as the loop a clerk
 * follows. Each transaction is a nested protothread function, and each
 * query inside it is another, which sends a request to the database and
 * blocks until the reply comes back. The database is sharded by warehouse,
 * as a distributed database would be, and each shard is a few worker
 * protothreads taking requests from a queue. Transactions lock the rows
 * they update, using protothread_lock.h, and hold the locks until they
 * finish.
 *
 * The clock is simulated. When nothing is ready to run, main() jumps it to
 * the next timer deadline, so ten simulated minutes take well under a
 * second, and every run with the same seed gives the same output.
 *
 *   ./pttpcc                  100 warehouses, 1000 terminals
 *   ./pttpcc -w 10000         100,000 terminals, one OS thread
 *   ./pttpcc -u               no row locks: watch the consistency check fail
 *   ./pttpcc -H               start every terminal at once (thundering herd)
 *   ./pttpcc -m 10 -s 1       simulated minutes, random seed
 *
 * This is not a TPC-C implementation, and its numbers are not tpmC. It keeps
 * TPC-C's structure (the transaction mix, keying and think times, and where
 * the contention is) and leaves out the data that doesn't cause contention.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

#ifndef PT_NWAIT
#define PT_NWAIT (1 << 17)      /* about one wait list per waiting terminal */
#endif
#define PT_NTIMERS (1 << 18)    /* every terminal and worker can sleep */

#include "protothread.h"
#include "protothread_lock.h"
#include "protothread_queue.h"
#include "protothread_timer.h"

#define DISTRICTS 10            /* per warehouse, and one terminal each */
#define SHARD_WAREHOUSES 10     /* warehouses per database shard */
#define SHARD_WORKERS 4         /* worker protothreads per shard */
#define HIST_MS 10000           /* latency histogram range, in ms */

/* ---- the simulated database ---- */

typedef struct {
    pt_lock_t lock ;
    long ytd ;                  /* year-to-date payments */
} warehouse_t ;

typedef struct {
    pt_lock_t lock ;
    long ytd ;
    long next_o_id ;            /* the New-Order hot spot */
    long orders ;
} district_t ;

static warehouse_t *warehouses ;
static district_t *districts ;  /* [w * DISTRICTS + d] */
static long *paid ;             /* client side: committed, per warehouse */

typedef enum {
    Q_OTHER,                    /* costs a round trip, touches no stored data */
    Q_READ_NEXT_O_ID,
    Q_WRITE_NEXT_O_ID,
    Q_INSERT_ORDER,
    Q_READ_W_YTD,
    Q_WRITE_W_YTD,
    Q_READ_D_YTD,
    Q_WRITE_D_YTD,
} query_op_t ;

typedef struct {
    query_op_t op ;
    int w, d ;
    long value ;                /* in for a write, out for a read */
    bool_t done ;               /* set by a worker; the terminal waits on it */
} request_t ;

PT_QUEUE_DEFINE(reqq, request_t *)
static reqq_t *shards ;         /* one request queue per shard */
static unsigned int peak_queued ;

static pt_timers_t timers ;     /* static: PT_NTIMERS makes it large */
static unsigned long long rng = 88172645463325252ull ;

/* xorshift64; deterministic, so a run can be repeated exactly */
static unsigned int
rnd(unsigned int n)
{
    rng ^= rng << 13 ;
    rng ^= rng >> 7 ;
    rng ^= rng << 17 ;
    return (unsigned int)(rng % n) ;
}

static void
db_apply(request_t * const r)
{
    district_t * const dist = &districts[r->w * DISTRICTS + r->d] ;

    switch (r->op) {
    case Q_OTHER: break ;
    case Q_READ_NEXT_O_ID: r->value = dist->next_o_id ; break ;
    case Q_WRITE_NEXT_O_ID: dist->next_o_id = r->value ; break ;
    case Q_INSERT_ORDER: dist->orders++ ; break ;
    case Q_READ_W_YTD: r->value = warehouses[r->w].ytd ; break ;
    case Q_WRITE_W_YTD: warehouses[r->w].ytd = r->value ; break ;
    case Q_READ_D_YTD: r->value = dist->ytd ; break ;
    case Q_WRITE_D_YTD: dist->ytd = r->value ; break ;
    }
}

typedef struct {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    pt_timer_env_t timer_env ;
    reqq_t *requests ;          /* this worker's shard */
    request_t *req ;
    pt_time_t service ;
} worker_context_t ;

static pt_t
db_worker_thr(env_t const env)
{
    worker_context_t * const c = env ;
    pt_resume(c) ;

    for (;;) {
        pt_queue_receive(c, c->requests, &c->req) ;
        c->service = 1 + rnd(2) ;  /* 1 or 2 ms */
        pt_sleep(c, &c->timer_env, &timers, c->service) ;
        db_apply(c->req) ;
        c->req->done = true ;
        pt_broadcast(pt_get_pt(c), &c->req->done) ;
    }
    return PT_DONE ;
}

/* ---- one query: send it, and block until the reply ---- */

typedef struct {
    pt_func_t pt_func ;
    request_t req ;
    request_t *reqp ;           /* what's queued; must survive the send */
} query_context_t ;

static pt_t
db_query(query_context_t * const c, query_op_t op, int w, int d, long value)
{
    pt_resume(c) ;

    c->req = (request_t){ .op = op, .w = w, .d = d, .value = value } ;
    c->reqp = &c->req ;
    pt_queue_send(c, &shards[w / SHARD_WAREHOUSES], &c->reqp) ;
    if (peak_queued < reqq_count(&shards[w / SHARD_WAREHOUSES])) {
        peak_queued = reqq_count(&shards[w / SHARD_WAREHOUSES]) ;
    }
    while (!c->req.done) {
        pt_wait(c, &c->req.done) ;
    }
    return PT_DONE ;
}

/* ---- the transactions ---- */

static bool_t use_locks = true ;
static unsigned long lock_waits ;

/* acquire a row lock for writing, counting the times it had to wait */
#define LOCK(c, env, row) \
    do { \
        if (use_locks) { \
            pt_lock_acquire_write(c, env, &(row)->lock) ; \
            lock_waits += pt_call_waited(c) ; \
        } \
    } while (0)
#define UNLOCK(env, row) \
    do { \
        if (use_locks) { \
            pt_lock_release_write(env, &(row)->lock) ; \
        } \
    } while (0)

typedef struct {
    pt_func_t pt_func ;
    pt_lock_env_t lock_env ;
    query_context_t q ;
    long o_id ;
    int items ;
    int i ;
} new_order_context_t ;

static pt_t
tx_new_order(new_order_context_t * const c, int w, int d)
{
    district_t * const dist = &districts[w * DISTRICTS + d] ;
    pt_resume(c) ;

    LOCK(c, &c->lock_env, dist) ;
    pt_call(c, db_query, &c->q, Q_READ_NEXT_O_ID, w, d, 0) ;
    c->o_id = c->q.req.value ;
    /* without the lock, another terminal can read the same o_id here */
    pt_call(c, db_query, &c->q, Q_WRITE_NEXT_O_ID, w, d, c->o_id + 1) ;
    pt_call(c, db_query, &c->q, Q_INSERT_ORDER, w, d, 0) ;
    c->items = 5 + rnd(11) ;
    for (c->i = 0; c->i < c->items; c->i++) {
        /* read the item, update its stock, insert the order line */
        pt_call(c, db_query, &c->q, Q_OTHER, w, d, 0) ;
    }
    UNLOCK(&c->lock_env, dist) ;
    return PT_DONE ;
}

typedef struct {
    pt_func_t pt_func ;
    pt_lock_env_t w_lock_env ;
    pt_lock_env_t d_lock_env ;
    query_context_t q ;
    long amount ;
} payment_context_t ;

static pt_t
tx_payment(payment_context_t * const c, int w, int d)
{
    warehouse_t * const wh = &warehouses[w] ;
    district_t * const dist = &districts[w * DISTRICTS + d] ;
    pt_resume(c) ;

    c->amount = 1 + rnd(5000) ;
    /* always warehouse, then district, so two payments can't deadlock */
    LOCK(c, &c->w_lock_env, wh) ;
    LOCK(c, &c->d_lock_env, dist) ;
    pt_call(c, db_query, &c->q, Q_READ_W_YTD, w, d, 0) ;
    pt_call(c, db_query, &c->q, Q_WRITE_W_YTD, w, d,
            c->q.req.value + c->amount) ;
    pt_call(c, db_query, &c->q, Q_READ_D_YTD, w, d, 0) ;
    pt_call(c, db_query, &c->q, Q_WRITE_D_YTD, w, d,
            c->q.req.value + c->amount) ;
    pt_call(c, db_query, &c->q, Q_OTHER, w, d, 0) ;     /* customer */
    pt_call(c, db_query, &c->q, Q_OTHER, w, d, 0) ;     /* history */
    UNLOCK(&c->d_lock_env, dist) ;
    UNLOCK(&c->w_lock_env, wh) ;
    paid[w] += c->amount ;
    return PT_DONE ;
}

typedef struct {
    pt_func_t pt_func ;
    pt_lock_env_t lock_env ;
    query_context_t q ;
    int d ;
    int i ;
} other_context_t ;

/* customer, their latest order, its lines; reads only, no locks */
static pt_t
tx_order_status(other_context_t * const c, int w, int d)
{
    pt_resume(c) ;

    for (c->i = 0; c->i < 3; c->i++) {
        pt_call(c, db_query, &c->q, Q_OTHER, w, d, 0) ;
    }
    return PT_DONE ;
}

/* deliver the oldest undelivered order in each district, one at a time */
static pt_t
tx_delivery(other_context_t * const c, int w)
{
    pt_resume(c) ;

    for (c->d = 0; c->d < DISTRICTS; c->d++) {
        LOCK(c, &c->lock_env, &districts[w * DISTRICTS + c->d]) ;
        for (c->i = 0; c->i < 3; c->i++) {
            pt_call(c, db_query, &c->q, Q_OTHER, w, c->d, 0) ;
        }
        UNLOCK(&c->lock_env, &districts[w * DISTRICTS + c->d]) ;
    }
    return PT_DONE ;
}

/* recent order lines whose stock is low; a read lock on the district */
static pt_t
tx_stock_level(other_context_t * const c, int w, int d)
{
    district_t * const dist = &districts[w * DISTRICTS + d] ;
    pt_resume(c) ;

    if (use_locks) {
        pt_lock_acquire_read(c, &c->lock_env, &dist->lock) ;
        lock_waits += pt_call_waited(c) ;
    }
    pt_call(c, db_query, &c->q, Q_READ_NEXT_O_ID, w, d, 0) ;
    pt_call(c, db_query, &c->q, Q_OTHER, w, d, 0) ;
    if (use_locks) {
        pt_lock_release_read(&c->lock_env, &dist->lock) ;
    }
    return PT_DONE ;
}

/* ---- the terminal: the loop a clerk follows ---- */

typedef enum { NEW_ORDER, PAYMENT, ORDER_STATUS, DELIVERY, STOCK_LEVEL,
               NTYPES } tx_type_t ;
static char const * const type_name[NTYPES] = {
    "New-Order", "Payment", "Order-Status", "Delivery", "Stock-Level",
} ;
static unsigned int const keying_ms[NTYPES] =
    { 18000, 3000, 2000, 2000, 2000 } ;
static unsigned int const think_ms[NTYPES] =
    { 12000, 12000, 10000, 5000, 5000 } ;

static unsigned long tx_count[NTYPES] ;
static unsigned long long tx_total_ms[NTYPES] ;
static unsigned long histogram[NTYPES][HIST_MS + 1] ;

static pt_time_t end_time ;
static bool_t herd ;

typedef struct {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    pt_timer_env_t timer_env ;
    int w ;
    int d ;                     /* the district for this transaction */
    tx_type_t type ;
    pt_time_t start ;
    pt_time_t pause ;           /* keying or think time */
    union {                     /* only one transaction runs at a time */
        new_order_context_t new_order ;
        payment_context_t payment ;
        other_context_t other ;
    } tx ;
} terminal_context_t ;

static tx_type_t
pick_transaction(void)
{
    unsigned int const r = rnd(100) ;
    return r < 45 ? NEW_ORDER : r < 88 ? PAYMENT : r < 92 ? ORDER_STATUS
         : r < 96 ? DELIVERY : STOCK_LEVEL ;
}

static void
record(tx_type_t type, pt_time_t ms)
{
    tx_count[type]++ ;
    tx_total_ms[type] += ms ;
    histogram[type][ms < HIST_MS ? ms : HIST_MS]++ ;
}

static pt_t
terminal_thr(env_t const env)
{
    terminal_context_t * const c = env ;
    pt_resume(c) ;

    if (!herd) {
        /* stagger the start, so the terminals don't all begin at once */
        c->pause = rnd(keying_ms[NEW_ORDER]) ;
        pt_sleep(c, &c->timer_env, &timers, c->pause) ;
    }
    while (pt_time_after(end_time, timers.now)) {
        c->type = pick_transaction() ;
        pt_sleep(c, &c->timer_env, &timers, keying_ms[c->type]) ;

        c->start = timers.now ;
        /* chosen here, not in the calls: pt_call() re-evaluates its
         * arguments every time the call resumes
         */
        c->d = rnd(DISTRICTS) ;
        memset(&c->tx, 0, sizeof(c->tx)) ;
        switch (c->type) {
        case NEW_ORDER:
            pt_call(c, tx_new_order, &c->tx.new_order, c->w, c->d) ;
            break ;
        case PAYMENT:
            pt_call(c, tx_payment, &c->tx.payment, c->w, c->d) ;
            break ;
        case ORDER_STATUS:
            pt_call(c, tx_order_status, &c->tx.other, c->w, c->d) ;
            break ;
        case DELIVERY:
            pt_call(c, tx_delivery, &c->tx.other, c->w) ;
            break ;
        case STOCK_LEVEL:
            pt_call(c, tx_stock_level, &c->tx.other, c->w, c->d) ;
            break ;
        default:
            break ;
        }
        record(c->type, timers.now - c->start) ;

        /* TPC-C's think time is exponential; uniform has the same mean */
        c->pause = rnd(2 * think_ms[c->type] + 1) ;
        pt_sleep(c, &c->timer_env, &timers, c->pause) ;
    }
    return PT_DONE ;
}

/* ---- main: run until every terminal is done ---- */

static pt_time_t
percentile(tx_type_t type, unsigned int pct)
{
    unsigned long const want = (tx_count[type] * pct + 99) / 100 ;
    unsigned long seen = 0 ;

    for (pt_time_t ms = 0; ms <= HIST_MS; ms++) {
        seen += histogram[type][ms] ;
        if (seen >= want) {
            return ms ;
        }
    }
    return HIST_MS ;
}

static int
check_consistency(int nwarehouses)
{
    long lost_ids = 0, total = 0, lost_w = 0, lost_d = 0 ;

    for (int w = 0; w < nwarehouses; w++) {
        long d_sum = 0 ;
        for (int d = 0; d < DISTRICTS; d++) {
            district_t const * const dist = &districts[w * DISTRICTS + d] ;
            d_sum += dist->ytd ;
            /* order ids start at 1, and each order should take a new one */
            lost_ids += dist->orders - (dist->next_o_id - 1) ;
        }
        total += paid[w] ;
        lost_w += paid[w] - warehouses[w].ytd ;
        lost_d += paid[w] - d_sum ;
    }
    if (lost_ids == 0 && lost_w == 0 && lost_d == 0) {
        printf("consistency: OK\n") ;
        return 0 ;
    }
    printf("consistency: FAILED, lost updates\n"
           "  %ld order ids handed out twice\n"
           "  $%ld of $%ld in payments missing from warehouse totals, "
           "$%ld from district totals\n", lost_ids, lost_w, total, lost_d) ;
    return 1 ;
}

static void
usage(void)
{
    fprintf(stderr, "usage: pttpcc [-w warehouses] [-m minutes] [-s seed] "
            "[-u] [-H]\n") ;
    exit(2) ;
}

int
main(int argc, char **argv)
{
    static struct protothread_s pt ;    /* static: PT_NWAIT makes it large */
    int nwarehouses = 100 ;
    int minutes = 10 ;
    clock_t const cpu_start = clock() ;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-u")) {
            use_locks = false ;
        } else if (!strcmp(argv[i], "-H")) {
            herd = true ;
        } else if (i + 1 < argc && !strcmp(argv[i], "-w")) {
            nwarehouses = atoi(argv[++i]) ;
        } else if (i + 1 < argc && !strcmp(argv[i], "-m")) {
            minutes = atoi(argv[++i]) ;
        } else if (i + 1 < argc && !strcmp(argv[i], "-s")) {
            rng += strtoull(argv[++i], NULL, 0) ;
        } else {
            usage() ;
        }
    }
    int const nterminals = nwarehouses * DISTRICTS ;
    int const nshards =
        (nwarehouses + SHARD_WAREHOUSES - 1) / SHARD_WAREHOUSES ;
    int const nworkers = nshards * SHARD_WORKERS ;
    if (nwarehouses < 1 || minutes < 1 || nterminals + nworkers > PT_NTIMERS) {
        usage() ;
    }

    warehouses = calloc(nwarehouses, sizeof(*warehouses)) ;
    districts = calloc(nterminals, sizeof(*districts)) ;
    paid = calloc(nwarehouses, sizeof(*paid)) ;
    shards = calloc(nshards, sizeof(*shards)) ;
    request_t ** const slots = malloc(nterminals * sizeof(*slots)) ;
    terminal_context_t * const terminals =
        calloc(nterminals, sizeof(*terminals)) ;
    worker_context_t * const workers = calloc(nworkers, sizeof(*workers)) ;
    if (!warehouses || !districts || !paid || !shards || !slots
            || !terminals || !workers) {
        fprintf(stderr, "pttpcc: out of memory\n") ;
        return 1 ;
    }
    for (int w = 0; w < nwarehouses; w++) {
        pt_lock_init(&warehouses[w].lock) ;
    }
    for (int i = 0; i < nterminals; i++) {
        pt_lock_init(&districts[i].lock) ;
        districts[i].next_o_id = 1 ;
    }

    protothread_init(&pt) ;
    pt_timers_init(&timers, 0) ;
    end_time = (pt_time_t)minutes * 60 * 1000 ;
    /* a terminal has at most one request outstanding, so none ever fills */
    for (int s = 0; s < nshards; s++) {
        int const shard_terminals = SHARD_WAREHOUSES * DISTRICTS ;
        reqq_init(&shards[s], &slots[s * shard_terminals],
                  s < nshards - 1 ? shard_terminals
                                  : nterminals - s * shard_terminals) ;
    }
    for (int i = 0; i < nworkers; i++) {
        workers[i].requests = &shards[i / SHARD_WORKERS] ;
        pt_create(&pt, &workers[i].pt_thread, db_worker_thr, &workers[i]) ;
    }
    for (int i = 0; i < nterminals; i++) {
        terminals[i].w = i / DISTRICTS ;
        pt_create(&pt, &terminals[i].pt_thread, terminal_thr, &terminals[i]) ;
    }

    for (;;) {
        pt_time_t next ;
        while (protothread_run(&pt)) {
        }
        if (!pt_timer_next(&timers, &next)) {
            break ;     /* nothing sleeping, so the terminals have finished */
        }
        pt_timer_run(&pt, &timers, next) ;
    }
    for (int i = 0; i < nworkers; i++) {
        pt_kill(&workers[i].pt_thread) ;    /* idle in pt_queue_receive() */
    }

    printf("%d warehouses, %d terminals, %d database shards of %d workers, "
           "%d simulated minutes%s%s\n", nwarehouses, nterminals, nshards,
           SHARD_WORKERS, minutes, use_locks ? "" : ", NO LOCKS",
           herd ? ", herd start" : "") ;
    printf("\n%-14s %9s %9s %9s %9s\n",
           "transaction", "count", "mean ms", "p90 ms", "max ms") ;
    for (int t = 0; t < NTYPES; t++) {
        pt_time_t max = 0 ;
        for (pt_time_t ms = 0; ms <= HIST_MS; ms++) {
            if (histogram[t][ms]) {
                max = ms ;
            }
        }
        printf("%-14s %9lu %9.1f %9lu %8lu%s\n", type_name[t], tx_count[t],
               tx_count[t] ? (double)tx_total_ms[t] / tx_count[t] : 0.0,
               (unsigned long)percentile(t, 90), (unsigned long)max,
               max == HIST_MS ? "+" : "") ;
    }
    printf("\nNew-Orders per simulated minute: %.0f\n",
           (double)tx_count[NEW_ORDER] / minutes) ;
    printf("lock waits: %lu, peak requests queued on a shard: %u\n",
           lock_waits, peak_queued) ;

    struct rusage ru ;
    getrusage(RUSAGE_SELF, &ru) ;
#ifdef __APPLE__
    long const rss_kib = ru.ru_maxrss / 1024 ;      /* bytes on macOS */
#else
    long const rss_kib = ru.ru_maxrss ;             /* KiB on Linux */
#endif
    printf("terminal context: %zu bytes; CPU time %.2f s; peak RSS %.1f MiB\n",
           sizeof(terminal_context_t),
           (double)(clock() - cpu_start) / CLOCKS_PER_SEC, rss_kib / 1024.0) ;

    int const rc = check_consistency(nwarehouses) ;
    free(workers) ;
    free(terminals) ;
    free(slots) ;
    free(shards) ;
    free(paid) ;
    free(districts) ;
    free(warehouses) ;
    return rc ;
}
