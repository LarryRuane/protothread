# Timers

`#include "protothread_timer.h"`. The library never reads a clock -- there is no portable one, and depending on one would cost the freestanding property. Instead you drive it from whatever time source you already have: a tick interrupt, a SysTick handler, or the idle loop. A sleep is measured from the most recent `pt_timer_run()`, so the resolution of `pt_sleep()` is your tick period.

> `void pt_timers_init(pt_timers_t *timers, pt_time_t now)`
>
> Initialize a timer list. There is usually one per clock, and one clock.

> `void pt_sleep(struct context_t *c, pt_timer_env_t *timer_env, pt_timers_t *timers, pt_time_t ticks)`
>
> Block for `ticks`. Needs a `pt_timer_env_t` in the calling context structure, like any other nested function's context.

> `void pt_timer_run(protothread_t, pt_timers_t *timers, pt_time_t now)`
>
> Wake every protothread whose deadline has arrived, and record `now` as the base for subsequent sleeps. `now` may jump by more than one tick; everything that became due is woken. Safe to call from an interrupt handler if the `PT_CRITICAL_*` macros are defined.

> `bool_t pt_timer_next(pt_timers_t const *timers, pt_time_t *deadline)`
>
> Report the soonest deadline, or FALSE if nothing is sleeping. Use this in an idle loop to decide how long the CPU can be stopped.

> `bool_t pt_timer_cancel(pt_timers_t *timers, pt_timer_env_t *timer_env)`
>
> Take a sleeper off the timer list; returns TRUE if it was still pending. This does **not** wake it: the protothread stays blocked in `pt_sleep()`. It's for use before `pt_kill()`ing or freeing a protothread that might be sleeping, since otherwise the timer list keeps a dangling entry.

The clock type is `PT_TIME_T` (default `uint32_t`), paired with the signed `PT_TIME_DIFF_T` (default `int32_t`). **Counter wraparound is handled correctly**: comparisons use a signed difference rather than a direct `>`, so a 32-bit millisecond clock behaves properly across its 49-day rollover. The one requirement is that no live deadline be more than half the counter range in the future -- about 24 days for that clock.

A typical bare-metal idle loop. The ready function tells it whether an interrupt made anything ready after the last run, so it never sleeps with work waiting (see [Lost wakeups](../README.md#lost-wakeups)):

```c
static volatile bool work;
static void on_ready(void *env) { work = true; }    /* may run in an interrupt */

protothread_set_ready_function(pt, on_ready, NULL);
for (;;) {
    pt_timer_run(pt, &timers, clock_now());
    work = false;
    while (protothread_run(pt));
    disable_interrupts();
    if (!work) {
        if (pt_timer_next(&timers, &deadline)) {
            sleep_until(deadline);      /* like WFI, wakes for a pending interrupt */
        } else {
            wait_for_interrupt();
        }
    }
    enable_interrupts();
}
```
