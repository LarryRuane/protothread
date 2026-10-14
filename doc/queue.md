# Message queues

`#include "protothread_queue.h"`. A queue is a first-in, first-out buffer of fixed capacity in storage you supply, so it needs no allocator. Go calls the same thing a channel. Queues are type-safe. `PT_QUEUE_DEFINE(name, type)` generates a queue type `name_t` and its functions, much as `<sys/tree.h>` generates trees, and `pt_queue_t` is a predefined queue of `void *`.

```c
typedef struct { int kind; int value; } msg_t;
PT_QUEUE_DEFINE(msgq, msg_t)            /* msgq_t, msgq_try_send() and the rest */

msg_t slots[8];
msgq_t q;
msgq_init(&q, slots, 8);
```

Sending and receiving block like `pt_wait()` does, in your own protothread, so they are macros, and they work on any queue. Items go in and come out through pointers, so each is copied once each way:

```c
pt_queue_send(c, &q, &c->msg);          /* blocks while full */
pt_queue_receive(c, &q, &c->msg);       /* blocks while empty */
```

> `void pt_queue_send(struct context_t *c, name_t *q, type const *item)`
>
> Block while the queue is full, then append a copy of `*item`.

> `void pt_queue_receive(struct context_t *c, name_t *q, type *item)`
>
> Block while the queue is empty, then remove the oldest item into `*item`.

> `void pt_queue_wait_new(struct context_t *c, name_t *q, unsigned int seen)`
>
> `unsigned int name_sent(name_t const *q)`
>
> Block, without taking anything, until something has been sent since `name_sent()` returned `seen`. See the example below.

Like the other blocking macros, these evaluate `q` and `item` more than once, so neither may have side effects. They aren't `pt_call()`s, so `pt_call_waited()` doesn't apply to them. With queues it's seldom needed, since what a protothread needs to know arrives in the message, not in memory that others may have changed.

> `bool_t name_try_send(protothread_t, name_t *q, type const *item)`
>
> `bool_t name_try_receive(protothread_t, name_t *q, type *item)`
>
> The same as send and receive, but return FALSE instead of blocking. `try_send()` is safe to call from an interrupt handler once the `PT_CRITICAL_*` macros are defined, and a protothread blocked in `pt_queue_receive()` or `pt_queue_wait_new()` cannot miss what it sends.

> `unsigned int name_count(name_t const *q)`
>
> `type *name_at(name_t *q, unsigned int i)`
>
> `bool_t name_remove(protothread_t, name_t *q, unsigned int i, type *item)`
>
> These look inside the queue without blocking. They return the number of items, a pointer to the `i`-th oldest (0 is the next to be received), and remove the `i`-th oldest while keeping the rest in order. `item` may be NULL to discard. A pointer from `at()` stays valid until something is removed.

Like the [semaphore](sem.md), a queue isn't strictly fair. A protothread that finds it non-empty takes an item even if another was woken for it first. The reason is the same, to avoid convoys. Every wakeup is a broadcast, and senders and receivers wait on different channels, so sending wakes only receivers and receiving wakes only senders. Items are copied by assignment, and for an item larger than a few words the compiler may emit a call to `memcpy` (clang targeting Cortex-M does so from 16 bytes at `-Os`). Every embedded toolchain provides it, but if you are avoiding it, queue pointers instead.

**There is no `select`.** Go's `select` waits on several channels at once, but a protothread waits on one channel at a time, and that stays true here. Pike's [Go Concurrency Patterns](https://www.youtube.com/watch?v=f6kdp27TYZs) talk shows the usual alternative just before it introduces `select`. It's *fan-in*, where everything a receiver might want arrives on one queue. With different kinds of message, the item is a tagged union:

```c
typedef struct {
    enum { REPLY, STATUS, TIMEOUT } kind;
    unsigned int gen;               /* for timeouts; see below */
    union { struct reply reply; struct status status; } u;
} msg_t;
```

**Timeouts.** A timeout is just another sender, a small protothread that sleeps (using `pt_sleep()` from [timers](timer.md)) and then sends a `TIMEOUT` message into the same queue. That's all Go's `time.After()` does:

```c
static pt_t
timeout_thr(void * const env)
{
    timeout_context_t * const c = env;
    pt_resume(c);
    pt_sleep(c, &c->timer_env, c->timers, 10);
    pt_queue_send(c, c->q, &c->msg);
    return PT_DONE;
}
```

In Go, the timeout has a channel of its own, so when the reply wins, that channel is simply forgotten. Here the timeout shares the reply's queue, so two things are needed. When the reply wins, stop the timer, which also frees its context for the next wait. And since the timer may already have sent by then, number each wait, and skip a `TIMEOUT` left over from an earlier one:

```c
c->timeout.msg.kind = TIMEOUT;
c->timeout.msg.gen = ++c->gen;
pt_create(pt_get_pt(c), &c->timeout.pt_thread, timeout_thr, &c->timeout);
do {
    pt_queue_receive(c, &q, &c->msg);
} while (c->msg.kind == TIMEOUT && c->msg.gen != c->gen);
if (c->msg.kind != TIMEOUT) {
    pt_timer_cancel(&timers, &c->timeout.timer_env);
    pt_kill(&c->timeout.pt_thread);
}
```

When a receiver wants one kind of message while others wait their turn, it can look before it takes. `pt_queue_wait_new()` blocks only until something new arrives, so messages it has already passed over don't wake it again. `seen` is read *before* looking, so that a message sent while the receiver looks, even by an interrupt, ends the wait at once instead of being slept through:

```c
for (;;) {
    c->seen = msgq_sent(&q);
    for (c->i = 0; c->i < msgq_count(&q); c->i++) {
        if (msgq_at(&q, c->i)->kind == REPLY) {
            msgq_remove(pt_get_pt(c), &q, c->i, &c->msg);
            goto got_reply;
        }
    }
    pt_queue_wait_new(c, &q, c->seen);
}
```

That's linear in the length of the queue, which is fine for the queues a microcontroller has.

**Queues of queues.** In Go a channel can be sent over a channel, which makes request and reply easy. The client sends its question together with a channel for the answer, and the server replies on whatever channel it was given. A Go channel is a reference, so the equivalent here is a pointer to a queue:

```c
typedef struct {
    int x;
    intq_t * reply;                 /* where the answer goes */
} request_t;
PT_QUEUE_DEFINE(requestq, request_t)

/* server */
pt_queue_receive(c, &requests, &c->req);
c->answer = c->req.x * c->req.x;
pt_queue_send(c, c->req.reply, &c->answer);

/* client, with its own reply queue in its context */
c->req.x = 7;
c->req.reply = &c->replies;
pt_queue_send(c, &requests, &c->req);
pt_queue_receive(c, &c->replies, &c->answer);
```

Send the pointer, never the queue itself. A copy of a queue would be a second queue sharing the first one's storage. The reply queue also has to outlive the request, which it does here because the client waits for its answer.
