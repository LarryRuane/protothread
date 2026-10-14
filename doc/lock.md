# Reader-writer locks

`#include "protothread_lock.h"`. A `pt_lock_t` allows either many concurrent readers or one writer. Each thread needs a `pt_lock_env_t` in its context structure.

> `void pt_lock_init(pt_lock_t *lock)`
>
> Initialize the lock. Must be called before use.

> `void pt_lock_acquire_read(struct context_t *c, pt_lock_env_t *lock_env, pt_lock_t *lock)`
>
> Acquire the lock for shared (read) access. May block.

> `void pt_lock_acquire_write(struct context_t *c, pt_lock_env_t *lock_env, pt_lock_t *lock)`
>
> Acquire the lock for exclusive (write) access. May block.

> `void pt_lock_release_read(pt_lock_env_t *lock_env, pt_lock_t *lock)`
>
> `void pt_lock_release_write(pt_lock_env_t *lock_env, pt_lock_t *lock)`
>
> Release the lock. Guaranteed not to block.

> `bool_t pt_lock_cancel(pt_lock_env_t *lock_env, pt_lock_t *lock)`
>
> Take a request out of the lock, whether it is still queued or already granted, and start whatever that lets proceed. Returns TRUE if the lock still knew about the request. This does **not** wake the protothread. It's for use before `pt_kill()`ing or freeing one that might be using the lock, exactly as `pt_timer_cancel()` is for one that might be sleeping. Guaranteed not to block.

Requests are granted in arrival order, so a steady stream of readers cannot starve a waiting writer. Consecutive readers at the head of the queue are all started together. Unlike the semaphore, the lock is fair. A release hands it directly to the next request before waking it, so a protothread that releases and immediately reacquires waits its turn. The downside is the convoy described under [semaphores](sem.md). The same queue is why a protothread that might be using the lock needs `pt_lock_cancel()` before it's killed or freed. Otherwise its request would stay queued, and be granted to a thread that never releases it.

There's no upgrade call, because you don't need one. Release the read lock, acquire the write lock, and ask whether that blocked:

```c
pt_lock_release_read(&c->lock_env, c->lock);
pt_lock_acquire_write(c, &c->lock_env, c->lock);
if (pt_call_waited(c)) {
    /* others may have written while we waited; re-check what we read */
}
```

Releasing never blocks, so if acquiring didn't either, nothing else ran in between and the upgrade was atomic. That's the case when you were the only reader and nobody was queued. Otherwise you still end up holding the write lock, but anything you read under the read lock may be stale. The answer can only be wrong in the safe direction. If another reader was holding the lock, it reports a wait even though nothing could have changed.
