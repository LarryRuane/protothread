# Demo programs

Four complete, working programs. They are built together, and are not built
by default because two of them need POSIX threads and the library itself
does not:

```
cmake -S . -B build -DPROTOTHREAD_DEMOS=ON
cmake --build build
./build/ptpool && ./build/ptaio && ./build/pthelper && ./build/pttpcc
```

| source | binary | what it shows |
| --- | --- | --- |
| [`pool.c`](pool.c) | `ptpool` | 500 protothreads over a fixed pool of 4 worker threads: parallel computation underneath, protothreads owning the sequencing |
| [`async_io.c`](async_io.c) | `ptaio` | concurrent asynchronous I/O and no POSIX threads at all -- one `poll()` loop turning completions into signals |
| [`helper_thread.c`](helper_thread.c) | `pthelper` | one throwaway POSIX thread per blocking call, created and joined by the protothread that needs it |
| [`tpcc.c`](tpcc.c) | `pttpcc` | a load-generating client modeled on the TPC-C benchmark, with one protothread per terminal, against a simulated database |

The first three are each a different way of combining protothreads with
work that happens outside them.

All three obey the same rule, which is the thing to take away from them: a
worker, a completion handler or an interrupt handler must never call
`pt_signal()` itself. It records what happened, and the thread that owns
`protothread_run()` turns that into a signal *between* protothread runs,
where no protothread is halfway through enqueuing itself. `pool.c` explains
the race this avoids in full.

Run `ptaio` with `-v` to trace every submission and completion. They come out
thoroughly interleaved and in no useful order, because the simulated device
finishes requests late and out of sequence, and each protothread is somewhere
different in its own sequence of I/Os:

```
   0 ms  submit  protothread 19  op 1/3
   0 ms  done    protothread  8  op 1/1
   1 ms  done    protothread  3  op 1/3
   1 ms  submit  protothread  3  op 2/3
   1 ms  done    protothread  5  op 1/3
   1 ms  submit  protothread  5  op 2/3
   1 ms  done    protothread 14  op 1/4
   1 ms  submit  protothread 14  op 2/4
```

The exact order differs from run to run, because the device's deadlines are
anchored to the wall clock. The protothread scheduling itself stays entirely
determined by the completion events; see the header comment in `async_io.c`.

No protothread is written to cope with that, and none has to be: each one
reads as straight-line code that starts an I/O and waits for it.

## The TPC-C client

`tpcc.c` is modeled on the client in YDB's [Making Coroutines
Routine](https://blog.ydb.tech/making-coroutines-routine-building-a-scalable-tpc-c-client-in-c-b14f55a09471),
which needed 150,000 OS threads and 600 GiB before they rewrote it with C++20
coroutines. Here each terminal is a protothread written as the loop a clerk
follows, each transaction is a nested function, and each query blocks until
the simulated database replies. It uses three of the add-on headers: timers for
keying and think time, a queue per database shard, and row locks held for the
length of a transaction.

The clock is simulated, so ten minutes take a fraction of a second, and the
same seed always gives the same output:

```
$ ./build/pttpcc
100 warehouses, 1000 terminals, 10 database shards of 4 workers, 10 simulated minutes

transaction        count   mean ms    p90 ms    max ms
New-Order          12980      19.5        26       38
Payment            12201       9.0        11       27
Order-Status        1181       4.5         6        6
Delivery            1177      45.1        49       63
Stock-Level         1188       3.0         4        4

New-Orders per simulated minute: 1298
lock waits: 44, peak requests queued on a shard: 3
terminal context: 456 bytes; CPU time 0.04 s; peak RSS 3.1 MiB
consistency: OK
```

`-w 10000` runs 100,000 terminals on one OS thread. `-u` leaves out the row
locks. A New-Order or Payment reads a value, waits for its next query, and
writes the value back, so without a lock another terminal can slip in
between. The run then fails TPC-C's consistency checks and reports what was
lost. There's no preemption, but a wait between a read and a write is still a
race. `-H` starts every terminal at once, the thundering herd YDB ran into,
and the peak queue depth shows it.

This demo also shows why the timers keep sleepers in a heap. They used to be in
a sorted list, and a new sleep usually went at the end, so each one walked
the whole list. With the list, `-w 1000` (10,000 terminals) took 38 seconds of
CPU. With the heap it takes about half a second.
