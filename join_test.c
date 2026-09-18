#include <stdio.h>
#include <string.h>
#include "protothread.h"

static char trace[64] ;
static int trace_n ;
static int chan ;
static int failures ;
#define T(ch) (trace[trace_n++] = (char)(ch))

typedef struct { pt_thread_t pt_thread ; pt_func_t pt_func ; int n ; } worker_t ;
typedef struct { pt_thread_t pt_thread ; pt_func_t pt_func ; worker_t *target ; char id ; } joiner_t ;

static pt_t
worker(env_t const env)
{
    worker_t * const c = env ;
    pt_resume(c) ;
    for (c->n = 0; c->n < 3; c->n++) {
        pt_yield(c) ;
    }
    T('w') ;
    return PT_DONE ;
}

static pt_t
blocker(env_t const env)
{
    worker_t * const c = env ;
    pt_resume(c) ;
    pt_wait(c, &chan) ;
    return PT_DONE ;
}

static pt_t
joiner(env_t const env)
{
    joiner_t * const c = env ;
    pt_resume(c) ;
    pt_join(c, &c->target->pt_thread) ;
    T(c->id) ;
    return PT_DONE ;
}

/* bounded, so a join that never returns fails the test instead of hanging */
static void
drain(protothread_t pt)
{
    int i ;
    for (i = 0; i < 10000 && protothread_run(pt); i++) {
    }
}

static void
expect(char const *name, char const *want)
{
    trace[trace_n] = '\0' ;
    if (strcmp(trace, want) == 0) {
        printf("  %-40s ok   (%s)\n", name, trace) ;
    } else {
        printf("  %-40s FAIL got \"%s\", wanted \"%s\"\n", name, trace, want) ;
        failures++ ;
    }
    trace_n = 0 ;
}

int
main(void)
{
    struct protothread_s s ;
    static worker_t w ;
    static joiner_t j1, j2 ;

    /* 1. the joiner blocks and is released when the worker exits */
    protothread_init(&s) ;
    pt_create(&s, &w.pt_thread, worker, &w) ;
    j1.target = &w ; j1.id = '1' ;
    pt_create(&s, &j1.pt_thread, joiner, &j1) ;
    drain(&s) ;
    expect("join a running protothread", "w1") ;

    /* 2. the worker has already exited before the join is even created */
    protothread_init(&s) ;
    pt_create(&s, &w.pt_thread, worker, &w) ;
    drain(&s) ;
    j1.target = &w ; j1.id = '1' ;
    pt_create(&s, &j1.pt_thread, joiner, &j1) ;
    drain(&s) ;
    expect("join one that already exited", "w1") ;

    /* 3. two joiners on one target */
    protothread_init(&s) ;
    pt_create(&s, &w.pt_thread, worker, &w) ;
    j1.target = &w ; j1.id = '1' ;
    j2.target = &w ; j2.id = '2' ;
    pt_create(&s, &j1.pt_thread, joiner, &j1) ;
    pt_create(&s, &j2.pt_thread, joiner, &j2) ;
    drain(&s) ;
    expect("two joiners", "w12") ;

    /* 4. the target is killed rather than exiting */
    protothread_init(&s) ;
    pt_create(&s, &w.pt_thread, blocker, &w) ;
    j1.target = &w ; j1.id = '1' ;
    pt_create(&s, &j1.pt_thread, joiner, &j1) ;
    drain(&s) ;                       /* both block */
    pt_kill(&w.pt_thread) ;
    drain(&s) ;
    expect("join a killed protothread", "1") ;

    printf("  failures=%d\n", failures) ;
    return failures != 0 ;
}
