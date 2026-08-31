# TODO

Ideas and deferred work, roughly in order of how much they would be worth.

## C++ support ##

Many embedded projects are C++ now, and the payoff would be large: a protothread
that can use `std::vector`, `std::map`, `std::string` and the rest of the
non-allocating-until-you-ask standard library is a much more comfortable place to
write a state machine.

Investigated; it is closer than expected. With `g++ 15 -std=c++17 -Wall -Wextra`:

  * **Labels-as-values works in g++ and clang++**, so the core technique ports.
    (IAR and ARMCC still do not support it, in C or C++.)
  * **All four headers already compile as C++** with no errors.
  * A protothread whose context structure contains a `std::vector` and a
    `std::map` compiles, runs and produces correct results today.

Two things to fix or document:

1. **One real blocker.** `protothread_create()` does
   `state_t const s = malloc(sizeof(*s))`, and C++ has no implicit conversion
   from `void *`. A cast fixes it for both languages:
   `state_t const s = (state_t)malloc(sizeof(*s))`.
   Everything else builds clean, and `PT_NO_MALLOC` sidesteps it entirely.

2. **One silent trap, and it needs a loud warning in the README.** The existing C
   rule -- a protothread function cannot keep state in a local across a wait --
   becomes undefined behavior in C++ rather than merely a wrong value, because
   `pt_resume()` jumps over constructors while destructors still run at scope
   exit. A local `Noisy` object around a `pt_yield()` produces:

        ctor 42        <- first pass
        dtor 42        <- pt_yield() returns from the function
        dtor 0         <- resumed pass destructs an object never constructed

   One construction, two destructions, the second on garbage. g++ accepts this
   with only `-Wmaybe-uninitialized`. With a real `std::vector` it is heap
   corruption. The rule is unchanged from C -- put state in the context
   structure, where it works fine -- but the consequence of breaking it is much
   worse, so it should be stated explicitly.

3. **Exceptions wedge the scheduler.** If an exception propagates out of a
   protothread function, `protothread_run()` does not catch it, and
   `s->running` is left non-NULL forever:

        caught: deserialization failed
        scheduler state: running=STALE (non-NULL) ready=nonempty

   After that, `protothread_run()`'s `pt_assert(s->running == NULL)` fires in a
   debug build, and in a production build `pt_add_ready()`'s `!s->running` test
   is wrong permanently, so `ready_function` never fires again. The throwing
   protothread is also off every list with its context un-freed. Any C++ use
   needs `protothread_run()` to restore `s->running` on the way out -- a
   `try`/`catch(...)` that resets it and rethrows, or a small RAII guard --
   and a documented policy for what a throwing protothread means.

Also worth doing for a C++ port: `pt_set_atexit()` is the natural hook for
running a context's destructor when a protothread is killed.

Note that for a *hosted* C++ codebase, C++20 coroutines already provide
suspendable functions with working locals, RAII and exceptions, at the cost of a
heap-allocated frame. Protothreads win where that frame is unaffordable. The
port is most valuable for C++ on microcontrollers, which is where the memory
argument actually bites.

## More synchronization primitives ##

Timers are done (`protothread_timer.h`). Remaining, most useful first:

  * **Message queue.** The most-used RTOS primitive in practice, and it would
    replace the hand-rolled single-slot mailbox in the README's own example.
    Bounded ring over a caller-supplied buffer, blocking put/get, no allocation.
  * **Event flags.** Wait for any or all of a bit mask. Pairs well with
    interrupt handlers, and is the cheap workaround for the lack of multi-wait.
  * **Wait with timeout.** Needs the pattern the reader-writer lock already
    uses: block on a private channel with an explicit waiter list, so the wakeup
    can come from either the primitive or the timer. Raw `pt_wait()` on a shared
    channel cannot support this, because a thread can only be on one wait list
    (`pt_thread_t` has a single `channel` field and a single `next` pointer).
  * **`pt_join()`.** The README says outright that the system cannot tell you
    when a thread exits. The exiting thread broadcasts on its own
    `pt_thread_t` address; an "exited" flag avoids losing a late join.
  * **Barrier and countdown latch.** Both tiny; the latch is Go's `WaitGroup`,
    which suits the fan-out/fan-in shape protothreads fall into naturally.

Deliberately *not* worth adding: spinlocks, atomics, RCU, seqlocks. They exist to
handle preemption or true parallelism, and this scheduler has neither.

## Deadlock and quiescence detector ##

Under `PT_DEBUG`, every `pt_func_t` already records `__FILE__`, `__LINE__` and
`__FUNCTION__`, chained through `next` -- which is what the `ptbt` gdb macro
walks. So the program can print a symbolic backtrace of every blocked
protothread without a debugger attached.

Combine that with a fact unique to a deterministic cooperative scheduler: when
`protothread_run()` returns false while threads remain on wait lists, the system
is *provably* deadlocked or quiescent -- there is no "maybe another core will
signal it" ambiguity. A `protothread_check_stalled()` that dumps each blocked
thread's channel and `file:line` would turn the worst kind of event-driven
debugging into a one-line call, and costs nothing in a production build.

## Smaller items ##

  * **Continuous integration.** A GitHub Actions matrix over the configurations
    the test suite already passes (`PT_DEBUG` on/off, `NDEBUG` on/off, `PT_NWAIT`
    1/4/1024, `-O0/-Os/-O2/-O3`, ASan+UBSan, `-std=c99` through `c23`, all with
    `-Wall -Wextra -Werror`). This would have caught the uninitialized `atexit`
    bug and the `PT_DEBUG` ABI mismatch.
  * **Tag a release.** `git tag -a v2.0.0`; there has never been one.
  * **Document that `pt_wait`/`pt_signal` *is* a condition variable**, and that
    it needs no associated mutex because the scheduler is non-preemptive.
    People arriving from pthreads look for `pt_cond_t`, fail to find it, and
    conclude something is missing that is not.
  * **Better wait-list hash.** `((uintptr_t)chan >> 4) & (PT_NWAIT-1)` clusters
    badly when channels are elements of an array of structures: 1000 contexts of
    128 bytes reach only 128 of 1024 buckets, with chains 8 long. A multiply-shift
    on the high bits gives 847 buckets and chains of 2. But this is a *hosted*
    optimization -- a 64-bit multiply is expensive on an 8- or 16-bit MCU, and at
    `PT_NWAIT=1` there is no hash at all -- so it should be conditional.
  * **A payload in `pt_t`.** `pt_t` is already a struct wrapping the return
    enum, so adding an `intptr_t` would let a child protothread return one word
    directly. It would actually work: only the final `PT_DONE` return reaches a
    live caller, and it does so with no unwind in between, so unlike a local the
    value survives. Measured cost of widening `pt_t` from 4 to 16 bytes, on a
    freestanding `-Os` build: `.text` grows 6 bytes on x86-64 and 10 bytes on
    x86-32, and the test suite still passes. Rejected for now, because 32-bit ARM
    AAPCS returns any struct larger than 4 bytes through a hidden pointer, which
    puts the cost on every `pt_call()` return on the primary target -- and it
    buys syntax for a single word where the callee's context structure already
    carries any number of values for free. See "Structure of a protothread" in
    README.md for that idiom.
  * **Do not "simplify" `protothread_init()`.** The explicit loop that clears
    `s->wait[]` looks like something to replace with
    `*s = (struct protothread_s){0}`, and that is wrong. Measured undefined
    symbols in a freestanding build at `PT_NWAIT=1024`: the loop needs none at
    any optimization level, while the struct assignment makes gcc emit `memset`
    and clang emit `memcpy` and `memset` at `-O0` (none at `-O1` and above). That
    breaks the zero-libc-symbols guarantee in exactly the build someone debugging
    on bare metal would use. `-ffreestanding` does not prevent it: the standard
    permits a compiler to emit calls to `memcpy`, `memset`, `memmove` and
    `memcmp` even in freestanding mode, which is why that property has to be
    tested rather than assumed. The CI freestanding job loops `-O0` through
    `-Os`, so it would catch a regression -- the `-O0` in that list is
    load-bearing. (Relatedly: C has no default member initializers, the C++11
    feature that would let the defaults live in the struct declaration itself.
    C23's `= {}` still initializes an object, not a type.)
  * **`pt_mutex_t`.** A semaphore of 1 or the write half of the reader-writer
    lock already covers it, but a dedicated one would be smaller and could assert
    that the releaser is the owner.
  * **`set(CMAKE_C_COMPILER "gcc")`** in CMakeLists hard-forces gcc on every
    platform, though clang works fine. Make it conditional or drop it.
  * **Killing a sleeping protothread** leaves a dangling entry on the timer list
    unless `pt_timer_cancel()` is called first; same hazard the reader-writer
    lock has. Documented, but could be handled automatically via `pt_atexit`.
