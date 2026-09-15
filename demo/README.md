# Demo programs

Three complete, working programs, each one a different way of combining
protothreads with work that happens outside them. They are built together,
and are not built by default because two of the three need POSIX threads
and the library itself does not:

```
cmake -S . -B build -DPROTOTHREAD_DEMOS=ON
cmake --build build
./build/ptpool && ./build/ptaio && ./build/pthelper
```

| source | binary | what it shows |
| --- | --- | --- |
| [`pool.c`](pool.c) | `ptpool` | 500 protothreads over a fixed pool of 4 worker threads: parallel computation underneath, protothreads owning the sequencing |
| [`async_io.c`](async_io.c) | `ptaio` | concurrent asynchronous I/O and no POSIX threads at all -- one `poll()` loop turning completions into signals |
| [`helper_thread.c`](helper_thread.c) | `pthelper` | one throwaway POSIX thread per blocking call, created and joined by the protothread that needs it |

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
