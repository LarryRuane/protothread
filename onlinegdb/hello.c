/* Protothreads 2.1.0, https://github.com/LarryRuane/protothread
 *
 * protothread.h is pasted in first below. The example is at the bottom,
 * after "END OF protothread.h". Press Ctrl-End to get there.
 *
 * To step through it, set a breakpoint on the first line of main(), the call
 * to protothread_create(). The easiest way to set a breakpoint is to click
 * to the left of the line number. Click Debug at the top, then type "r" at
 * the (gdb) prompt to run to the breakpoint. From there, type "s" (step
 * into), and then press Enter to continue stepping. Stepping into everything
 * shows how little is needed to create a protothread, schedule it, and
 * run it. Imagine single-stepping through a POSIX pthread_create() and a
 * context switch!
 *
 * Setting PT_NWAIT to 2 just below makes stepping through protothread_init()
 * faster.
 *
 * The Local Variables panel leaves out function arguments. To see them,
 * type "info args" at the (gdb) prompt, or add one (such as *t) under
 * Display Expressions.
 *
 * Debug sessions here time out after a few minutes. Just click Debug again,
 * since every run takes the same path (breakpoints are preserved).
 */
#define PT_DEBUG 0
#define PT_NWAIT 2

#include <stdio.h>
#include "protothread.h"

typedef struct {
    pt_thread_t pt_thread;
    pt_func_t pt_func;
    char const * message;
} hello_context_t;

static pt_t
hello_thr(void * const env)
{
    hello_context_t * const c = env;
    pt_resume(c);

    printf("%s\n", c->message);
    return PT_DONE;
}

int main()
{
    protothread_t const pt = protothread_create();
    hello_context_t c;

    c.message = "Hello from the protothread!";
    pt_create(pt, &c.pt_thread, hello_thr, &c);

    /* run protothreads until none is ready to run */
    while (protothread_run(pt)) {
    }

    printf("Protothread has finished executing.\n");
    protothread_free(pt);
    return 0;
}
