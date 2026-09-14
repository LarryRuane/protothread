## Introduction ##

[![CI](https://github.com/LarryRuane/protothread/actions/workflows/ci.yml/badge.svg)](https://github.com/LarryRuane/protothread/actions/workflows/ci.yml)


[Protothreads](http://en.wikipedia.org/wiki/Protothreads) is a programming model invented by Adam Dunkels that combines the advantages of _event-driven_ (sometimes also called _state machine_) programming and _threaded_ programming. The main advantage of the event-driven model is efficiency, both speed and memory usage. The main advantage of the threaded model is [algorithm clarity](http://dunkels.com/adam/dunkels06protothreads.pdf). Protothreads gives you both. A protothread is an extremely lightweight thread. As with event-driven programming, there is a single stack; but like threaded programming, a function can (at least conceptually) block. This protothreads implementation:
  * is not an implementation of POSIX threads or any other standard API
  * does not require assembly-language code or use setjmp/longjmp
  * is independent of CPU architecture
  * schedules threads non-preemptibly and deterministically.

### Why another protothreads implementation? ###

I wrote this from scratch, and it is not compatible with other versions of protothreads. I came to it from a Unix kernel background, and while Dunkels' idea is brilliant, I wanted a programming interface that felt more like the threaded code I was used to reading and writing. The differences that matter in daily use:

  * **Blocking functions can nest.** A protothread function can `pt_call()` another protothread function, which can block, to any depth. The common implementations give you a single flat function per protothread; here a protothread is a genuine call chain, so you can factor blocking code into subroutines the way you would anywhere else.
  * **You can block anywhere**, including inside a `switch` statement. Implementations built on Duff's device cannot, because they have already spent the `switch`.
  * **A real scheduler, with wait channels.** `pt_wait()`/`pt_signal()`/`pt_broadcast()` on an arbitrary address deliberately mirror condition variables (`pthread_cond_wait()`, except a `pthread_cond_t` variable isn't needed) and the classic Unix kernel `sleep()`/`wakeup()`. Threads live on a run list or a wait list; you are not hand-rolling dispatch.
  * **Semaphores and reader-writer locks** are built on top of that, so the familiar synchronization vocabulary is available.

The cost of nesting and arbitrary blocking is that this implementation uses [gcc label variables](http://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html), which is the one part of it that is not standard C, so it requires **gcc or clang** (see [Compiler requirements](#compiler-requirements)). Dunkels' `switch`-based version is portable to any C compiler; this one trades that for a better interface.

### What you get ###

  * **Header-only.** Copy the headers into your project. Nothing to build, nothing to link, no submodules, no dependencies.
  * **Usable from C++.** The headers compile as C++ too, though protothread functions need an explicit cast from `env_t`; see [Using it from C++](#using-it-from-c).
  * **Runs with no C library at all.** A production build needs zero libc symbols and compiles `-ffreestanding`, so it works on bare metal. See [Bare-metal and embedded use](#bare-metal-and-embedded-use).
  * **Tiny.** 32 bytes of RAM per protothread and about 700 bytes of code on a 32-bit MCU.
  * **Fast.** Against POSIX threads doing the same work, measured by the [benchmark](#benchmarks) included in this repository:

| | protothread | pthread | ratio |
|---|---|---|---|
| context switch | 4.6 ns | 3,335 ns | **720x** |
| create + destroy | 2.7 ns | 29,259 ns | **10,800x** |
| memory per thread | 64 bytes | 16,384 bytes | **256x** |

> Protothreads are faster here because they do less: no kernel transition, no scheduler, no stack. POSIX threads buy preemption and real parallelism, which protothreads do not provide -- see [Memory overhead and performance](#memory-overhead-and-performance-benchmarks) for what the comparison does and does not mean, and [Protothreads on a multi-core system](#protothreads-on-a-multi-core-system) for using both together.
  * two synchronization facilities built on top of the base protothreads (semaphores and locks)
  * about 1000 lines of test code
  * gdb (debugger) macros to print the stack traces of a given protothread or all protothreads.
  * a cmake find script (FindPROTOTHREAD.cmake)

### Using it ###

There is nothing to build. Copy `protothread.h` (and `protothread_sem.h` and `protothread_lock.h` if you want semaphores or locks) into your project and include them:

```c
#include "protothread.h"
```

To build and run the test suite:

```
cmake -S . -B build && cmake --build build && ./build/pttest
```

### The whole API ###

This is all of it. Everything else in the headers is internal and carries a `pt_i_` or `PT_I_` prefix to say so, so anything *without* one of those prefixes is API you can rely on, and anything with one may change in any release.

**The scheduler** (`protothread.h`)

| call | what it does |
|---|---|
| `protothread_init(s)` | initialize a `protothread_t` you allocated yourself |
| `protothread_deinit(s)` | check nothing is still scheduled (`PT_DEBUG` builds) |
| `protothread_create()` | allocate and initialize one (needs `malloc`) |
| `protothread_free(s)` | deinitialize and free it |
| `protothread_run(s)` | run one ready protothread; true if more remain |
| `protothread_set_ready_function(s, f, env)` | called when the run list becomes non-empty |
| `pt_create(s, thread, func, env)` | create a protothread and make it ready |
| `pt_signal(s, channel)` | make the oldest waiter on `channel` ready |
| `pt_broadcast(s, channel)` | make every waiter on `channel` ready |
| `pt_kill(thread)` | unschedule one; true if it was still scheduled |
| `pt_set_atexit(thread, func)` | destructor to run at the end of `pt_kill()` |

**Inside a protothread function**, where `c` is the context. All of these are macros.

| call | what it does |
|---|---|
| `pt_resume(c)` | first statement of every protothread function |
| `pt_wait(c, channel)` | block until `channel` is signalled |
| `pt_yield(c)` | let other ready protothreads run, then continue |
| `pt_call(c, func, child_c, ...)` | call a protothread function that may block |
| `pt_call_waited(c)` | did that `pt_call()` block? |
| `pt_reset(c)` | forget the resume point; start again from the top |
| `pt_get_pt(c)` | the `protothread_t` this protothread belongs to |
| `PT_DONE` | what a protothread function returns when it is finished |

**Semaphores** (`protothread_sem.h`) and **reader-writer locks** (`protothread_lock.h`)

| call | what it does |
|---|---|
| `pt_sem_acquire(c, sem_env, value)` | block until the count is non-zero, then take one |
| `pt_sem_release(sem_env, value)` | give one back; never blocks |
| `pt_lock_init(lock)` | initialize an unheld lock |
| `pt_lock_acquire_read(c, lock_env, lock)` | block until read access is granted |
| `pt_lock_acquire_write(c, lock_env, lock)` | block until exclusive access is granted |
| `pt_lock_release_read(lock_env, lock)` | release; never blocks |
| `pt_lock_release_write(lock_env, lock)` | release; never blocks |

**Timers** (`protothread_timer.h`). This library never reads a clock; you drive it.

| call | what it does |
|---|---|
| `pt_timers_init(timers, now)` | initialize a timer set |
| `pt_sleep(c, timer_env, timers, ticks)` | block for `ticks` of your own clock |
| `pt_timer_run(s, timers, now)` | wake everything now due; call this on a tick |
| `pt_timer_cancel(timers, timer_env)` | wake a sleeper early; true if it was pending |
| `pt_timer_next(timers, deadline)` | soonest deadline, for an idle loop |
| `pt_time_after(a, b)` | wraparound-safe time comparison |

Types: `protothread_t`, `pt_thread_t`, `pt_func_t`, `pt_t`, `pt_f_t`, `env_t`, `bool_t`, `pt_sem_env_t`, `pt_lock_t`, `pt_lock_env_t`, `pt_timers_t`, `pt_timer_env_t`, `pt_time_t`. The compile-time knobs are under [Configuration](#configuration).

## Threads without stacks ##

The key concept of any protothreads implementation is that when a function wants to wait for an event to occur (that is, suspend itself and let other threads run), it saves its current location within the function (conceptually its line number or program counter), and returns back to the scheduler or idle loop, releasing use of the stack. The scheduler runs a different thread, handles interrupts or waits for an external event to occur. When the event occurs, the scheduler calls the function in the usual way, and the first thing the function does is `goto` the previously saved location. This location might be within levels of nested loops and `if` statements.

The `return` and `goto` statements are hidden within macros, so the application code looks very much like regular threaded code. This implementation uses a little-known gcc feature that lets you store the address of a `goto` label in a variable, and then later `goto` that variable (from within the same function), even if the function has returned and is now being called again. The performance is about the same as event-drive software; a context switch is just a few simple C statements.

You can think of protothreads as a generalization of event-driven programming. An event-driven work item (request in progress) typically consists of a pointer to a function (or a _state_ variable when using a big `switch` statement instead of individual functions) and a _context_ structure. These encapsulate the current state of the work item. A protothread has not just a function pointer (and context), but also a location _within_ the function. That location encodes a more fine-grain form of state than a simple function address. Actually, a protothread is even more general; if function **A** calls function **B**, **B** calls **C** and **C** blocks, then the thread now has a set of _three_ pointers into the middle of those functions.

## Example - Producer / Consumer ##

Here is a protothreads version of the famous producer-consumer algorithm with two threads and a single shared integer mailbox. This is just to show the basic idea; details are explained later. All protothreads functions and types begin with `pt_` or (for "system level" entities) `protothread_`. Each thread needs a context structure:
```
 typedef struct {
     pt_thread_t pt_thread;
     pt_func_t pt_func;
     int i;
     int * mailbox;
 } pc_thread_context_t;
```
Besides the first two fields, which are used by the protothreads system, the structure contains a counting index, `i`, and a pointer the mailbox that the threads will share. For the producer, the `i` is the next value to write to the mailbox; for the consumer, it's the next value to expect from the mailbox. A value of zero in the mailbox means it is empty.

Example 2 shows producer and consumer threads:
```
 static pt_t
 producer_thr(void * const env)
 {
     pc_thread_context_t * const c = env;
     pt_resume(c);
 
     for (c->i = 1; c->i <= 100; c->i++) {
         while (*c->mailbox) {
             /* mailbox is full */
             pt_wait(c, c->mailbox);
         }
         *c->mailbox = c->i;
         pt_signal(pt_get_pt(c), c->mailbox);
     }
     return PT_DONE;
 }
 
 static pt_t
 consumer_thr(void * const env)
 {
     pc_thread_context_t * const c = env;
     pt_resume(c);
 
     for (c->i = 1; c->i <= 100; c->i++) {
         while (*c->mailbox == 0) {
             /* mailbox is empty */
             pt_wait(c, c->mailbox);
         }
         assert(*c->mailbox == c->i);
         *c->mailbox = 0;
         pt_signal(pt_get_pt(c), c->mailbox);
     }
     return PT_DONE;
 }
```
The producer thread waits until the mailbox is empty, then writes the next value to the mailbox and signals the consumer. The consumer thread waits until something appears in the mailbox, verifies that it's the expected value, writes a zero to signify that the mailbox is empty, and wakes up the producer. The threads signal each other using the address of the mailbox as the _channel_. It's common to use the address of the data structure whose state changes are of possible interest to waiting threads as the channel. In its role as a channel, the address is never dereferenced; it is strictly used to match signals with waits.

This technique of thread synchronization (and the term "channel") was first used in the UNIX kernel. The concept is similar to the _condition variable_ in POSIX threads -- there is no "memory" associated with the channel or a POSIX condition variable; signaling a channel or condition variable when no thread is waiting has no effect. The reason I choose to implement the channel approach is that it's simpler to use because it's not necessary to allocate condition variables. The API also includes `pt_broadcast()`, which is similar to `pt_signal()` except that it wakes up all threads waiting on the given channel, not just the longest-waiting thread.

The main test function allocates the overall protothread object or instance (`pt`) and a context for each thread, initializes the mailbox to empty, creates the threads, and runs the protothread system until there is no more work to do:
```
 static void
 test_pc(void)
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
     assert(cc->i == 101);
     assert(pc->i == 101);
 
     free(cc);
     free(pc);
     protothread_free(pt);
 }
```
## How does it work? ##

Now for the details. The two most interesting calls in this example are `pt_resume()` and `pt_wait()`. Let's expand these in the producer thread function to see how they work. (The full API reference manual is given at the end of this article.) The `pt_func` member of the context structure contains the protothreads-private _function context_; the protothread macros access this field by name.
```
 static pt_t
 producer_thr(void * const env)
 {
     pc_thread_context_t * const c = env;
 
     /* pt_resume(c) expanded: *****/
     if ((c)->pt_func.label) goto *(c)->pt_func.label;
     /* pt_resume end *****/
 
     for (c->i = 1; c->i <= 100; c->i++) {
         while (*c->mailbox) {
             /* mailbox is full */
 
             /* pt_wait(c, c->mailbox) expanded: *****/
             do {
                 (c)->pt_func.label = &&pt_label_18;
                 pt_i_enqueue_wait((c)->pt_func.thread, c->mailbox);
                 return PT_I_WAIT;
               pt_label_18:;
             } while (0);
             /* pt_wait end *****/
        }
        *c->mailbox = c->i;
        pt_signal(pt_get_pt(c), c->mailbox);
     }
     return PT_DONE;
 }
```
The first time the thread runs, its label variable is `NULL`, so it does not `goto` -- the code enters the `for` loop from the top. When it reaches the call to `pt_wait()`, it saves the address of the label (whose name is derived from the line number, `__LINE__`; note the double-ampersand syntax to denote the address corresponding to a label), enqueues the thread on a _waiting_ list within the protothreads object (and it's going to wait for a signal on the address of the mailbox), and returns `PT_I_WAIT`. (The return value is not used in this example; as explained later it is used only if there are nested protothread functions.)

When this thread is resumed, the label variable is non-NULL, so `pt_resume()` jumps to the value of the label variable, and execution continues from where it left off. In this case, the producer thread continues in the `while` loop, waiting for the mailbox to become empty. As when using POSIX condition variables, it's common to re-test the condition being waited for.

### Structure of a protothread ###

A protothread comprises one or more _protothread functions._ Only a protothread function can block or call a function that blocks. The overall protothread needs a `pt_thread_t` structure, which contains the protothread-private context for the thread. Furthermore, _each level_ of nested blocking function needs a user-defined context structure, and this structure must contain a `pt_func` member of type `pt_func_t`, which contains the protothread-private context for this function. If **A** calls **B** which in turn calls **C** (and **C** can block), each needs its own instance of a structure containing at least a `pt_func` member.

This structure also contains user-defined state that is specific to that function, usually what correspond to local variables when using POSIX threads. Protothread functions generally cannot use local variables, because their values are not preserved across waits. (You may have noticed the example uses `c->i` instead of `i` for the loop index.) There is one narrow exception, and there are two ways to get this wrong; see [Local variables](#local-variables) below.

There are only two ways to run a protothread function; a protothread function should never be called directly.

  * Any code (a regular function or a protothread function) can call `pt_create()` to create a new thread. You specify a function address and a context pointer which is passed to the function as its only argument. This call schedules the thread (does not run it directly). A scheduled thread can be cancelled with `pt_kill()`, but only if it was written to expect that. The protothread system does not notify you when a thread exits normally; that's up to you to arrange if you need to know. The `pt_create()` call also requires a unique (to this thread) `pt_thread_t` structure, which can be allocated anywhere, but is typically included within the top-level function's context structure (as in the structure `pc_thread_context_t` above).
  * A protothread function can execute `pt_call()`. This has the same semantics as a normal function call, but you must use `pt_call()` when calling a protothread function. Any number of arguments of any types may be passed to the called function (and the usual compiler type checking applies), but the first argument must be a pointer to a context structure (which contains a `pt_func` member) for the called function to use to hold its state.

Any return statements you write must return `PT_DONE`; the return value belongs to the protothreads system, which uses it to propagate blocking up the call chain. That is less of a restriction than it looks, because the caller owns the callee's context structure, which makes a better return channel than a return value would be: it carries any number of values of any types, and unlike a return value it survives blocking. Declare the results alongside the arguments:
```
 typedef struct {
     pt_func_t pt_func;
     int n;               /* in:  number of records to read */
     int count;           /* out: number actually read */
     int error;           /* out: zero, or an errno-style code */
 } read_context_t;

 static pt_t
 read_thr(env_t const env)
 {
     read_context_t * const c = env;
     pt_resume(c);

     c->error = 0;
     for (c->count = 0; c->count < c->n; c->count++) {
         pt_wait(c, &device_status);    /* may block any number of times */
         if (device_status != 0) {
             c->error = device_status;  /* give up on a short read */
             return PT_DONE;
         }
     }
     return PT_DONE;
 }
```
The caller embeds that context within its own, so it can read the results as soon as `pt_call()` returns:
```
 typedef struct {
     pt_thread_t pt_thread;
     pt_func_t pt_func;
     read_context_t read;     /* the callee's context lives here */
 } caller_context_t;

     c->read.n = 100;
     pt_call(c, read_thr, &c->read);
     if (c->read.error) {
         /* c->read.count says how many arrived before it failed */
     }
```
This costs nothing: the caller already allocates the callee's context, so there is no copying and no additional storage. When the top-level protothread function returns, the thread has exited, and control returns to the scheduler.

### Local variables ###

A protothread function releases the stack every time it blocks, so **a local variable does not keep its value across `pt_yield()`, `pt_wait()` or `pt_call()`.** Anything that must survive a block belongs in the context structure. This is the most common mistake when writing protothreads, and it appears in two forms: one the compiler catches, one it does not.

**Declared after `pt_resume()` -- the compiler catches this.** On resume, `pt_resume()` jumps straight to the resume point, skipping the declaration and its initializer:
```
 static pt_t
 example_thr(env_t const env)
 {
     context_t * const c = env;
     pt_resume(c);
     int x = 1;              /* skipped when the thread resumes */

     pt_yield(c);
     c->result = x;          /* x is undefined here */
     return PT_DONE;
 }
```
Compiling with `-Wall` catches it, pointing at both the use and the declaration -- gcc says `warning: 'x' may be used uninitialized [-Wmaybe-uninitialized]`, clang says `warning: variable 'x' is uninitialized when used here [-Wuninitialized]`.

This diagnostic used to require `-O2` or higher, because it depended on optimizer flow analysis. That is no longer the case: current compilers report it at every optimization level, `-O0` included. (Verified with gcc 15.2 and clang 20 and 22 at `-O0`, `-O1`, `-O2` and `-Os`.) You do need `-Wall` or `-Wextra` -- at the default warning level both compilers stay silent. The supplied `CMakeLists.txt` sets `-Wall`.

**Declared before `pt_resume()` -- the compiler does not catch this.** Here the declaration is not skipped; it re-runs on every entry, so the variable is silently reset each time the thread resumes:
```
     int x = 1;              /* re-initialized on every resume */
     pt_resume(c);

     x = 2;
     pt_yield(c);
     printf("x = %d\n", x);  /* prints 1, not 2 */
```
This compiles cleanly under `-Wall -Wextra` on both gcc and clang at every optimization level, which makes it the more dangerous of the two. If a local is assigned in one part of a protothread function and read after a block, move it into the context structure.

**The safe exception.** Initializing a local before `pt_resume()` is fine, and idiomatic, when its value is a pure function of the arguments and it is then used read-only -- re-running the initializer on each entry recomputes the same value. That is what the `c = env;` line does in every example here. For the same reason the initializer must have no side effects.

**Declare those locals `const`.** Read-only is not merely a convention here, it is a correctness requirement, so let the compiler enforce it:
```
     int const x = 1;        /* not just `int x = 1;` */
     pt_resume(c);

     x = 2;                  /* error: assignment of read-only variable 'x' */
```
That converts the silent failure above into a hard compile error, in both gcc and clang, at any optimization level and with no warning flags needed. For a pointer the qualifier goes after the `*`, as in `context_t * const c = env;` -- it is the pointer that must not change, not what it points at.

### Protothread function nesting ###

How does function nesting work? When protothread function **A** calls (using `pt_call()`) a protothread function **B**, and **B** wants to block (`pt_wait()`), **B** saves its current location into its context and returns `PT_I_WAIT` to the `pt_call()` in **A**, which causes it to save into **A**'s context as its resume point exactly where it calls **B**. **A** then returns `PT_I_WAIT` to its caller. When the scheduler resumes the thread, **A** runs, its `pt_resume()` jumps to the call to **B**, so **A** calls **B**, and **B**'s `pt_resume()` jumps to just after where it had blocked and continues running. So the stack unwinds when the thread blocks, and "forward-winds" when it resumes. This is how the overall system still uses a single stack. Also, it should be clear now why evaluating the arguments that **A** passes to **B** should have no side effects -- **A** calls **B** every time the thread is resumed.

When **B** finally finishes and returns `PT_DONE`, **A** knows to continue running following the `pt_call()` to **B**.

The context for function **A** can include **B**'s context structure within its own, or it can dynamically allocate **B**'s context; allocation of contexts is up to the user. To provide greater data hiding, **A** can allocate a very small structure with just the required `pt_func` field and a pointer to an opaque (to **A**) **B** context. The first thing **B** does is dynamically allocate its full context structure and link it from the small context that **A** passed.

Another interesting idea is that if **A** calls **B** and after **B** returns **A** calls **C** (so **B** and **C** are not running at the same time), the contexts for **B** and **C** can be members of a `union` within **A**'s context. This sharing of memory between **B** and **C** reflects what happens within the stack of a POSIX thread.

## Deterministic execution ##

An important advantage of event-driven software over POSIX threads is that execution can be entirely deterministic. Protothreads shares this advantage. Why does this matter? Because it allows one to write pseudo-random tests that can reliably reproduce bugs. You start the test with a randomly-chosen random number generator seed, and if a bug is found during the run, you can start the test again with the same seed (perhaps with more tracing enabled or new assertions added to catch the problem earlier), and the test is guaranteed to follow exactly the same sequence of states and thus reproduce the bug. This also often allows you to verify a proposed fix (unless the fix changes the execution sequence in a way that invalidates the seed).

Of course, the entire environment must be carefully controlled so that no nondeterminism can sneak into the system. It may be necessary, for example, to simulate time; the code should not make any decisions based on real (wall-clock) time, because real time will differ from run to run. The random number generator should be used for anything that is non-deterministic in the real system (such as delays in simulated network or disk transfers).

With POSIX threads this is much harder to arrange, because the thread scheduler is outside the test program's control and in my experience makes decisions that vary from run to run. It is not impossible, though: determinism can be imposed from the outside, either by a replacement runtime such as [DThreads](https://plasma.cs.umass.edu/emery/dthreads.html) or [Parrot](https://sigops.org/s/conferences/sosp/2013/papers/p388-cui.pdf), which serialize thread synchronization into a fixed order, or by a supervising tool such as [rr](https://rr-project.org/) (record and deterministic replay) or [Hermit](https://github.com/facebookexperimental/hermit) (a deterministic sandbox that also controls time, thread interleaving and randomness). These work, and rr in particular is worth knowing about for any concurrent C program. But each buys determinism with an external mechanism and a slowdown, and the deterministic run is not the run you ship. Protothreads are deterministic by construction, with no tooling at all, and the schedule you debug is the schedule that runs in production.

## Protothreads on a multi-core system ##

All protothreads in one `protothread_t` run on a single thread, one at a time. That does not confine the technique to single-core machines. It means protothreads are the wrong tool for parallel *computation* and the right tool for concurrent *control flow* -- and the two compose well.

The usual arrangement is a pool of POSIX threads doing the heavy lifting on every core, with protothreads as the coordination layer. A protothread submits a work item, blocks, and resumes when the result is ready; other protothreads run in the meantime. N cores stay busy on the computation while one thread runs thousands of protothreads that own the sequencing, retries, timeouts and state -- parallelism where it pays, and no locking discipline where it does not.

The one thing to get right is how a worker wakes a protothread: it must not call `pt_signal()` itself, for the reason given under [Lost wakeups](#lost-wakeups). It should post the finished work somewhere, and the thread that owns `protothread_run()` turns that into a `pt_signal()` between runs.

The [`demo/`](demo) directory has three complete working programs, one for each way of combining protothreads with the outside world:

| program | what it shows |
| --- | --- |
| [`demo/pool.c`](demo/pool.c) | 500 protothreads over a fixed pool of 4 worker threads -- the arrangement described above |
| [`demo/async_io.c`](demo/async_io.c) | concurrent asynchronous I/O and no POSIX threads at all: one `poll()` loop turning completions into signals. `./build/ptaio -v` traces every submission and completion, which come out thoroughly interleaved |
| [`demo/helper_thread.c`](demo/helper_thread.c) | one throwaway POSIX thread per blocking call, created and joined by the protothread that needs it |

They are not built by default, since two of the three need pthreads and the library itself does not:

```
cmake -S . -B build -DPROTOTHREAD_DEMOS=ON
cmake --build build
./build/ptpool && ./build/ptaio && ./build/pthelper
```

### Blocking system calls ###

A protothread that makes a blocking system call stops **every** protothread, because they all share one thread and one stack. `pt_wait()` is not a system call and does not do this; `read()`, `connect()` and `fsync()` do.

This is the constraint every event loop has, and it has the same two answers: use non-blocking I/O and `pt_wait()` on readiness ([`demo/async_io.c`](demo/async_io.c)), or hand the call to another thread ([`demo/pool.c`](demo/pool.c) for a standing pool, [`demo/helper_thread.c`](demo/helper_thread.c) for a thread created and joined around the one call).

Handing the call to a thread is really three arrangements, not one. A thread created for the call and joined when it finishes is the cheapest thing that works, and is right when the call blocks, has no asynchronous form, and happens rarely -- no pool, no work queue, no shutdown protocol. A pool is right when the offloaded work is constant and hot, or when there are far more protothreads than you would want threads. Between them sits a dedicated helper thread per protothread, created at startup and parked between calls: it costs about 8 kB of resident memory per parked helper against 64 bytes for the protothread itself, and in exchange no protothread ever queues behind another waiting for a free worker, and each helper can hold state across calls -- a connection, an open descriptor, a thread-bound library handle that a stateless pool worker cannot keep. That last point makes it a requirement, not an optimization, for some synchronous libraries.

In all three the protothreads themselves still share one thread, which is what keeps switching between them a computed goto. Giving each protothread a thread to *run on* is the thing that does not work: that is POSIX threads with extra steps.

But it is worth saying plainly that blocking is often *fine*. If the call is rare and short -- reading a configuration file at startup, an occasional log flush -- the cost is that other protothreads wait a few milliseconds, and building a thread pool to avoid it is a bad trade. What to avoid is a blocking call on a hot path, where it quietly converts a system that handles thousands of concurrent activities into one that handles them one at a time. Measure before engineering around it.

## Memory overhead and performance (benchmarks) ##

The best known implementation of protothreads (by Adam Dunkels) uses just two bytes per protothread. This implementation is not quite so parsimonious, mainly because it includes a scheduler: threads are on either the wait or the run list, and that costs pointers. In exchange you get nesting, wait channels and synchronization primitives.

Each protothread function context has a `pt_func_t` structure, which is 2 pointers. Each overall protothread requires a `pt_thread_t` structure, which is 6 pointers. Measured with `PT_DEBUG=0`:

| | 32-bit | 64-bit |
|---|---|---|
| `pt_func_t` (per nesting level) | 8 | 16 |
| `pt_thread_t` (per protothread) | 24 | 48 |
| **minimum RAM per protothread** | **32** | **64** |
| `protothread_t` state, `PT_NWAIT=1` | 20 | 40 |
| `protothread_t` state, default `PT_NWAIT` | 4112 | 8224 |
| `pt_lock_t` | 12 | 16 |

Add roughly 8 bytes of real C stack per level of `pt_call()` nesting: a protothread is stackless between waits, but while it is running, a chain of `pt_call()`s is an ordinary chain of C calls.

For comparison, an RTOS task typically costs a control block of about 90 bytes plus a stack of at least several hundred bytes, so a protothread is on the order of twenty times cheaper.

Note that `PT_DEBUG` adds four fields to `pt_func_t` and one to `pt_thread_t` for the debugger macros, roughly tripling the per-thread cost. It is on by default; turn it off in production builds.

### Benchmarks ###

`protothread_bench.c` is a benchmark that runs the same workloads twice, once with protothreads and once with POSIX threads, so you can reproduce these numbers on your own hardware rather than taking them on faith. It is not built by default, because unlike the library it needs pthreads:

```
cmake -S . -B build -DPROTOTHREAD_BENCH=ON
cmake --build build
./build/ptbench
```

On the machine this was written on:

| | protothread | pthread | ratio |
|---|---|---|---|
| context switch | 4.6 ns | 3,335 ns | **720x** |
| create + destroy | 2.7 ns | 29,259 ns | **10,800x** |
| memory per thread | 64 bytes | 16,384 bytes | **256x** |

The context switch benchmark is two threads handing a token back and forth a million times -- `pt_wait`/`pt_signal` on one side, a mutex and one condition variable per thread on the other. Creation is a thread that does nothing, created and reaped. Both are checked for linear scaling across two orders of magnitude, so the compiler is demonstrably not optimizing the work away.

Read these as orders of magnitude, not as digits: the absolute numbers move with the machine, the kernel, and its speculative-execution mitigations, and the pthread side is the part that moves most.

**This is not an apples-to-apples comparison, and it should not be read as one.** POSIX threads give you preemption and real parallelism across cores; protothreads give you neither. What the benchmark measures is the cost of the mechanism itself -- what you pay, per operation, for the ability to write code that blocks. Protothreads win by these margins because they do enormously less: no kernel transition, no scheduler, no stack. Where that trade is a good one -- a state machine per connection, an event loop, an embedded system with no MMU -- the ratios above are the reason to care. Where you need to keep four cores busy, they are beside the point.

## Versioning ##

This project uses [semantic versioning](https://semver.org). The version lives in `protothread.h`, which is the single source of truth -- the build reads it from there -- so a header copied into your own tree can always identify itself:

```c
#define PT_VERSION_MAJOR 2
#define PT_VERSION_MINOR 0
#define PT_VERSION_PATCH 0
#define PT_VERSION_NUMBER   /* ordered: 20000 */
#define PT_VERSION_STRING   /* "2.0.0", derived from the numbers */
#define PT_VERSION_AT_LEAST(major, minor, patch)
```

So a vendored copy can be checked at compile time:

```c
#if !defined(PT_VERSION_NUMBER) || !PT_VERSION_AT_LEAST(2, 0, 0)
#error protothread 2.0.0 or later is required
#endif
```

### Upgrading from 1.x ###

Version 2 is **not** a drop-in replacement. The API you write against is essentially unchanged, but the packaging is not:

  * **There is no library to link any more.** `protothread_sem.c` and `protothread_lock.c` are gone; their contents moved into the matching headers. The `protothread-static` and `protothread-shared` CMake targets are gone, and pkg-config no longer emits `-lprotothread`. Delete those from your build; include the headers and you are done.
  * **The headers no longer include `<stdlib.h>`, `<string.h>` or `<assert.h>`.** If your code relied on getting `malloc`, `memset` or `assert` transitively from `protothread.h`, include them yourself. This is what buys the freestanding property.
  * **`pt_set_atexit()` must now be called after `pt_create()`**, which clears the handler. Previously the field was left uninitialized, so calling it beforehand happened to work.
  * **Reader-writer locks are now FIFO.** Requests are granted in arrival order, so a stream of readers can no longer starve a waiting writer. If you somehow depended on the old LIFO order, you did not want it.
  * **The license changed from Apache-2.0 to MIT.**

Everything else -- `pt_wait`, `pt_yield`, `pt_call`, `pt_create`, `pt_signal`, `pt_broadcast`, the context structure layout, all of it -- is unchanged.

## Configuration ##

All configuration is by preprocessor macro. Because the library is header-only, define these on the compiler command line (`-DPT_DEBUG=0`) so that every translation unit agrees.

`PT_DEBUG` (default `1`)
> Enables internal assertions and the bookkeeping the gdb macros use to print protothread stack traces. Set to `0` for production builds. **This changes the layout of `pt_thread_t` and `pt_func_t`**, so it must be the same for your whole program.

`PT_NWAIT` (default `1024`)
> Number of wait queues, a power of 2. Waiting threads are hashed onto this table by channel address. Each entry is one pointer, so the default costs 8KB per `protothread_t` on a 64-bit machine. **Set `PT_NWAIT=1` on a memory-constrained system**: with only a handful of waiters, one linear wait list is both smaller and faster than hashing.

`PT_NO_MALLOC`
> Define this to drop `<stdlib.h>`, `protothread_create()` and `protothread_free()`. Use `protothread_init()` on statically allocated storage instead.

`pt_assert(condition)`
> Define your own before including `protothread.h` to avoid `<assert.h>` entirely. By default it is `assert()` when `PT_DEBUG` is set, and a no-op (that still type-checks the expression) otherwise.

`PT_CRITICAL_T`, `PT_CRITICAL_ENTER()`, `PT_CRITICAL_EXIT(saved)`
> Mutual exclusion against interrupt handlers. See [Interrupt safety](#interrupt-safety). No-ops by default.

## Bare-metal and embedded use ##

Protothreads were invented for memory-constrained embedded systems, and this implementation is usable in one: no operating system, no heap, and no C library.

Only freestanding headers (`<stddef.h>`, `<stdint.h>`, `<stdbool.h>`) are included unconditionally. With this configuration:

```
-DPT_DEBUG=0 -DPT_NO_MALLOC -DPT_NWAIT=1
```

and `protothread_init()` on static storage, a program that uses protothreads, semaphores and reader-writer locks compiles under `-ffreestanding` and requires **zero libc symbols** at every optimization level. The scheduler plus one protothread costs about 700 bytes of code and 20 bytes of state, plus 32 bytes per protothread.

### Interrupt safety ###

**By default this library is not interrupt-safe.** The scheduler's lists are updated with several stores that are not atomic with respect to an interrupt handler. If an interrupt lands in the middle of one, a protothread can be silently and permanently orphaned: removed from its wait queue, never placed on the run queue, and unreachable by any future signal. No assertion fires.

So by default, `pt_signal()`, `pt_broadcast()` and `pt_kill()` must be called only from thread context, never from an interrupt handler.

Defining the macros changes that for those three calls, and for `pt_timer_run()`. It does **not** change it for `protothread_run()`, which must never be called from an interrupt handler under any configuration -- it runs your thread code, and an interrupt that re-enters it would start a second protothread on top of the one already running. The division is worth stating plainly:

| | from an interrupt handler |
|---|---|
| `pt_signal()`, `pt_broadcast()` | lists stay intact once `PT_CRITICAL_*` are defined |
| `pt_kill()`, `pt_timer_run()` | lists stay intact once `PT_CRITICAL_*` are defined |
| `protothread_run()` | **never safe** |
| `pt_wait()`, `pt_yield()`, `pt_call()` | never -- these only run inside a protothread |

Note the wording: "lists stay intact" is not the same as "correct". There is a second, independent hazard, described next.

#### Lost wakeups ####

`PT_CRITICAL_*` protects the scheduler's data structures. It does **not** make the ordinary condition-variable idiom safe against a signal from outside:

```
    protothread                     interrupt handler / other thread
    -----------                     --------------------------------
    while (!job->done)     <--- tests the predicate: false
                                    job->done = 1
                                    pt_signal(pt, job)   <-- nothing is
                                        waiting yet, so this is LOST
        pt_wait(c, job)    <--- enqueues, and sleeps forever
```

The predicate test and `pt_wait()`'s enqueue are not atomic with respect to another context, and no critical section can make them so, because `pt_wait()` returns from the function. Among protothreads this race cannot happen -- nothing runs in between -- which is exactly why it is easy to overlook when an interrupt handler is added later.

The fix is to not signal from the outside at all. Have the handler record what happened -- set a flag, push onto a queue -- and have the loop that owns `protothread_run()` turn that into a `pt_signal()` **between** protothread runs. At that point no protothread is mid-execution, so every waiter has finished enqueuing:

```c
for (;;) {
    drain_pending_signals(pt) ;    /* flags -> pt_signal(), in thread context */
    while (protothread_run(pt));
    wait_for_interrupt();
}
```

`demo/pool.c` is a complete working program built this way, and `demo/helper_thread.c` uses a self-pipe to the same end. A pleasant side effect: if signals are only ever raised from the scheduler's own context, the lists are never touched concurrently and `PT_CRITICAL_*` is not needed at all.

To signal a protothread from an interrupt handler, define the critical-section macros to disable and restore interrupts. They must nest, so `PT_CRITICAL_EXIT()` restores the saved state rather than unconditionally enabling. On Cortex-M with CMSIS:

```c
static inline uint32_t pt_critical_enter(void) {
    uint32_t s = __get_PRIMASK();
    __disable_irq();
    return s;
}
#define PT_CRITICAL_T       uint32_t
#define PT_CRITICAL_ENTER() pt_critical_enter()
#define PT_CRITICAL_EXIT(s) __set_PRIMASK(s)
```

The critical sections are short and O(1), except that `pt_signal()`, `pt_broadcast()` and `pt_kill()` walk one wait list. If interrupt latency is critical, keep `PT_NWAIT` large enough that wait lists stay short, or have the interrupt handler set a flag that the main loop turns into a `pt_signal()` at a safe point.

The usual bare-metal structure is an idle loop:

```c
for (;;) {
    while (protothread_run(&state));
    wait_for_interrupt();
}
```

## Compiler requirements ##

This implementation requires the gcc [labels-as-values](http://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html) extension (`&&label` and `goto *ptr`), so it needs **gcc or clang**. That includes `arm-none-eabi-gcc`, `armclang`, `avr-gcc`, `msp430-gcc` and the RISC-V toolchains. It does not work with IAR or ARMCC, which do not support computed goto; for those compilers use Dunkels' `switch`-based implementation instead.

Apart from that one extension the code is ordinary C. CI builds and runs the test suite on gcc and clang, on Linux and macOS, across `PT_DEBUG` and `NDEBUG` on and off, `PT_NWAIT` of 1, 4 and 1024, `-O0` through `-Os`, `-std=c99` through `-std=c23`, and under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer -- all with `-Wall -Wextra -Werror`. It also asserts that a freestanding build still needs no libc symbols at all, since one careless `#include` would quietly break that.

Two consequences of the `__LINE__`-based label naming are worth knowing:

  * You cannot put two protothread macros (`pt_wait`, `pt_yield`, `pt_call`) on the same source line. This is a compile error (`duplicate label`), never a silent bug.
  * A protothread function's blocking macros must all be in the same function; you cannot hide one inside a helper macro that is used twice on one line.

`pt_resume()` contains a dead address-of-label expression. That is not decoration: clang rejects an indirect `goto` in a function containing no address-of-label at all, so without it a protothread function that never blocks -- and therefore has no `pt_wait()`, `pt_yield()` or `pt_call()` to supply one -- fails to compile. It costs nothing; the generated code is byte-for-byte identical.

### Using it from C++ ###

The four headers compile as C++ as well as C, and CI builds and runs a real protothread -- yields, nesting through the semaphore, lock and timer helpers, and the computed goto -- under `g++` and `clang++` at `-std=c++11`, `c++17` and `c++20`.

Existing C code does not port unchanged, though. Every protothread function starts by recovering its context:

```c
ctx_t * const c = env ;
```

C++ will not convert `void *` implicitly, so each one needs a cast -- `(ctx_t *)env` works in both languages, `static_cast<ctx_t *>(env)` in C++ only. That is the single most repeated line in any protothread program, so expect to touch every protothread function. (Watch for one other C/C++ difference while porting: a `struct` tag declared inside another `struct` is visible at file scope in C, but scoped to the enclosing class in C++.)

One pleasant surprise: `clang++` rejects an initialized local declared after `pt_resume()` outright --

```
error: cannot jump from this indirect goto statement to one of its possible targets
```

-- because C++ forbids jumping into the scope of a variable with an initializer. That promotes the bug described under [Local variables](#local-variables) from a warning to a hard error. Note what it does *not* do: the silent case, a variable initialized *before* `pt_resume()` and re-initialized on every resume, is still accepted, so declaring those `const` remains the only thing that catches it. `g++` only warns where `clang++` errors, so the same source can build under one and fail under the other.

None of this makes protothreads idiomatic C++ -- there is no RAII, no type-safe context, and for new C++ code C++20 coroutines are the native answer. What the headers offer a C++ project is the same scheduler, usable from C++ translation units, which mostly matters when C++ and C code need to share one protothread scheduler.

## Conclusion ##

For many resource-constrained or real-time applications, using protothreads gives far better performance and uses much less memory than POSIX threads. At the same time, algorithms can be expressed much more clearly using protothreads than using the event-driven model.

## API Reference ##

### Thread execution context ###

These are macros (designed to look and act like function calls) whose first argument is a pointer to a user-defined context structure, `c` (assume the context structure's name is `context_t`, but that is up the the user). The type `pt_f_t` is a pointer to a protothread function.

`void pt_resume(struct context_t *c)`
> Every thread function must call this macro first, after initializing any local variables (which must be a function only of the arguments and each other, not any global state; see [Local variables](#local-variables)). If the thread is being resumed, `pt_resume()` causes it to `goto` the resumption point, which is where this function last blocked. If the function is being called for the first time (that is, the thread is not being resumed), `pt_resume()` has no effect.

`void pt_wait(struct context_t *c, void *channel)`
> Block until a signal is sent to the given channel. The channel is an arbitrary `void *` value which is usually chosen to be the address of a data structure whose state change the thread is interested. A channel itself has no state; the protothread system never uses the channel as an address (does not dereference it). Typically, after this function returns the condition being waited for is re-evaluated. Analogous to [POSIX pthread\_cond\_wait()](http://www.opengroup.org/onlinepubs/009695399/functions/pthread_cond_wait.html).

`void pt_yield(struct context_t *c)`
> Reschedule the current thread and release the CPU. It is like `pt_wait()` on a channel that is immediately signaled. The current thread queues itself behind all ready to run threads and returns control to the scheduler.

`void pt_call(struct context_t *c, pt_f_t child_func, struct child_context_t *child_context, arg...)`
> Immediately call the given protothread function, passing it the given environment and arguments, and wait for it to return. There can be no context switch between the start of this statement and the start of the child function. Be careful that argument evaluation has no side effects, since this call occurs every time the thread is resumed. The usual C compile-time type checking is performed on all arguments.

`bool_t pt_call_waited(struct context_t *c)`
> Returns TRUE if the most recent `pt_call()` blocked (either directly in the called function, or in a function that it called, recursively). If function **A** calls **B** and **B** blocks, then when it finally returns to **A**, it's sometimes helpful for **A** to know that other threads might have run, so it should reevaluate the state of the world. But if **B** didn't block, then **A** knows that only a limited change of state (namely, whatever **B** might do) could have occurred.

`void pt_reset(struct context_t *c)`
> Forget this function's saved resume point, so that the next time it runs it starts from the top rather than from where it last blocked. Useful to restart a protothread function, or to reuse a context structure.

`protothread_t pt_get_pt(struct context_t *c)`
> This returns the protothread object handle (`protothread_t`). It is a convenience that allows code in a thread context to call API functions that require a protothread object argument, such as `pt_create()` or `pt_signal()`.

### Either thread or non-thread execution context ###

`void pt_create(protothread_t, pt_thread_t *, pt_f_t func, void *env)`
> Schedule the given protothread function to run, passing it the given environment. This function becomes the top-level function of the thread. There is no context break between this call and the caller's next statement. The new thread queues behind all ready threads. Analogous to [POSIX pthread\_create()](http://www.opengroup.org/onlinepubs/009695399/functions/pthread_create.html).

`void pt_broadcast(protothread_t, void *channel)`
> Send a signal to the given channel, which wakes up (schedules) all threads waiting on the channel to run in the same order they blocked. If there are no threads waiting, this call has no effect; the signal is not queued (there is no "memory" associated with a channel). These threads queue behind all ready threads. Analogous to [POSIX pthread\_cond\_broadcast()](http://www.opengroup.org/onlinepubs/009695399/functions/pthread_cond_broadcast.html).

`void pt_signal(protothread_t, void *channel)`
> Same as `pt_broadcast()` but wakes up only one (the oldest) waiting thread. Analogous to [POSIX pthread\_cond\_signal()](http://www.opengroup.org/onlinepubs/009695399/functions/pthread_cond_signal.html).

`bool_t pt_kill(pt_thread_t *)`
> Remove a thread from whatever list it is on, so that it is never scheduled again. Returns TRUE if the thread was found (it is not an error to kill a thread that has already exited). This is dangerous unless the thread was written to expect it: the thread is stopped wherever it happens to be blocked, and any resources it holds -- allocated contexts, semaphores, locks -- are not released. Do not call this on the currently running thread. If a destructor was installed with `pt_set_atexit()`, it runs after the thread is unlinked.

`void pt_set_atexit(pt_thread_t *, void (*func)(void *env))`
> Install an optional destructor, called with the thread's top-level environment when `pt_kill()` removes the thread. It is not called when a thread exits normally by returning `PT_DONE`. Call this after `pt_create()`, which clears it.

`protothread_t protothread_create(void)`
> This is usually only called once to create the overall protothread object. It returns the protothread handle (or NULL if allocation fails). The protothread system uses no global variables. All protothread state is within this object; multiple protothread instances are independent. This is the only protothread API function that allocates memory, and it can be compiled out with `PT_NO_MALLOC`.

`void protothread_free(protothread_t)`
> Free the state allocated with `protothread_create()`. There must be no threads associated with this object.

`void protothread_init(protothread_t)`
> Initialize a `struct protothread_s` that you allocated yourself, statically or otherwise. This is the alternative to `protothread_create()` on systems with no heap.

`void protothread_deinit(protothread_t)`
> The counterpart to `protothread_init()`. It frees nothing; when `PT_DEBUG` is set it asserts that no threads remain on any list.

### Scheduling ###

`bool_t protothread_run(protothread_t)`
> Run the next ready thread (if there is one). Returns TRUE if there remains at least one thread ready to run (more work to do).

> **Never call this from an interrupt handler**, even with the `PT_CRITICAL_*` macros defined. Those make the scheduler's *lists* safe against interrupts; nothing can make `protothread_run()` safe, because it runs your thread code. An interrupt arriving while a protothread is running and calling `protothread_run()` would re-enter the scheduler and start a second thread on top of the first. When `PT_DEBUG` is set, the `pt_assert(s->running == NULL)` at the top of `protothread_run()` catches this.

`void protothread_set_ready_function(protothread_t, void (*ready_function)(void *), void *env)`
> This function lets you use protothreads with an existing scheduler (that you can't or don't want to modify). You don't need this function if you are providing your own scheduler. This function is usually called once during initialization. Its effect is to arrange to have the protothreads system call the given `ready_function` (passing it `env`) when a thread becomes ready (and no threads were ready), and no thread is currently running. You can pass NULL for `ready_function` to disable this feature.

> The given `ready_function` generally schedules (using whatever method is available on your system) another function that calls `protothread_run()` repeatedly until there are no more threads to run (`protothread_run()` returns FALSE). The `ready_function` should not call `protothread_run()` directly.

> If an interrupt handler signals a protothread, `ready_function` is called from that interrupt context. The library calls it outside its own critical section, so interrupts are at whatever level the handler is running at, not masked. Keep it short, and note that the rule above becomes a hard requirement there: scheduling the runner is fine, calling `protothread_run()` is not.

> To prevent a sequence of protothread executions from holding onto the CPU for too long, the function can limit the number of times it calls `protothread_run()`; for example it may run no more than 20 threads before returning to the main scheduler to let other things (outside of protothreads) run. But if it does so (if the last call to `protothread_run()` returns TRUE), it should reschedule itself because there is still work to do.

### Semaphores ###

`#include "protothread_sem.h"`. A semaphore is an ordinary `unsigned int` that you initialize yourself (to 1 for mutual exclusion). Acquiring one needs a `pt_sem_env_t` context, which lives in your context structure like any other nested function's.

`void pt_sem_acquire(struct context_t *c, pt_sem_env_t *sem_env, unsigned int *value)`
> Wait until the semaphore is non-zero, then decrement it. May block.

`void pt_sem_release(pt_sem_env_t *sem_env, unsigned int *value)`
> Increment the semaphore and wake any waiters. Guaranteed not to block.

This implementation is deliberately not fair: a thread can release and immediately reacquire ahead of existing waiters, which costs fewer context switches. If that matters, `pt_yield()` before reacquiring.

### Timers ###

`#include "protothread_timer.h"`. The library never reads a clock -- there is no portable one, and depending on one would cost the freestanding property. Instead you drive it from whatever time source you already have: a tick interrupt, a SysTick handler, or the idle loop. A sleep is measured from the most recent `pt_timer_run()`, so the resolution of `pt_sleep()` is your tick period.

`void pt_timers_init(pt_timers_t *timers, pt_time_t now)`
> Initialize a timer list. There is usually one per clock, and one clock.

`void pt_sleep(struct context_t *c, pt_timer_env_t *timer_env, pt_timers_t *timers, pt_time_t ticks)`
> Block for `ticks`. Needs a `pt_timer_env_t` in the calling context structure, like any other nested function's context.

`void pt_timer_run(protothread_t, pt_timers_t *timers, pt_time_t now)`
> Wake every protothread whose deadline has arrived, and record `now` as the base for subsequent sleeps. `now` may jump by more than one tick; everything that became due is woken. Safe to call from an interrupt handler if the `PT_CRITICAL_*` macros are defined.

`bool_t pt_timer_next(pt_timers_t const *timers, pt_time_t *deadline)`
> Report the soonest deadline, or FALSE if nothing is sleeping. Use this in an idle loop to decide how long the CPU can be stopped.

`bool_t pt_timer_cancel(pt_timers_t *timers, pt_timer_env_t *timer_env)`
> Remove a sleeper early; returns TRUE if it was still pending. Call this before `pt_kill()`ing or freeing a protothread that might be sleeping, otherwise the timer list will retain a dangling entry.

The clock type is `PT_TIME_T` (default `uint32_t`), paired with the signed `PT_TIME_DIFF_T` (default `int32_t`). **Counter wraparound is handled correctly**: comparisons use a signed difference rather than a direct `>`, so a 32-bit millisecond clock behaves properly across its 49-day rollover. The one requirement is that no live deadline be more than half the counter range in the future -- about 24 days for that clock.

A typical bare-metal idle loop:

```c
for (;;) {
    pt_timer_run(pt, &timers, clock_now());
    while (protothread_run(pt));
    if (pt_timer_next(&timers, &deadline)) {
        sleep_until(deadline);
    } else {
        wait_for_interrupt();
    }
}
```

### Reader-writer locks ###

`#include "protothread_lock.h"`. A `pt_lock_t` allows either many concurrent readers or one writer. Each thread needs a `pt_lock_env_t` in its context structure.

`void pt_lock_init(pt_lock_t *lock)`
> Initialize the lock. Must be called before use.

`void pt_lock_acquire_read(struct context_t *c, pt_lock_env_t *lock_env, pt_lock_t *lock)`
> Acquire the lock for shared (read) access. May block.

`void pt_lock_acquire_write(struct context_t *c, pt_lock_env_t *lock_env, pt_lock_t *lock)`
> Acquire the lock for exclusive (write) access. May block.

`void pt_lock_release_read(pt_lock_env_t *lock_env, pt_lock_t *lock)`
`void pt_lock_release_write(pt_lock_env_t *lock_env, pt_lock_t *lock)`
> Release the lock. Guaranteed not to block.

Requests are granted in arrival order, so a steady stream of readers cannot starve a waiting writer. Consecutive readers at the head of the queue are all started together.

## License ##

MIT. See [LICENSE](LICENSE).

The one exception is `FindPROTOTHREAD.cmake`, which is third-party code by Ryan Pavlik (Iowa State University) under the Boost Software License 1.0; its notice is preserved in the file. Both licenses are permissive and GPL-compatible. Every file carries an SPDX identifier.

## References and Acknowledgements ##

[Wikipedia protothreads](http://en.wikipedia.org/wiki/Protothreads)

[POSIX thread reference](http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/pthread.h.html)

I wish to gratefully acknowledge Adam Dunkels (with support from Oliver Schmidt) for inventing this brilliant idea. Please see his [web site](http://dunkels.com/adam/pt/).

My thanks to Paul Soulier for introducing me to the concept of protothreads, and to Marshall McMullen and John Rockenfeller for reviewing drafts of this article.

## Contact and contributing ##

The project lives at [github.com/LarryRuane/protothread](https://github.com/LarryRuane/protothread). [TODO.md](TODO.md) lists ideas and deferred work, if you are looking for somewhere to start.

Bug reports, questions and pull requests are welcome as [GitHub issues](https://github.com/LarryRuane/protothread/issues) -- please prefer those to email, so that other people can find the answers. Otherwise: _LarryRuane@gmail.com_
