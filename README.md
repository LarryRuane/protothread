## Introduction ##

[![CI](https://github.com/LarryRuane/protothread/actions/workflows/ci.yml/badge.svg)](https://github.com/LarryRuane/protothread/actions/workflows/ci.yml)


[Protothreads](http://en.wikipedia.org/wiki/Protothreads) is a programming model invented by Adam Dunkels that combines the advantages of _event-driven_ (sometimes also called _state machine_) programming and _threaded_ programming. The trick underneath, jumping back into the middle of a function, goes back to [Duff's device](https://en.wikipedia.org/wiki/Duff%27s_device) (1983), Tom Duff's `switch` that jumps into the middle of a loop. Simon Tatham turned it into [coroutines in C](https://www.chiark.greenend.org.uk/~sgtatham/coroutines.html), and Dunkels built protothreads on that. The main advantage of the event-driven model is efficiency, both speed and memory usage. The main advantage of the threaded model is [algorithm clarity](http://dunkels.com/adam/dunkels06protothreads.pdf). Protothreads gives you both. A protothread is a lightweight thread. As with event-driven programming, there is a single stack, shared by every protothread. But as with threaded programming, a function can (at least conceptually) block. (This is why protothreads are called "stackless". There is a stack, but no thread has one of its own.) This protothreads implementation:
  * is not an implementation of POSIX threads, and isn't compatible with other versions of protothreads
  * does not require assembly-language code or use setjmp/longjmp
  * is independent of CPU architecture
  * schedules threads non-preemptibly and deterministically

### Concurrency, not parallelism ###

If protothreads never run two things at once, why threads at all? Because running things at once was never the main reason for threads. Rob Pike makes the distinction in his talk [Concurrency is not Parallelism](https://go.dev/blog/waza-talk). Concurrency is *dealing with* many things at once; parallelism is *doing* many things at once. A program that talks to a sensor, a radio, and a user interface has to deal with all three, whether it has one core or eight. Threads let each of those activities be written as a sequence of steps, in the order they happen, instead of being taken apart into callbacks and state variables. That clarity is what protothreads provides, for a few dozen bytes per thread. They can be paired with ordinary threads for parallelism (see [Protothreads on a multi-core system](#protothreads-on-a-multi-core-system)).

### Why another protothreads implementation? ###

I wrote this from scratch, and it is not compatible with other versions of protothreads. I came to it from a Unix kernel background. Dunkels' idea is brilliant, but I wanted a programming interface that felt more like the threaded code I was used to. These are the main differences:

  * **Blocking functions nest with an ordinary call.** A protothread function can `pt_call()` another protothread function, passing it arguments, and the one it calls can block. This works to any depth, so you can factor blocking code into subroutines the way you would anywhere else. Dunkels' library nests too, with `PT_SPAWN()`. What differs is how a blocked thread gets resumed, the third point below.
  * **You can block anywhere**, including inside a `switch` statement. Implementations built on Duff's device cannot, because they're already using `switch`.
  * **A real scheduler, with _wait channels_.** `pt_wait()`/`pt_signal()`/`pt_broadcast()` on an arbitrary address deliberately mirror condition variables (`pthread_cond_wait()`, except a `pthread_cond_t` variable isn't needed) and the classic Unix kernel [`sleep()`/`wakeup()`](https://www.ques10.com/p/47397/sleep-event-and-addresses-in-unix-memory-managem-1/). Each thread is on either the run list or a wait list, and a blocked thread does not run until its channel is signaled. Dunkels' library leaves scheduling to the application. A blocked protothread is simply called again later, and re-tests its condition each time, re-running every level of any nesting to get there. Semaphores, reader-writer locks, timers, and message queues are built on top of it, in optional headers of their own.

Nesting and arbitrary blocking require [gcc label variables](http://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html), which aren't standard C, restricting the compiler to **gcc or clang** (see [Compiler requirements](#compiler-requirements)). Dunkels' `switch`-based version is portable to any C compiler.

### What you get ###

  * **Header-only.** Copy the headers into your project. Nothing to build, nothing to link, no submodules, no dependencies.
  * **Usable from C++.** The headers compile as C++ too; see [Using it from C++](#using-it-from-c).
  * **Runs with no C library at all.** A production build needs zero libc symbols and compiles `-ffreestanding`, so it runs on a bare microcontroller with no RTOS underneath it. See [Bare-metal and embedded use](#bare-metal-and-embedded-use) and [Interrupt safety](#interrupt-safety).
  * **Small, and known at compile time.** 28 bytes of RAM per protothread and about 700 bytes of code on a 32-bit microcontroller. There's no per-thread stack to size, so the compiler can tell you the real number instead of you guessing the worst case.
  * **Deterministic, which makes bugs reproducible.** Your program decides the schedule, not the OS. So a seeded pseudo-random test replays exactly, and a failure found in hour three of a soak test can be reproduced on demand. This isn't free, since every external interface has to be mockable, time included. But protothreads removes the one source of nondeterminism you can't reach from inside your own program. See [Deterministic execution](#deterministic-execution).
  * **Fast.** Against POSIX threads doing the same work, measured by the [benchmark](#benchmarks) included in this repository:

| | protothread | pthread | ratio |
|---|---|---|---|
| context switch | 4.7 ns | 3,241 ns | **690x** |
| create + destroy | 2.8 ns | 28,852 ns | **10,300x** |
| memory per thread | 56 bytes | 16,384 bytes | **293x** |

Protothreads are faster here because they do less. There's no kernel transition, no scheduler, no per-thread stack, no mutex locking. POSIX threads provide preemption and real parallelism, which protothreads do not. See [Memory overhead and performance](#memory-overhead-and-performance-benchmarks) for a detailed comparison and [Protothreads on a multi-core system](#protothreads-on-a-multi-core-system) for using both together. Also included:
  * optional semaphores, reader-writer locks, timers, and message queues, each in its own header and built entirely on the core
  * about 1000 lines of test code
  * gdb (debugger) macros to print the stack traces of a given protothread or all protothreads
  * a CMake find module, `FindPROTOTHREAD.cmake`. Copy it into another CMake project, and `find_package(PROTOTHREAD)` will locate the installed headers

### Using it ###

There is nothing to build. Copy `protothread.h` into your project and include it:

```c
#include "protothread.h"
```

To build and run the test suite:

```
cmake -S . -B build && cmake --build build && ./build/pttest
```

### The API ###

The core is twenty-one entries in the following two tables, all in `protothread.h`. The `protothread_` prefix is used for operations on the overall protothread object; the `pt_` prefix is for operations involving specific protothreads. Everything else in `protothread.h` is internal and has a `pt_i_` or `PT_I_` prefix. Anything *without* those prefixes is API you can rely on, and anything with one may change in a later release.

**The scheduler**

| call | what it does |
|---|---|
| `protothread_init(s)` | initialize a `protothread_t` you allocated yourself |
| `protothread_deinit(s)` | check nothing is still scheduled (`PT_DEBUG` builds) |
| `protothread_create()` | allocate and initialize one (needs `malloc`) |
| `protothread_free(s)` | deinitialize and free it |
| `protothread_run(s)` | run one ready protothread; true if more remain |
| `protothread_set_ready_function(s, f, env)` | called when the run list becomes non-empty |
| `pt_create(s, thread, func, env)` | create a protothread and make it ready |
| `pt_broadcast(s, channel)` | make every waiter on `channel` ready; the one to use by default |
| `pt_signal(s, channel)` | make only the oldest waiter on `channel` ready |
| `pt_kill(thread)` | unschedule one; true if it was still scheduled |
| `pt_is_alive(thread)` | has it not yet exited or been killed? never blocks |

**Inside a protothread function**, where `c` is the context. All of these are macros.

| call | what it does |
|---|---|
| `pt_resume(c)` | first statement of every protothread function |
| `pt_wait(c, channel)` | block until `channel` is signaled |
| `pt_wait_until(c, channel, cond)` | the same, but safe against an interrupt handler |
| `pt_yield(c)` | let other ready protothreads run, then continue |
| `pt_call(c, func, child_c, ...)` | call a protothread function that may block |
| `pt_call_waited(c)` | did that `pt_call()` block? |
| `pt_join(c, thread)` | block until that protothread has exited |
| `pt_reset(c)` | forget the resume point; start again from the top |
| `pt_get_pt(c)` | the `protothread_t` this protothread belongs to |
| `PT_DONE` | what a protothread function returns when it is finished |

Types: `protothread_t`, `pt_thread_t`, `pt_func_t`, `pt_t`, `pt_f_t`, `env_t`, `bool_t`. The compile-time knobs are under [Configuration](#configuration).

That is everything you need to start.

For contrast, POSIX [`pthread.h`](https://pubs.opengroup.org/onlinepubs/9699919799.orig/basedefs/pthread.h.html) declares 101 functions. That isn't a fair comparison. POSIX threads give you preemption, real parallelism, priorities, and synchronization between processes. But most of that count isn't extra functionality; it's configuration of the extra functionality. Forty-five of those functions manage attribute objects; thirty-five of them are `get`/`set` pairs; and another ten are `init` and `destroy` for the synchronization objects themselves. The rest involve detach state, scheduling scope and inheritance, cancellation state and type, cleanup handler stacks, thread-specific data keys with destructors, four mutex types, three priority protocols, and process-shared variants of most of it.

Nearly all of that exists because a pthread can be interrupted between any two instructions. A protothread can't; it runs until it explicitly blocks, so there is no priority to invert, no cancellation point to reason about, and no per-thread stack to size. Those functions aren't needed here.

Four optional headers, built on the core, provide commonly used abstractions:

  * `protothread_sem.h` -- counting semaphores
  * `protothread_timer.h` -- sleeping, driven by a clock you supply
  * `protothread_lock.h` -- reader-writer locks
  * `protothread_queue.h` -- message queues, what Go calls channels

Each is ordinary protothread code using `pt_wait()` and `pt_broadcast()`; these can also be considered examples of what's possible using the base API. They are described under [Built on top](#built-on-top-semaphores-timers-locks-and-queues).

## Threads that share one stack ##

Protothreads are often called *stackless* threads, which can sound as though there is no stack at all. There's exactly one, the ordinary C stack, and every protothread runs on it, one at a time, just as every handler in an event-driven program does. What a protothread lacks is a stack of its *own*, so it leaves nothing on that shared stack when it blocks, freeing up the stack for another protothread to use.

Every protothreads implementation works the same basic way. When a function wants to wait for an event (that is, suspend itself and let other threads run), its current location within the function, conceptually its line number or program counter, is saved in a `struct`. Then it returns to the scheduler or idle loop, releasing the stack. The scheduler runs a different thread, handles interrupts, or waits for an external event to occur. When the waited-for event occurs, the scheduler calls the function in the usual way, and the first thing the function does is `goto` the previously saved location. This location might be within levels of nested loops and `if` statements.

The `return` and `goto` statements are hidden within macros, so the application code looks like regular threaded code.

Contrary to the usual C convention and unlike Dunkels' `PT_WAIT_UNTIL`, those macros are lowercase. They appear constantly in protothread code, and capitals would seem like SHOUTING, drowning out the main program logic. There are precedents for this, such as the C standard's `setjmp`, `assert`, and `va_arg`, and the Linux kernel's `wait_event()`, which is similar to `pt_wait()`. The user must be careful to observe the usual rules about macro arguments having no side-effects, since they are evaluated more than once. `PT_DONE` is a value, so it's left uppercase.

This implementation uses a gcc and clang feature that lets you store the address of a `goto` label in a variable. Later you can `goto` that variable from within the same function, even if the function has returned and is now being called again. The performance is similar to event-driven software; a context switch is just a few simple C statements.

You can think of protothreads as a generalization of event-driven programming. An event-driven work item (request in progress) typically consists of a pointer to a function (or a _state_ variable when using a big `switch` statement instead of individual functions) and a _context_ structure. These encapsulate the current state of the work item. A protothread records not just a function pointer (and context), but also a location _within_ the function. That location encodes a more fine-grained form of state than a simple function address. Actually, a protothread is even more general. If function **A** calls function **B**, **B** calls **C**, and **C** blocks, then the thread has _three_ pointers into the middle of those functions. Each can have several possible values at each nesting level, so together they encode a lot of state.

## Example - Hello, world ##

Below is a complete program that creates one protothread, prints from it, and exits. There's a well-known [pthreads](https://en.wikipedia.org/wiki/Pthreads) version of this that calls `pthread_create()` and then `pthread_join()`. The operating system automatically runs pthreads. The main function is the default initial pthread, so both the main function and the child function run in separate threads.

Here, the OS knows nothing about protothreads. Our own main function must _run_ them, and it isn't a protothread itself. So, although the library does include a join function (`pt_join()`, allowing one protothread to wait for the completion of another), it's not needed here. Instead, the main function runs the scheduler, which runs any available protothreads, until there's nothing left to run. This example prints a message from the protothread and another from the main function, then exits.

Every protothread needs a _context_ structure. Its first two fields, which the user doesn't need to worry about, are used by the protothreads system internally:
```
 #include <stdio.h>
 #include "protothread.h"

 typedef struct {
     pt_thread_t pt_thread;
     pt_func_t pt_func;
     char const * message;
 } hello_context_t;
```
Here we have one protothread function, which takes its context as a `void *`. Its first executable statement is always `pt_resume()`:
```
 static pt_t
 hello_thr(void * const env)
 {
     hello_context_t * const c = env;
     pt_resume(c);

     printf("%s\n", c->message);
     return PT_DONE;
 }
```
_Protothread functions_ are special in that they can only run within a protothread (they can't be called directly on their own). To make this clear, our convention is to add the `_thr` suffix. Another clue is that a protothread function must always have `pt_t` as its return value type. A protothread function can call non-protothread functions (such as `printf` here); once the protothread function is running, it's just normal C code.

In our example, `main()` creates the protothread _system_ (object), creates the protothread, and runs it:
```
 int
 main(void)
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
```
In general, `protothread_run()` runs (at most) one ready-to-run protothread until it blocks or exits, and returns true if any protothread is still ready to run. The thread context, `c`, is an ordinary local variable. A protothread (function) can't allocate its own context structure, since that needs to exist to get the protothread function running. So usually, whoever creates the protothread (calls `pt_create()`) also allocates its context.

A protothread doesn't need a stack of its own, only somewhere to keep its state, including its arguments (`message`, in this case). It does have to outlive the protothread, so a local works here but wouldn't if the calling function returned while the protothread was still going.

This protothread never blocks, which makes it a thread in name only. The call to `protothread_run()` calls `hello_thr()`, which prints and returns, causing `protothread_run()` to return false, dropping out of the loop (so the loop isn't actually needed here). The next example blocks.

## Example - Producer / Consumer ##

Here is the well-known producer-consumer algorithm in its simplest form. The producer generates the integers 1 through 100, and the consumer checks that it receives them in order. They share a mailbox holding a single integer (no queuing), so the producer has to wait when it is "full" and the consumer waits while it's empty. This is still just to show the basic idea; details are explained later. Each of the two protothreads needs a context structure, and here both can have the same type:
```
 typedef struct {
     pt_thread_t pt_thread;
     pt_func_t pt_func;
     int i;
     int * mailbox;
 } pc_thread_context_t;
```
Besides the first two system fields, it holds a counting index, `i`, and a pointer to the shared mailbox. A value of zero in the mailbox means it's empty. For the producer, `i` is the next value to write to the mailbox; for the consumer, it's the next value to expect from the mailbox. With pthreads, `i` would be a local variable. Here it's part of the protothread state, so it must be in the context structure. A protothread doesn't have a persistent stack as a pthread does. (Here, we mean persistent across blocking and resuming.) This is why you see `c->i` instead of just `i`.

Here are the producer and consumer threads (notice the `pt_t` return values):
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
         pt_broadcast(pt_get_pt(c), c->mailbox);
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
         pt_broadcast(pt_get_pt(c), c->mailbox);
     }
     return PT_DONE;
 }
```
The producer thread waits until the mailbox is empty, then writes the next value to the mailbox and wakes the consumer. The consumer thread waits until something appears in the mailbox, verifies that it's the expected value, writes a zero to signify that the mailbox is empty, and wakes up the producer. _There can be no race conditions, because all of this actually runs single-threaded._ The threads signal each other using the address of the mailbox as the _channel_. It's common to use the address of the data structure whose state changes are of possible interest to waiting threads as the channel. In its role as a channel, the address is never dereferenced. It's only used to match signals with waits. The tests are `while` loops rather than `if` statements because a wakeup is only a hint that the condition may have changed; see [Wait channels](#wait-channels).

A channel has no memory. Signaling one when no thread is waiting on it has no effect. `pt_broadcast()` wakes every thread waiting on the channel, and is recommended unless you have a reason not to. `pt_signal()` wakes only the longest-waiting one. Where this interface comes from, and why it is used instead of condition variables, is covered under [Wait channels](#wait-channels).

The main test function allocates the overall protothread object or instance (`pt`) and a context for each thread, and initializes the mailbox to empty. Then it creates the threads. Creating a thread doesn't run it. It's only _ready_ to run. Then the main function runs the protothread system until there is no more work to do:
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

So why is this better than an event-driven version of this example? The event-driven version of the consumer is a two-state machine, a `switch` on "mailbox empty" and "mailbox full". For two states that's readable. But notice what's gone. With protothreads, the producer and the consumer are each a `for` loop, which is how you would describe what they do, while in the event-driven version neither loop appears. The only loop is the one around the `switch` that dispatches events, which is what `protothread_run()` does internally. Each activity's own loop has been turned inside out into transitions between states, so you have to reconstruct it to see that it is a loop at all.

The difference becomes more obvious with complexity. Every `pt_wait()` in a protothread is a state that the event-driven version has to name, store, and dispatch on by hand. Naming is the hard part. Finding a good name for every intermediate state of a long sequence is often harder than writing the sequence. A protothread's state is its position in the function, which needs no name.

### The same, with a queue ###

The mailbox above is a queue of one, written by hand. With a [message queue](doc/queue.md) from `protothread_queue.h`, the two protothreads are even simpler:
```
 /* defines the queue operations, but doesn't allocate the queue or any storage, even statically */
 PT_QUEUE_DEFINE(intq, int)

 typedef struct {
     pt_thread_t pt_thread;
     pt_func_t pt_func;
     intq_t * q;
     int i;
     int value;
 } qpc_context_t;

 static pt_t
 producer_thr(void * const env)
 {
     qpc_context_t * const c = env;
     pt_resume(c);

     for (c->i = 1; c->i <= 100; c->i++) {
         pt_queue_send(c, c->q, &c->i);
     }
     return PT_DONE;
 }

 static pt_t
 consumer_thr(void * const env)
 {
     qpc_context_t * const c = env;
     pt_resume(c);

     for (c->i = 1; c->i <= 100; c->i++) {
         pt_queue_receive(c, c->q, &c->value);
         assert(c->value == c->i);
     }
     return PT_DONE;
 }
```
The queue needs storage, `int slots[4]; intq_t q; intq_init(&q, slots, 4);`, and the rest of the test is as before.

You can think of the two versions as "moving" different things. In the mailbox version the data stays put and the protothreads come to it. Each one zips in, checks the mailbox, waits on it, changes it, zips out, and wakes the other. In the queue version the protothreads stay put, and the data moves, one value at a time, from the producer to the consumer, as if along a wire between them. The waiting and waking haven't gone away. They're inside `send` and `receive`. And with room for four values, the producer can run up to four steps ahead before it has to wait, so the two take turns less often. Passing 100 values takes 51 protothread runs here, against 201 through the mailbox.

## Bare-metal and embedded use ##

Protothreads were invented for memory-constrained embedded systems, and this implementation runs on a bare microcontroller, with no operating system, no heap, and no C library.

Only freestanding headers (`<stddef.h>`, `<stdint.h>`, `<stdbool.h>`) are included unconditionally. With this configuration:

```
-DPT_DEBUG=0 -DPT_NO_MALLOC -DPT_NWAIT=1
```

and `protothread_init()` on static storage, a program that uses protothreads (and any of the extensions: semaphores, reader-writer locks, timers, or queues) compiles under `-ffreestanding` and requires **no libc symbols** at any optimization level. The scheduler costs about 700 bytes of code and 20 bytes of state, plus 28 bytes per protothread.

### Interrupt safety ###

**By default this library is not interrupt-safe.** The scheduler's lists are updated with several stores that are not atomic with respect to an interrupt handler. If an interrupt occurs in the middle of one, a protothread can be silently and permanently orphaned. It's removed from its wait list, never placed on the run list, and no future signal can reach it. No assertion fires.

So by default, `pt_signal()`, `pt_broadcast()`, and `pt_kill()` must be called only from thread context, never from an interrupt handler.

Defining three macros ([how](#defining-the-critical-section-macros)) eliminates that restriction, for `pt_timer_run()` too. It does **not** change it for `protothread_run()`, which must never be called from an interrupt handler. It runs your thread code, and an interrupt handler that re-enters it would run a second protothread on top of the one already running. These are the restrictions:

| | from an interrupt handler |
|---|---|
| `pt_signal()`, `pt_broadcast()` | lists stay intact once `PT_CRITICAL_*` are defined |
| `pt_kill()`, `pt_timer_run()` | lists stay intact once `PT_CRITICAL_*` are defined |
| `protothread_run()` | **never safe** |
| `pt_wait()`, `pt_yield()`, `pt_call()` | never -- these only run inside a protothread |

But "lists stay intact" isn't the same as "correct". There's a second, independent hazard, described next.

#### Lost wakeups ####

`PT_CRITICAL_*` protects the scheduler's data structures. It does **not** make the ordinary condition-variable idiom safe against a signal from outside. In the following, the predicate test and `pt_wait()`'s enqueue are not atomic with respect to interrupts, so the wakeup is lost forever:

```
    protothread                     interrupt handler / other thread
    -----------                     --------------------------------
    while (!job->done)     <--- tests the predicate: not done
                                    job->done = 1
                                    pt_broadcast(pt, job)
        pt_wait(c, job)    <--- enqueues, and sleeps forever
```

This race can't happen between protothreads. That's why it's easy to overlook when an interrupt handler is added later. (This same race is why `pthread_cond_wait()` takes a mutex. There, the race can also occur between threads.)

There are two ways out, and they can be mixed.

**Wait with `pt_wait_until()`**, which does the test and the enqueue inside one critical section:

```c
pt_wait_until(c, job, job->done);
```

For this to be safe against interrupts, define `PT_CRITICAL_*`. The library uses this for its own two cases, which is what makes `pt_timer_run()` safe to call from a tick interrupt and `pt_kill()` safe against a protothread waiting in `pt_join()`.

**Or don't signal from the outside at all.** Have the handler record what happened (set a flag, push onto a queue, or send to a self-pipe), and have the loop that owns `protothread_run()` turn that into a `pt_signal()` **between** protothread runs. At that point no protothread is mid-execution, so every waiter has finished enqueuing. (This also shows how to "slow down" the idle loop when there's nothing to do.)

```c
for (;;) {
    process_pending_signals(pt);    /* flags -> pt_signal(), in thread context */
    while (protothread_run(pt));
    disable_interrupts();
    if (!signals_pending()) {
        wait_for_interrupt();     /* still wakes for an interrupt already pending */
    }
    enable_interrupts();
}
```

Check for work with interrupts masked before waiting. Otherwise an interrupt occurring after the last check but before `wait_for_interrupt()` is serviced right there, and the loop sleeps until a later interrupt with work already pending. On Cortex-M, `__WFI()` with `PRIMASK` set still wakes for a pending interrupt, which is what makes this work. Most other cores have an equivalent.

`demo/pool.c` is a complete working program built this way, and `demo/helper_thread.c` uses a self-pipe to the same end. If signals are only ever raised from the scheduler's own context, races can't occur, so `PT_CRITICAL_*` isn't needed at all.

## How does it work? ##

The two most interesting calls in the producer/consumer example are `pt_resume()` and `pt_wait()`. Let's expand these in the producer thread function to see how they work. (The full API reference manual is given at the end of this article.) The `pt_func` member of the context structure contains the protothreads-private _function context_. The protothread macros access this field by name.
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
        pt_broadcast(pt_get_pt(c), c->mailbox);
     }
     return PT_DONE;
 }
```
The first time the thread runs, its label variable is `NULL`, so it does not `goto`, and the code enters the `for` loop from the top. When it reaches the call to `pt_wait()`, it saves the address of the label. The label's name is made unique with `__COUNTER__`, and the double ampersand is the syntax for the address of a label. Then it puts the thread on a _waiting_ list within the protothreads object, to wait for a signal on the address of the mailbox, and returns `PT_I_WAIT`. (The return value tells the caller, the scheduler or a `pt_call()`, whether the function is waiting or has finished.)

When this thread is resumed, the label variable is non-NULL, so `pt_resume()` jumps to the value of the label variable, and execution continues from where it left off. In this case, the producer thread continues in the `while` loop, waiting for the mailbox to become empty. As when using POSIX condition variables, it's common to re-test the condition being waited for.

### Structure of a protothread ###

A protothread comprises one or more _protothread functions._ Only a protothread function can block or call a function that blocks. The overall protothread needs a `pt_thread_t` structure, which contains the protothread-private context for the thread. Also, _each level_ of nested blocking function needs a user-defined context structure. It must contain a `pt_func` member of type `pt_func_t`, which holds the protothread-private context for that function. If **A** calls **B** which in turn calls **C** (and **C** can block), each needs its own instance of a structure containing at least a `pt_func` member.

This structure also contains user-defined state that is specific to that function, usually what would be local variables with POSIX threads. Protothread functions generally cannot use local variables, because their values are not preserved across waits. (You may have noticed the example uses `c->i` instead of `i` for the loop index.) See [Local variables](#local-variables) below.

There are only two ways to run a protothread function. It should never be called directly.

  * Any code (a regular function or a protothread function) can call `pt_create()` to create a new thread. You specify a function address and a context pointer which is passed to the function as its only argument. This call schedules the thread (does not run it directly). A scheduled thread can be canceled with `pt_kill()`, but only if it was written to expect that. Use `pt_is_alive()` to know if a thread has finished, or (within a protothread function) `pt_join()` to block until it has. The `pt_create()` call also requires a unique (to this thread) `pt_thread_t` structure, which can be allocated anywhere, but is typically included within the top-level function's context structure (as in the structure `pc_thread_context_t` above). That structure must outlive the protothread. **A protothread may not free the storage holding its own `pt_thread_t`**, because the scheduler records that the thread has exited after its function returns.
  * A protothread function can execute `pt_call()`. This has the same semantics as a normal function call, but you must use `pt_call()` when calling a protothread function. Any number of arguments of any types may be passed to the called function, with the usual type checking. The first argument must be a pointer to a context structure, containing a `pt_func` member, where the called function keeps its state. Because `pt_call()` calls the function directly, that parameter can be declared with its real type (`read_context_t * const c`, not `env_t`), and the compiler checks it like any other argument. Only a *top-level* function, the one passed to `pt_create()`, has to take `env_t` and cast it, because the scheduler keeps every protothread's function in one list and cannot know their types. (A nested function called through a `pt_f_t` function pointer needs `env_t` too, for the same reason.)

Any return statement you write must return `PT_DONE`. The return value belongs to the protothreads system, which uses it to pass blocking status (whether the function completed or blocked) up the call chain. That's less of a restriction than it looks. The caller owns the callee's context structure, which makes a better return channel than a return value. It can have any number of values of any types, and unlike a return value, it survives blocking. Declare the results alongside the arguments:
```
 typedef struct {
     pt_func_t pt_func;
     int n;               /* in:  number of records to read */
     int count;           /* out: number actually read */
     int error;           /* out: zero, or an errno-style code */
 } read_context_t;

 static pt_t
 read_thr(read_context_t * const c)    /* typed: no env_t, no cast */
 {
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
This costs nothing. The caller already allocates the callee's context, so there is no copying and no additional storage. When the top-level protothread function returns, the thread has exited, and control returns to the scheduler.

### Local variables ###

A protothread function releases the stack every time it blocks, so **a local variable does not keep its value across `pt_yield()`, `pt_wait()`, or `pt_call()`.** Anything that must survive blocking belongs in the context structure. This is the most common mistake when writing protothreads. It comes in two forms, and the compiler catches only one of them.

**Declared after `pt_resume()`. The compiler catches this.** On resume, `pt_resume()` jumps straight to the resume point, skipping the declaration and initialization of `x`:
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
Compiling with `-Wall` catches it, pointing at both the use and the declaration. gcc says `warning: 'x' may be used uninitialized [-Wmaybe-uninitialized]`, and clang says `warning: variable 'x' is uninitialized when used here [-Wuninitialized]`.

This diagnostic used to require `-O2` or higher, because it depended on optimizer flow analysis. Current compilers report it at every optimization level, `-O0` included. (Verified with gcc 15.2 and clang 20 and 22 at `-O0`, `-O1`, `-O2`, and `-Os`.) You do need `-Wall` or `-Wextra`. The supplied `CMakeLists.txt` sets `-Wall`.

gcc's check is weaker for the most natural form of this mistake, a loop counter:
```
     pt_resume(c);
     for (int i = 0; i < 10; i++) {
         pt_yield(c);        /* i is lost here */
     }
```
gcc 15.2 warns about this only at `-O0`, while clang's `-Wall` catches it at every level. So even if you ship with gcc, building with clang now and then is recommended.

**Declared before `pt_resume()`. The compiler does not catch this.** Here the declaration isn't skipped. It re-runs on every entry, so the variable is silently reset each time the thread resumes:
```
     int x = 1;              /* re-initialized on every resume */
     pt_resume(c);

     x = 2;
     pt_yield(c);
     printf("x = %d\n", x);  /* prints 1, not 2 */
```
This compiles cleanly under `-Wall -Wextra` on both gcc and clang at every optimization level, which makes it the more dangerous of the two. If a local is assigned in one part of a protothread function and read after a block, move it into the context structure.

**The safe exception.** Initializing a local before `pt_resume()` is fine, and idiomatic, when its value is a pure function of the arguments or really is a constant, and is then used read-only. Re-running the initializer on each entry recomputes the same value. That is what the `c = env;` line does at the top of every top-level protothread function here (which is just our convention). For the same reason the initializer must have no side effects.

**Declare those locals `const`.** Read-only is a correctness requirement here, so let the compiler enforce it:
```
     int const x = 1;        /* not just `int x = 1;` */
     pt_resume(c);

     x = 2;                  /* error: assignment of read-only variable 'x' */
```
That converts the silent failure above into a compile error, in both gcc and clang, at any optimization level and with no warning flags needed. For a pointer the qualifier goes after the `*`, as in `context_t * const c = env;`. It's the pointer that must not change, not what it points at.

### Protothread function nesting ###

How does function nesting work? Suppose protothread function **A** calls protothread function **B** using `pt_call()`, and **B** wants to block with `pt_wait()`. **B** saves its current location into its context and returns `PT_I_WAIT` to the `pt_call()` in **A**. That saves the point where **A** calls **B** as **A**'s resume point, and **A** then returns `PT_I_WAIT` to its caller. When the scheduler resumes the thread, **A** runs, and its `pt_resume()` jumps to the call to **B**. So **A** calls **B**, and **B**'s `pt_resume()` jumps to just after where it had blocked and continues running. So the stack unwinds when the thread blocks, and "forward-winds" when it resumes. This is how the overall system still uses a single stack. This is also why the arguments that **A** passes to **B** should have no side effects. **A** calls **B** every time the thread is resumed.

It also means resuming costs one function call per level. A thread blocked N calls deep re-enters all N to get back to where it was. Measured, that adds about 3 ns per level to a context switch, from 8.6 ns for a thread blocked one call deep to 59 ns for one blocked seventeen deep (gcc `-O2`, on the benchmark machine).

When **B** finally finishes and returns `PT_DONE`, **A** knows to continue running following the `pt_call()` to **B**.

The context for function **A** can include **B**'s context structure within its own, or it can dynamically allocate **B**'s context. Allocating contexts is up to the user. To provide greater data hiding, **A** can allocate a very small structure with just the required `pt_func` field and a pointer to an opaque (to **A**) **B** context. The first thing **B** does is dynamically allocate its full context structure and link it from the small context that **A** passed.

If **A** calls **B**, and then calls **C** after **B** returns, **B** and **C** never run at the same time. So their contexts can be members of a `union` within **A**'s context. This sharing of memory between **B** and **C** reflects what happens within the stack of a POSIX thread.

### Wait channels ###

`pt_wait()`, `pt_signal()`, and `pt_broadcast()` work like pthread condition variable calls, without the `pthread_cond_t`. The names are taken straight from POSIX (`pthread_cond_wait()`, `pthread_cond_signal()`, `pthread_cond_broadcast()`), so they read as they do everywhere else, even though there's no condition variable to pass. The channel is any address, usually the address of the variable whose value you're waiting on. So there's nothing to declare, allocate, initialize, or destroy, and no bookkeeping about which condition variable belongs to which datum. This is the [`sleep()`/`wakeup()`](https://www.ques10.com/p/47397/sleep-event-and-addresses-in-unix-memory-managem-1/) interface of the early Unix kernels. I once made it work on a multiprocessor kernel, and compared it to semaphores, in [Process Synchronization in the UTS Kernel](https://static.usenix.org/publications/compsystems/1990/sum_ruane.pdf) (Computing Systems, 1990). Linux adopted this concept from the other direction. A `futex` is wait-and-wake on an address, and all Linux pthreads primitives are built on it.

There is no associated mutex because the scheduler is non-preemptive. `pthread_cond_wait()` needs one to make "test the predicate" and "suspend" a single indivisible step. Protothreads can't interleave (except explicitly), so that step is already indivisible. (But this problem can exist with interrupts; see [Lost wakeups](#lost-wakeups)).

**It is universal.** Every common blocking synchronization primitive can be built on top of it. The semaphores in `protothread_sem.h`, the reader-writer lock in `protothread_lock.h`, the sleeps in `protothread_timer.h`, and the queues in `protothread_queue.h` are all ordinary protothread code using `pt_wait()` and `pt_broadcast()`, with no privileged access to the scheduler. If you need a barrier or a latch, you write it the same way, in your own code, without patching the library.

**The mental model stays simple.** A protothread waits for a condition to be *true*, not for an event to *happen*. In electrical engineering, this is level-triggered versus edge-triggered. Level-triggered means a condition can be re-tested at any time. An event (edge-triggered) can be missed. So the simplest correct way to wait is to spin on the predicate:

```c
while (!ready) { }
```

Obviously right, and obviously wasteful. `pt_wait()` doesn't affect that logic. It inserts a _delay_, so the protothread stops consuming the CPU and is likely to find the condition true when it next looks:

```c
while (!ready) {
    pt_wait(c, &ready);
}
```

You can reason about the second in the same way as the first. That is also why the predicate is re-tested in a `while` and not an `if`. A wakeup means the condition *may* now be true, not that it is. There's a cost to an unnecessary wakeup, but careful coding (such as choosing channels appropriately) can minimize the overhead.

**Expect stray wakeups.** In the producer/consumer example above, an `if` would work, because there's exactly one producer and one consumer. If there were a second consumer, the `while` would be needed, because a consumer can be woken to find the other consumer has already emptied the mailbox. The same happens whenever unrelated conditions share a channel, say a structure with two conditions that become ready independently, signaled on the structure's address. So always re-test the specific condition you're waiting for.

It's okay for multiple conditions to map to the same channel, as in producer/consumer. But we could signal one channel when the mailbox becomes full, and a separate channel when the mailbox becomes empty. We could even use `c->mailbox + 1` for this additional channel, since this address isn't dereferenced (although this would look strange).

**Use `pt_broadcast()` by default.** It's what the Unix kernel's `wakeup()` always did, and with the `while` loops above it's always correct. Every waiter re-tests, those that can proceed do, and the rest go back to sleep. `pt_signal()`, which wakes only the oldest waiter, is an optimization with conditions attached, and they're easy to break. It's safe only when three things hold. Every waiter on the channel waits for the same condition. The woken waiter is certain to run (it can't be `pt_kill()`ed). And that waiter, on every path, either leaves the condition false or, if it leaves it true, signals again to pass the wakeup on. Those are properties of the whole program, not of the line where `pt_signal()` is called. So a signal that's correct today can become a hang when someone later adds a different kind of waiter to the same channel.

Broadcasting wakes waiters that then go back to sleep, in proportion to how many are waiting on the one channel. On a microcontroller, with protothreads numbering in the tens and seldom more than a few sharing a channel, that's negligible. Where it does matter, consider splitting the channel (making it finer-grained). A channel is only an address, so each condition can have its own, `&s->a` and `&s->b` rather than `&s`. When a channel has exactly one waiter, signal and broadcast do the same thing. This library's lock, timers, and queues split their channels this way.

The debug (testing) define `PT_SIGNAL_WAKES_ALL` checks the other direction. If set to `1`, every `pt_signal()` wakes everyone. Correct code still works, more slowly. This library's test suite and demos enable that in CI, and its own semaphores, locks, timers, and queues don't call `pt_signal()` at all.

The analogy has one limitation. A real spin loop can never miss anything, because it re-tests continuously. `pt_wait()`'s delay ends only when somebody signals, so a missing signal causes a hang. A missing signal could be a plain logic error, or it could come from a timing window (see [Lost wakeups](#lost-wakeups)).

In a large, mission-critical system, consider calling `pt_broadcast()` on _every_ channel periodically. A missed-wakeup bug then causes a delay instead of a hang.

## Deterministic execution ##

When multiple POSIX threads are ready to run, the scheduler runs them in a random, or at least unpredictable, order, even if there's a single processor. After choosing a thread to run, it may be preempted at any time, with execution resuming on another thread (timeslicing). In contrast, an application built on protothreads, like event-driven software, runs deterministically if all other events are deterministic too. This allows one to write pseudo-random tests that can reliably reproduce bugs. You start the test with a randomly chosen random number generator seed (based on wall-clock time, for example). If a bug is found during the run, you can start the test again with the same seed, perhaps with more tracing or new assertions to catch the problem earlier. The test follows the same sequence of states, so it reproduces the bug. This often lets you verify a proposed fix too. That doesn't work if the fix changes the execution sequence enough to invalidate the seed, but sometimes you can "timebomb" the fix, making it active only just before the failure.

The entire environment must be carefully controlled so that no nondeterminism can sneak into the system. The random number generator should be used for anything that is non-deterministic in the real system. This usually requires making every external interface mockable (the network, the disk, the clock), which constrains the entire design. For example, if the code makes any decisions based on real time, such as a timeout, then the clock function must be a wrapper that returns real time in production but simulated time during the test. You can't just bolt on a test harness afterwards. So, although protothreads by itself doesn't give you this kind of reproducible testability, it contributes one important piece of this requirement.

By the way, a good way to simulate time is to keep future events, such as a network or disk completion, in a priority queue (a heap). The root is the event with the smallest, most imminent, timestamp. When there's nothing more to do at the current time, instantaneously advance simulated time to that timestamp.

With POSIX threads this is much harder to arrange, because the thread scheduler is outside the test program's control and in my experience makes decisions that vary from run to run. It isn't impossible, though. Determinism can be imposed from the outside by a replacement runtime such as [DThreads](https://plasma.cs.umass.edu/emery/dthreads.html) or [Parrot](https://sigops.org/s/conferences/sosp/2013/papers/p388-cui.pdf), which serialize thread synchronization into a fixed order. Or it can come from a supervising tool such as [rr](https://rr-project.org/) (record and deterministic replay) or [Hermit](https://github.com/facebookexperimental/hermit) (a deterministic sandbox that also controls time, thread interleaving, and randomness). But each involves an external mechanism and a slowdown, and the deterministic run is not the run you ship. Protothreads are deterministic by construction, and the schedule you debug is the schedule that runs in production.

The real system programming experience I had in mind writing this library was SAN/iQ, the distributed storage product by LeftHand Networks (later acquired by HP). It had many concurrent activities, a protocol that had to be correct under adversarial timing, and failures that surfaced once in hours. Nodes agreed on a shared configuration through a Paxos-style protocol and had to stay in sync while replicating data.

Its test, `dbd_test`, was a single self-contained executable running many simulated nodes in one address space over a simulated network, using simulated disks, on simulated time. It dropped messages at random, delayed and reordered them, failed disk requests, and crashed nodes and brought them back. All of it was driven by one global pseudo-random generator, seeded from the wall clock and printed at startup. A rack of machines ran independent instances of this test around the clock. When a run asserted, that seed reproduced it exactly, so you could go back in with tracing, a debugger, and additional auditing code. Failing seeds were kept like gold. The runs were deliberately time-boxed, around twenty minutes (a bug that takes two days to appear also takes two days to reproduce).

None of that is specific to `dbd_test`. It's an application of a broader habit, designing software to be easy to test in the first place, which pays off in development at least as much as in testing. [The Paranoid Programmer](https://docs.google.com/presentation/d/1WDzRvQyKbM19SXiXJ8d7A7f5xR27AbDyf5KLKdlea2A/edit?usp=sharing), by Dr. Mark Hayden, chief software architect at LeftHand Networks (with minor additions of mine), makes that case from ten years of practice with little or no QA team behind him.

What SAN/iQ and `dbd_test` were doing now has a name, *deterministic simulation testing*. It is not quite fuzzing, in that the randomness is in the environment and the schedule rather than in the inputs to one component, and it covers a whole system rather than narrowing onto a function. The best-known example is FoundationDB. It built [Flow](https://apple.github.io/foundationdb/engineering.html), an extension giving C++ single-threaded, actor-based concurrency, so that [an entire cluster could run deterministically in a single-threaded process](https://apple.github.io/foundationdb/testing.html). The network, the disk, the clock, and the random number generator are all replaced by shims. A failing seed replays the run down to the scheduling of individual actors. Whole-system simulation and cooperative single-threaded scheduling keep showing up together, because the second is what makes the first possible.

Protothreads came too late for SAN/iQ, whose event loop was already written and heavily optimized. But that system is the kind of thing this library is for. Watching that architecture be built by hand is why this implementation has nesting, wait channels, and a real scheduler.

## Protothreads on a multi-core system ##

All protothreads in one `protothread_t` run on a single OS thread, one at a time. That doesn't limit protothreads to single-core machines. Parallel *computation* can be combined with protothreads' concurrent *control flow*.

The usual arrangement is a pool of POSIX threads doing the heavy lifting on every core, with protothreads as the coordination layer. A protothread submits a work item, blocks, and resumes when the result is ready; other protothreads run in the meantime. N cores stay busy on the computation while one OS thread runs thousands of protothreads that handle the sequencing, retries, timeouts, and state.

A worker thread must not wake a protothread by calling `pt_signal()` itself, for the reason given under [Lost wakeups](#lost-wakeups). It should post the finished work somewhere, and the thread that owns `protothread_run()` can see that and issue a `pt_signal()` between runs.

Another arrangement is one `protothread_t` per OS thread. The library has no global state, so the instances don't interfere and need no locking. Protothreads in different instances that share data must coordinate like any other OS threads. They also can't `pt_signal()` each other directly, for the same reason a worker thread can't.

The [`demo/`](demo) directory has three complete working programs, one for each way of combining protothreads with the outside world:

| program | what it shows |
| --- | --- |
| [`demo/pool.c`](demo/pool.c) | 500 protothreads over a fixed pool of 4 worker threads (the arrangement described above) |
| [`demo/async_io.c`](demo/async_io.c) | concurrent asynchronous I/O and no POSIX threads. One `poll()` loop turns completions into signals. `./build/ptaio -v` traces every submission and completion, which come out interleaved |
| [`demo/helper_thread.c`](demo/helper_thread.c) | one throwaway POSIX thread per blocking call, created and joined by the protothread that needs it |

They are not built by default, since two of the three need pthreads and the library itself does not:

```
cmake -S . -B build -DPROTOTHREAD_DEMOS=ON
cmake --build build
./build/ptpool && ./build/ptaio && ./build/pthelper
```

### Blocking system calls ###

A protothread that makes a blocking system call stops **every** protothread, because they all share one thread and one stack. `pt_wait()` isn't a system call and doesn't do this, but `read()`, `connect()`, and `fsync()` do.

An event loop also has this constraint, and the usual solutions are non-blocking I/O ([`demo/async_io.c`](demo/async_io.c)) or handing the call to another thread.

There are at least three ways to hand work to a separate (OS) thread. The first is to create a thread when needed, and let it exit when the work is done (see the [helper thread demo](demo/helper_thread.c)). This is a good option when the call blocks, has no asynchronous form, and happens rarely. There's no pool, no work queue, and no shutdown protocol. The second option, a pool, is right when the offloaded work is constant and hot, or when there are far more protothreads than you would want threads (see the [thread pool demo](demo/pool.c)). A third option that combines the previous two is a dedicated helper thread per protothread, created at startup and parked between calls. It costs about 8 kB of resident memory per parked helper, plus 56 bytes for the protothread itself. The advantage is that no protothread ever queues behind another waiting for a free worker. Each helper can also hold state across calls, such as a connection, an open descriptor, or a thread-bound library handle that a stateless pool worker can't keep. For some synchronous libraries that makes it a requirement, not an optimization.

In all three the protothreads themselves still share one thread. Giving each protothread a thread to *run on* doesn't work. That's just POSIX threads with extra steps.

But blocking is often *fine*. If the call is rare and short, like reading a configuration file at startup or an occasional log flush, other protothreads wait a few milliseconds. Building a thread pool to avoid that may not be worth it. What to avoid is a blocking call on a hot path. It reduces a system designed to handle thousands of concurrent activities to one at a time.

## Memory overhead and performance (benchmarks) ##

The best-known implementation of protothreads (by Adam Dunkels) uses just two bytes per protothread. This implementation is not quite so parsimonious, mainly because it includes a scheduler. Threads are on either the wait or the run list, and that costs pointers. In exchange you get nesting, wait channels, and synchronization primitives.

Each protothread function context has a `pt_func_t` structure, which is 2 pointers. Each overall protothread requires a `pt_thread_t` structure, which is 5 pointers. Measured with `PT_DEBUG=0`:

| | 32-bit | 64-bit |
|---|---|---|
| `pt_func_t` (per nesting level) | 8 | 16 |
| `pt_thread_t` (per protothread) | 20 | 40 |
| **minimum RAM per protothread** | **28** | **56** |
| `protothread_t` state, `PT_NWAIT=1` | 20 | 40 |
| `protothread_t` state, default `PT_NWAIT` | 4112 | 8224 |

Add roughly 8 bytes of real C stack per level of `pt_call()` nesting. A protothread is stackless between waits, but while it's running, a chain of `pt_call()`s is an ordinary chain of C calls.

For comparison, an RTOS task typically needs a control block of about 90 bytes plus a stack of at least several hundred bytes, so a protothread is on the order of twenty times cheaper.

You also don't have to guess stack sizes. Sizing a stack for a POSIX thread or an RTOS task means predicting its worst-case depth (so the stacks don't run into each other), which depends on the compiler, the optimization level, inlining decisions, and the ABI. A debug build generally needs more stack than the optimized build the size was measured against, so the `-O0` binary you are stepping through can overflow a stack even if that can't happen in production. Guessing high causes waste proportional to the number of threads. Guessing low can cause intermittent corruption that is hard to attribute to the thread that caused it.

Protothreads come close to abolishing the question, because with a single stack there's usually nothing to pre-size. In the classic layout the stack grows down from the top of RAM, while the heap, if there is one, grows up from the end of static data. The free space between them belongs to whichever needs it at the moment. Many vendor linker scripts still name a minimum stack size, but only to check at link time that enough RAM is left over. As soon as there's a second stack, that stops working. Each one needs a region of its own, fixed in advance at its worst case, and space one isn't using can't help the other.

Everything that has to survive blocking lives in the context structure instead, and the compiler tells you how big that is with `sizeof`. You still need to know the depth of the deepest `pt_call()` chain, plus whatever your interrupt handlers need. On a microcontroller without an MPU, a stack that runs into the heap is rarely detected. gcc's `-fstack-usage` reports each function's frame.

`PT_DEBUG` adds four fields to `pt_func_t` and one to `pt_thread_t` for the debugger macros, raising the minimum per-protothread cost from 56 to 96 bytes on a 64-bit target. It's on by default, so turn it off in production builds.

### Benchmarks ###

`protothread_bench.c` is a benchmark that runs the same workloads twice, once with protothreads and once with POSIX threads, so you can reproduce these numbers on your own hardware. It is not built by default, because unlike the library it needs pthreads:

```
cmake -S . -B build -DPROTOTHREAD_BENCH=ON
cmake --build build
./build/ptbench
```

On the machine this was written on:

| | protothread | pthread | ratio |
|---|---|---|---|
| context switch | 4.7 ns | 3,241 ns | **690x** |
| create + destroy | 2.8 ns | 28,852 ns | **10,300x** |
| memory per thread | 56 bytes | 16,384 bytes | **293x** |

The context switch benchmark is two threads handing a token back and forth a million times. The protothread side uses `pt_wait`/`pt_signal`, and the pthread side uses a mutex and one condition variable per thread. Creation is a thread that does nothing, created and reaped. Both are checked for linear scaling across two orders of magnitude, so the compiler is demonstrably not optimizing the work away.

The absolute numbers vary by the machine, the kernel, and its speculative-execution mitigations. The pthread side moves the most, so only the orders of magnitude mean much.

**This is not an apples-to-apples comparison.** POSIX threads give you preemption and real parallelism across cores; protothreads give you neither. These kinds of benchmarks also overstate the difference, because a real system does a lot more work between context switches, making the thread-specific overhead a smaller fraction of what's being done.

## Versioning ##

This project uses [semantic versioning](https://semver.org). The version lives in `protothread.h`, and the build reads it from there. So a header copied into your own tree can always identify itself:

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
  * **The headers no longer include `<string.h>`, and include `<assert.h>` and `<stdlib.h>` only conditionally**: `<assert.h>` while `PT_DEBUG` is on, and `<stdlib.h>` unless `PT_NO_MALLOC` is defined. If your code relied on getting `memset`, `assert`, or `malloc` transitively from `protothread.h`, include them yourself. Watch for `assert` in particular. Code that relies on it can build with the default `PT_DEBUG` and then fail in a production build with `PT_DEBUG=0`. This is what buys the freestanding property.
  * **`pt_set_atexit()` has been removed.** Do any cleanup yourself once `pt_kill()` has found the thread. `if (pt_kill(&c->pt_thread)) cleanup(c);` does exactly what the callback did, and every protothread is a pointer smaller.
  * **A protothread may no longer free the storage holding its own `pt_thread_t`.** The scheduler records that a thread has exited after its function returns, so freeing that storage from inside the thread is now a use-after-free. Free it from whoever owns it instead, after `pt_join()` or once `pt_is_alive()` is false. Freeing a nested `pt_call()` context from inside that call is unaffected. Sanitizers don't reliably catch violations.
  * **Internal names now have a `pt_i_` or `PT_I_` prefix**, so that any name without one is public API. Several were visible in the 1.x headers: `pt_wake()`, `pt_get_protothread()`, `pt_create_thread()`, and the other scheduler internals; `PT_WAIT`, `PT_RETURN_WAIT`, and `PT_RETURN_DONE`; the lock's `PT_LOCK_READ` family; and the `_f` functions behind `pt_sem_acquire()` and the lock macros. `state_t` is gone; use `protothread_t`, which is the same type. The reserved struct tags `_pt_sem_env_t`, `_pt_lock_env_t`, and `_pt_lock_t` are now `pt_sem_env_s`, `pt_lock_env_s`, and `pt_lock_s`. Code that uses only the documented API is unaffected.
  * **Reader-writer locks are now FIFO.** Requests are granted in arrival order, so a stream of readers can no longer starve a waiting writer.
  * **The license changed from Apache-2.0 to MIT.**

Everything else is unchanged, including `pt_wait`, `pt_yield`, `pt_call`, `pt_create`, `pt_signal`, `pt_broadcast`, and the context structure layout.

## Configuration ##

All configuration is by preprocessor macro. Because the library is header-only, define these on the compiler command line (`-DPT_DEBUG=0`) so that every translation unit agrees.

> `PT_DEBUG` (default `1`)
>
> Enables internal assertions and the bookkeeping the gdb macros use to print protothread stack traces. Set to `0` for production builds. **This changes the layout of `pt_thread_t` and `pt_func_t`**, so it must be the same for your whole program.

> `PT_NWAIT` (default `1024`)
>
> Number of wait queues, a power of 2. Waiting threads are hashed onto this table by channel address. Each entry is one pointer, so the default costs 8 kB per `protothread_t` on a 64-bit machine. **Set `PT_NWAIT=1` on a memory-constrained system.** With only a handful of waiters, one linear wait list is both smaller and faster than hashing.

> `PT_NO_MALLOC`
>
> Define this to drop `<stdlib.h>`, `protothread_create()`, and `protothread_free()`. Use `protothread_init()` on statically allocated storage instead.

> `pt_assert(condition)`
>
> Define your own before including `protothread.h` to avoid `<assert.h>` entirely. By default it is `assert()` when `PT_DEBUG` is set, and a no-op (that still type-checks the expression) otherwise.

> `PT_CRITICAL_T`, `PT_CRITICAL_ENTER()`, `PT_CRITICAL_EXIT(saved)`
>
> Mutual exclusion against interrupt handlers. See [Interrupt safety](#interrupt-safety). No-ops by default.

> `PT_CRITICAL_ASSERT()`
>
> Checks that the caller really is in a critical section where the scheduler requires it. Compiles to nothing by default. CI defines it against a depth counter, so that a change which starts touching a list from thread context fails loudly instead of silently. You can define it on a real target the same way.

> `PT_SIGNAL_WAKES_ALL` (default `0`)
>
> Set to `1` to make `pt_signal()` wake every waiter on the channel, exactly as `pt_broadcast()` does. Correct code must still work, since a wakeup is only ever a hint, so this is a test of a design rather than a mode to ship. CI builds the test suite and the demos this way. See [Wait channels](#wait-channels).

### Defining the critical-section macros ###

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

The critical sections are short and O(1), except that `pt_signal()`, `pt_broadcast()`, and `pt_kill()` walk one wait list, and `pt_sleep()` and `pt_timer_cancel()` walk the timer list. If interrupt latency is critical, keep `PT_NWAIT` large enough that wait lists stay short, or have the interrupt handler set a flag that the main loop turns into a `pt_signal()` at a safe point.

## Compiler requirements ##

This implementation requires the gcc [labels-as-values](http://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html) extension (`&&label` and `goto *ptr`), so it needs **gcc or clang**. That includes the usual microcontroller toolchains: `arm-none-eabi-gcc`, `armclang`, `avr-gcc`, `msp430-gcc`, and the RISC-V ones. It does not work with IAR or ARMCC, which don't support computed goto. For those compilers, use Dunkels' `switch`-based implementation instead.

Apart from that extension, and `__COUNTER__` for naming the labels, which every gcc and clang also provides, the code is ordinary C. CI builds and runs the test suite on gcc and clang, on Linux and macOS, with `-Wall -Wextra -Werror`. It covers `PT_DEBUG` and `NDEBUG` on and off, `PT_NWAIT` of 1, 4, and 1024, `-O0` through `-Os`, `-std=c99` through `-std=c23`, and AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer. It also asserts that a freestanding build still needs no libc symbols at all, since one careless `#include` would quietly break that.

Each blocking macro names its label with a fresh `__COUNTER__` value, so labels never collide. Several blocking macros can share a line, and a macro of your own can contain more than one. Earlier versions named them after `__LINE__`, which made both of those a `duplicate label` compile error. A protothread function's blocking macros must still all be in that function, since a label belongs to the function it's in.

`pt_resume()` contains a dead address-of-label expression, because clang rejects an indirect `goto` in a function with no address-of-label at all. Without it, a protothread function that never blocks, and so has no `pt_wait()`, `pt_yield()`, or `pt_call()` to supply one, fails to compile. It costs nothing, and the generated code is byte-for-byte identical.

### Using it from C++ ###

The headers compile as C++ as well as C. CI builds and runs a real protothread under `g++` and `clang++` at `-std=c++11`, `c++17`, and `c++20`, covering yields, nesting through the semaphore, lock, and timer helpers, queues, and the computed goto.

Existing C code does not port unchanged, though. Every top-level protothread function (the ones passed to `pt_create()`) starts by recovering its context:

```c
ctx_t * const c = env;
```

C++ won't convert `void *` implicitly, so each one needs a cast. `(ctx_t *)env` works in both languages, and `static_cast<ctx_t *>(env)` in C++ only. Only top-level functions need that. A nested function reached through `pt_call()` can declare its context parameter with its real type, which needs no cast in either language and is better C besides. So in existing code written the other way, the fix for a nested function is to type its parameter, not to cast it. (One other C/C++ difference to watch for while porting is that a `struct` tag declared inside another `struct` is visible at file scope in C, but scoped to the enclosing class in C++.)

`clang++` rejects an initialized local declared after `pt_resume()` outright:

```
error: cannot jump from this indirect goto statement to one of its possible targets
```

That's because C++ forbids jumping into the scope of a variable with an initializer. That promotes the bug described under [Local variables](#local-variables) from a warning to a hard error. It doesn't catch the silent case, a variable initialized *before* `pt_resume()` and re-initialized on every resume, so declaring those `const` remains the only thing that catches it. `g++` only warns where `clang++` errors, so the same source can build under one and fail under the other.

None of this makes protothreads idiomatic C++. There's no RAII and no type-safe context, and for new C++ code, C\+\+20 coroutines are the native answer. What the headers offer a C++ project is the same scheduler, usable from C++ translation units, which mostly matters when C++ and C code need to share one protothread scheduler.

## Conclusion ##

For many resource-constrained or real-time applications, protothreads are faster and use less memory than POSIX threads, and algorithms can be expressed more clearly than with the event-driven model.

## API Reference ##

### Inside a protothread function ###

These are macros, designed to look and act like function calls. Their first argument is a pointer to a user-defined context structure, `c`. (Here it's called `context_t`, but the name is up to you.) The type `pt_f_t` is a pointer to a protothread function.

> `void pt_resume(struct context_t *c)`
>
> Every thread function must call this macro first, after initializing any local variables. Those must be a function only of the arguments and each other, not of any global state (see [Local variables](#local-variables)). If the thread is being resumed, `pt_resume()` causes it to `goto` the resumption point, which is where this function last blocked. If the function is being called for the first time (that is, the thread is not being resumed), `pt_resume()` has no effect.

> `void pt_wait(struct context_t *c, void *channel)`
>
> Block until a signal is sent to the given channel. The channel is an arbitrary `void *` value, usually the address of a data structure whose state change the thread is interested in. A channel itself has no state. The protothread system never dereferences it. Typically, after this function returns the condition being waited for is re-evaluated. Analogous to [POSIX pthread\_cond\_wait()](http://www.opengroup.org/onlinepubs/009695399/functions/pthread_cond_wait.html).

> `void pt_wait_until(struct context_t *c, void *channel, cond)`
>
> Block until `cond` is true, testing it and enqueueing on `channel` in one critical section. That's the difference from `while (!cond) pt_wait(c, channel)`. There, an interrupt handler that makes `cond` true and signals in between is missed, and the protothread blocks forever (see [Lost wakeups](#lost-wakeups)). `cond` is evaluated more than once, so it must have no side effects. Between protothreads the two forms are equivalent, since nothing runs in between. Dunkels' `PT_WAIT_UNTIL` is different. It re-tests its condition whenever the application calls the protothread. This one re-tests only when `channel` is signaled, so whatever makes `cond` true must also signal it.

> `void pt_yield(struct context_t *c)`
>
> Reschedule the current thread and release the CPU. It is like `pt_wait()` on a channel that is immediately signaled. The current thread queues itself behind all ready threads and returns control to the scheduler.

> `void pt_call(struct context_t *c, pt_f_t child_func, struct child_context_t *child_context, arg...)`
>
> Immediately call the given protothread function, passing it the given environment and arguments, and wait for it to return. There can be no context switch between the start of this statement and the start of the child function. Be careful that argument evaluation has no side effects, since this call occurs every time the thread is resumed. The usual C compile-time type checking is performed on all arguments.

> `bool_t pt_call_waited(struct context_t *c)`
>
> Returns TRUE if the most recent `pt_call()` blocked (either directly in the called function, or in a function that it called, recursively). If function **A** calls **B** and **B** blocks, then when it finally returns to **A**, it's sometimes helpful for **A** to know that other threads might have run, so it should reevaluate the state of the world. But if **B** didn't block, then **A** knows that only a limited change of state (namely, whatever **B** might do) could have occurred. This works after acquiring a semaphore or a lock too, since those are `pt_call()`s. See [Built on top](#built-on-top-semaphores-timers-locks-and-queues) for an example.

> `void pt_reset(struct context_t *c)`
>
> Forget this function's saved resume point, so that the next time it runs it starts from the top rather than from where it last blocked. Useful to restart a protothread function, or to reuse a context structure.

> `void pt_join(struct context_t *c, pt_thread_t *thread)`
>
> Block until the given protothread has exited or been killed, returning at once if it already has. Several protothreads may join the same one. The target's `pt_thread_t` must remain allocated until every joiner has returned, which is why a protothread may not free the storage holding its own. Joining yourself is a deadlock, and is caught by an assertion in `PT_DEBUG` builds. Analogous to [POSIX pthread\_join()](http://pubs.opengroup.org/onlinepubs/009695399/functions/pthread_join.html), except that there's no return value to collect. Results live in the context, which the joiner already owns.

> `protothread_t pt_get_pt(struct context_t *c)`
>
> This returns the protothread object handle (`protothread_t`). It is a convenience that allows code in a thread context to call API functions that require a protothread object argument, such as `pt_create()` or `pt_broadcast()`.

### Creating, waking, and killing protothreads ###

Call these from the OS thread that runs `protothread_run()`, either inside a protothread or in the scheduler loop between runs. From another OS thread or an interrupt handler, they race with the scheduler. Its lists can be corrupted unless the `PT_CRITICAL_*` macros are defined. Even then, `pt_signal()` and `pt_broadcast()` can lose a wakeup, and `pt_kill()` can't know whether its target is running at that moment. See [Interrupt safety](#interrupt-safety) and [Lost wakeups](#lost-wakeups).

What matters is overlap, not which thread. Before the scheduler first runs there's nothing to race with, so a setup thread may create protothreads and then hand over.

> `void pt_create(protothread_t, pt_thread_t *, pt_f_t func, void *env)`
>
> Schedule the given protothread function to run, passing it the given environment. This function becomes the top-level function of the thread. There is no context break between this call and the caller's next statement. The new thread queues behind all ready threads. Analogous to [POSIX pthread\_create()](http://www.opengroup.org/onlinepubs/009695399/functions/pthread_create.html).

> `void pt_broadcast(protothread_t, void *channel)`
>
> Send a signal to the given channel, which wakes up (schedules) all threads waiting on the channel to run in the same order they blocked. If there are no threads waiting, this call has no effect. The signal isn't queued (a channel has no memory). These threads queue behind all ready threads. Analogous to [POSIX pthread\_cond\_broadcast()](http://www.opengroup.org/onlinepubs/009695399/functions/pthread_cond_broadcast.html).

> `void pt_signal(protothread_t, void *channel)`
>
> Same as `pt_broadcast()` but wakes up only one (the oldest) waiting thread. That's safe only under conditions that are easy to break, so prefer `pt_broadcast()` (see [Wait channels](#wait-channels)). Analogous to [POSIX pthread\_cond\_signal()](http://www.opengroup.org/onlinepubs/009695399/functions/pthread_cond_signal.html).

> `bool_t pt_is_alive(pt_thread_t const *)`
>
> Returns TRUE from creation until the thread's top-level function has returned or `pt_kill()` has removed it. Never blocks, so it can be called from anywhere, including from outside a protothread. The `pt_thread_t` must still exist.

> `bool_t pt_kill(pt_thread_t *)`
>
> Remove a thread from whatever list it is on, so that it is never scheduled again. Returns TRUE if the thread was found (it is not an error to kill a thread that has already exited). This is dangerous unless the thread was written to expect it. The thread is stopped wherever it happens to be blocked, and any resources it holds (allocated contexts, semaphores, locks) aren't released. Two places it can be blocked need something done first. A sleeping thread must be taken off the timer list with `pt_timer_cancel()`. A thread that might be using a reader-writer lock must be taken out of it with `pt_lock_cancel()`, whether it's queued for the lock or holding it. Otherwise the lock is eventually granted to a thread that never releases it, and every later request waits forever. A thread waiting on a semaphore is safe to kill. Do not call this on the currently running thread. Any cleanup is up to the caller, once this has returned TRUE.

### Protothread system setup and teardown ###

None of these schedule or wake a protothread, so none has the lost-wakeup hazard. They're called while no protothread is running, before the first `protothread_run()` or after the last. So they may come from a different OS thread than the scheduler's. A setup thread can initialize everything and exit before the scheduler thread starts.

> `protothread_t protothread_create(void)`
>
> This is usually only called once to create the overall protothread object. It returns the protothread handle (or NULL if allocation fails). The protothread system uses no global variables. All protothread state is within this object, so multiple instances are independent (see [Protothreads on a multi-core system](#protothreads-on-a-multi-core-system)). This is the only protothread API function that allocates memory, and it can be compiled out with `PT_NO_MALLOC`.

> `void protothread_free(protothread_t)`
>
> Free the state allocated with `protothread_create()`. There must be no threads associated with this object.

> `void protothread_init(protothread_t)`
>
> Initialize a `struct protothread_s` that you allocated yourself, statically or otherwise. This is the alternative to `protothread_create()` on systems with no heap.

> `void protothread_deinit(protothread_t)`
>
> The counterpart to `protothread_init()`. It frees nothing; when `PT_DEBUG` is set it asserts that no threads remain on any list.

### Scheduling ###

> `bool_t protothread_run(protothread_t)`
>
> Run the next ready thread (if there is one). Returns TRUE if there remains at least one thread ready to run (more work to do).
>
> **Never call this from an interrupt handler**, even with the `PT_CRITICAL_*` macros defined. Those make the scheduler's *lists* safe against interrupts. Nothing can make `protothread_run()` safe, because it runs your thread code. An interrupt arriving while a protothread is running and calling `protothread_run()` would re-enter the scheduler and start a second thread on top of the first. When `PT_DEBUG` is set, the `pt_assert(s->running == NULL)` at the top of `protothread_run()` catches this.

> `void protothread_set_ready_function(protothread_t, void (*ready_function)(void *), void *env)`
>
> This function lets you use protothreads with an existing scheduler (that you can't or don't want to modify). You don't need this function if you are providing your own scheduler. This function is usually called once during initialization. The protothreads system then calls `ready_function`, passing it `env`, when a thread becomes ready while none were ready and no thread is running. You can pass NULL for `ready_function` to disable this feature.
>
> The given `ready_function` generally schedules (using whatever method is available on your system) another function that calls `protothread_run()` repeatedly until there are no more threads to run (`protothread_run()` returns FALSE). The `ready_function` should not call `protothread_run()` directly.
>
> If an interrupt handler signals a protothread, `ready_function` is called from that interrupt context. The library calls it outside its own critical section, so interrupts are at whatever level the handler is running at, not masked. Keep it short. The rule above becomes a hard requirement there. Scheduling the runner is fine, but calling `protothread_run()` is not.
>
> To prevent a sequence of protothread executions from holding onto the CPU for too long, the function can limit the number of times it calls `protothread_run()`. For example, it may run no more than 20 threads before returning to the main scheduler, to let other things (outside of protothreads) run. But if it does so (if the last call to `protothread_run()` returns TRUE), it should reschedule itself because there is still work to do.

## Built on top: semaphores, timers, locks, and queues ##

None of this is needed to use protothreads, and none of it is part of the core. Each header is ordinary protothread code over `pt_wait()` and `pt_broadcast()`, with no privileged access to the scheduler. They're closer in spirit to the programs in [`demo/`](demo) than to the API above, and they show how to build primitives of your own. They are tested and maintained like the rest of the library.

Every semaphore and lock acquire is a `pt_call()` underneath, so [`pt_call_waited()`](#inside-a-protothread-function) tells you afterwards whether it blocked. If it didn't block, no other protothread has run since you asked, so whatever you looked at beforehand is still exactly as you saw it. If it did, anything could have changed while you waited, and you need to check again. That's the usual shape of code that finds something and then locks it:

```c
c->e = cache_lookup(c->key);
pt_lock_acquire_write(c, &c->lock_env, &c->e->lock);
if (pt_call_waited(c) && c->e->key != c->key) {
    /* reused for another key while we waited; release it and start over */
}
```

In the common, uncontended case the lookup is trusted without a second look.

### Semaphores ###

`protothread_sem.h` provides counting semaphores. They're deliberately not fair, to avoid convoys. See [doc/sem.md](doc/sem.md).

### Timers ###

`protothread_timer.h` provides `pt_sleep()`, driven by whatever clock you already have, so the library never reads one. It's safe to drive from a tick interrupt, and wraparound is handled. See [doc/timer.md](doc/timer.md), which includes a bare-metal idle loop.

### Reader-writer locks ###

`protothread_lock.h` provides reader-writer locks. Requests are granted in arrival order, so writers don't starve. See [doc/lock.md](doc/lock.md), including why there's no upgrade call.

### Message queues ###

`protothread_queue.h` provides type-safe, fixed-capacity message queues in storage you supply, what Go (Golang) calls channels. `try_send()` is safe from an interrupt handler. There's no `select`, and [doc/queue.md](doc/queue.md) shows what to do instead (fan-in, timeouts, selective receive, and queues of queues).

## License ##

MIT. See [LICENSE](LICENSE).

The one exception is `FindPROTOTHREAD.cmake`, which is third-party code by Ryan Pavlik (Iowa State University) under the Boost Software License 1.0. Its notice is preserved in the file. Both licenses are permissive and GPL-compatible. Every file carries an SPDX identifier.

## References and Acknowledgements ##

[Wikipedia protothreads](http://en.wikipedia.org/wiki/Protothreads)

[POSIX thread reference](http://pubs.opengroup.org/onlinepubs/9699919799/basedefs/pthread.h.html)

Lawrence M. Ruane, [Process Synchronization in the UTS Kernel](https://static.usenix.org/publications/compsystems/1990/sum_ruane.pdf), Computing Systems 3(3), Summer 1990, pp. 387-421. Background for the wait-channel design, broadcast wakeups, and semaphore convoys.

I wish to gratefully acknowledge Adam Dunkels (with support from Oliver Schmidt) for inventing this brilliant idea. Please see his [web site](http://dunkels.com/adam/pt/).

My thanks to Paul Soulier for introducing me to the concept of protothreads, and to Marshall McMullen and John Rockenfeller for reviewing drafts of this article.

## Contact and contributing ##

The project lives at [github.com/LarryRuane/protothread](https://github.com/LarryRuane/protothread). [TODO.md](TODO.md) lists ideas and deferred work, if you are looking for somewhere to start.

Bug reports, questions, and pull requests are welcome as [GitHub issues](https://github.com/LarryRuane/protothread/issues). Please prefer those to email, so other people can find the answers. If you work with AI tools, please read [doc/AI_POLICY.md](doc/AI_POLICY.md) first. They're welcome, and were used here, but a person has to choose the work, understand it, and answer for it. Otherwise, you can email _LarryRuane@gmail.com_, or try [@LarryRuane](https://x.com/LarryRuane) on X if email doesn't reach me.

### Tips ###

If this saved you some time and you'd like to say so, tips are welcome, but entirely optional.

Lightning: `larryruane@speed.app`. Any amount from one satoshi works, and most wallets will let you attach a short message.

On-chain Bitcoin: send me a message and I'll give you an address. I would rather hand out a fresh one than publish a single address for everyone to reuse.

