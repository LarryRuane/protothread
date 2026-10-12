# Semaphores

`#include "protothread_sem.h"`. A semaphore is an ordinary `unsigned int` that you initialize yourself (to 1 for mutual exclusion). Acquiring one needs a `pt_sem_env_t` context, which lives in your context structure like any other nested function's.

> `void pt_sem_acquire(struct context_t *c, pt_sem_env_t *sem_env, unsigned int *value)`
>
> Wait until the semaphore is non-zero, then decrement it. May block.

> `void pt_sem_release(pt_sem_env_t *sem_env, unsigned int *value)`
>
> Increment the semaphore and wake any waiters. Guaranteed not to block.

This implementation is deliberately not fair. Releasing the semaphore wakes the waiters, but they can't take it until they run, so a protothread that releases and then reacquires without blocking in between gets it back ahead of them, and can keep doing so indefinitely. The fair alternative is to hand the semaphore straight to the oldest waiter, but then two protothreads that both use it heavily end up taking turns, with a context switch on every use and the hold time stretched by the wait to be scheduled. Those are convoys, and avoiding them is the reason event-wait semaphores work this way (see section 13 of [Process Synchronization in the UTS Kernel](https://static.usenix.org/publications/compsystems/1990/sum_ruane.pdf)). If a waiter mustn't starve, call `pt_yield()` between releasing and reacquiring, so the woken waiters run first.
