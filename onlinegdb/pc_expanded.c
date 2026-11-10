/* Protothreads 2.1.0, https://github.com/LarryRuane/protothread
 *
 * protothread.h is pasted in first below. The example, the README's
 * producer/consumer with a count of 3, is at the bottom, after "END OF
 * protothread.h". Press Ctrl-End to get there.
 *
 * Here pt_resume() and pt_wait() are expanded by hand, so the debugger can
 * step through them. Each expansion is marked with the macro it replaces.
 *
 * To step through it, first click "Fork this" at the top (no account is
 * needed), which brings up the Debug button. Then set a breakpoint on the first
 * line of main(), the call to protothread_create(). The easiest way to set a
 * breakpoint is to click to the left of the line number. Click Debug at the
 * top, then type "r" at the (gdb) prompt to run to the breakpoint. From there,
 * type "s" (step into), and then press Enter to continue stepping.
 *
 * Watch "return pt_i_return_wait()" go back to the scheduler, the other
 * protothread run, and then the goto in pt_resume() jump back into the
 * middle of the while loop.
 *
 * Setting PT_NWAIT to 2 just below makes stepping through protothread_init()
 * faster, and PT_DEBUG 0 leaves out bookkeeping that only gdb macros use.
 *
 * The Local Variables panel leaves out function arguments. To see them,
 * type "info args" at the (gdb) prompt, or add one (such as *t) under
 * Display Expressions.
 *
 * Debug sessions here time out after a few minutes. Just click Debug again,
 * since every run takes the same path.
 */
#define PT_NWAIT 2
#define PT_DEBUG 0
#include "protothread.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define COUNT 3

typedef struct {
    pt_thread_t pt_thread;
    pt_func_t pt_func;
    int i;
    int * mailbox;
} pc_thread_context_t;

static pt_t
producer_thr(void * const env)
{
    pc_thread_context_t * const c = env;

    /* pt_resume(c): jump back to where this function last waited */
    if (c->pt_func.label) goto *c->pt_func.label;

    for (c->i = 1; c->i <= COUNT; c->i++) {
        while (*c->mailbox) {
            /* mailbox is full */

            /* pt_wait(c, c->mailbox): remember where to resume, join the
             * mailbox's wait list, and return to the scheduler
             */
            c->pt_func.label = &&resume_full;
            pt_i_enqueue_wait(c->pt_func.thread, c->mailbox);
            return pt_i_return_wait();
          resume_full:;
        }
        *c->mailbox = c->i;
        pt_broadcast(pt_i_get_protothread(&c->pt_func), c->mailbox);
    }
    return pt_i_return_done();   /* PT_DONE */
}

static pt_t
consumer_thr(void * const env)
{
    pc_thread_context_t * const c = env;

    /* pt_resume(c): jump back to where this function last waited */
    if (c->pt_func.label) goto *c->pt_func.label;

    for (c->i = 1; c->i <= COUNT; c->i++) {
        while (*c->mailbox == 0) {
            /* mailbox is empty */

            /* pt_wait(c, c->mailbox): remember where to resume, join the
             * mailbox's wait list, and return to the scheduler
             */
            c->pt_func.label = &&resume_empty;
            pt_i_enqueue_wait(c->pt_func.thread, c->mailbox);
            return pt_i_return_wait();
          resume_empty:;
        }
        assert(*c->mailbox == c->i);
        printf("consumer got %d\n", *c->mailbox);
        *c->mailbox = 0;
        pt_broadcast(pt_i_get_protothread(&c->pt_func), c->mailbox);
    }
    return pt_i_return_done();   /* PT_DONE */
}

int
main(void)
{
    protothread_t const pt = protothread_create();
    pc_thread_context_t * const cc = malloc(sizeof(*cc));
    pc_thread_context_t * const pc = malloc(sizeof(*pc));
    int mailbox = 0;

    /* set up consumer context, start consumer thread */
    cc->mailbox = &mailbox;
    cc->i = 0;
    pt_create(pt, &cc->pt_thread, consumer_thr, cc);

    /* set up producer context, start producer thread */
    pc->mailbox = &mailbox;
    pc->i = 0;
    pt_create(pt, &pc->pt_thread, producer_thr, pc);

    /* while threads are available to run ... */
    while (protothread_run(pt)) {
    }

    /* threads have completed */
    assert(cc->i == COUNT + 1);
    assert(pc->i == COUNT + 1);

    free(cc);
    free(pc);
    protothread_free(pt);
    return 0;
}
