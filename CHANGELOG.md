# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.0] - Unreleased

The first tagged release. It rebuilds the library around one goal: to be usable
on an OS-less, memory-constrained system with no C library at all, which is the
environment protothreads were invented for.

**This is not a drop-in replacement for 1.x.** The API you write against is
essentially unchanged, but the packaging is not; see
[Upgrading from 1.x](README.md#upgrading-from-1x) in the README.

### Added

- `protothread_timer.h`: `pt_sleep()`, `pt_timer_run()`, `pt_timer_next()` and
  `pt_timer_cancel()`. The library never reads a clock; you drive it from a tick
  interrupt or an idle loop, and a 32-bit millisecond clock is handled correctly
  across its rollover.
- Interrupt safety: define `PT_CRITICAL_T`, `PT_CRITICAL_ENTER()` and
  `PT_CRITICAL_EXIT()` to protect every scheduler list update. They are no-ops by
  default.
- `PT_CRITICAL_ASSERT()`, which checks that internal list functions are entered
  only with a critical section already held.
- `PT_NO_MALLOC`, which drops `<stdlib.h>`, `protothread_create()` and
  `protothread_free()` for systems with no heap.
- `PT_NWAIT` is configurable. It was fixed, and cost 8 KB per `protothread_t`.
- `pt_assert()` can be defined by the application, to avoid `<assert.h>`.
- Version macros `PT_VERSION_MAJOR`, `PT_VERSION_MINOR`, `PT_VERSION_PATCH`,
  `PT_VERSION_NUMBER`, `PT_VERSION_STRING` and `PT_VERSION_AT_LEAST()`, so a
  vendored copy of the headers can identify itself.
- The headers compile as C++, from C\+\+11 on. Top-level protothread functions need
  an explicit cast from `env_t`; nested ones can take a typed context instead.
- Demo programs in `demo/`: a worker-thread pool, asynchronous I/O with no POSIX
  threads, and a helper thread per blocking call. Build with
  `-DPROTOTHREAD_DEMOS=ON`.
- A benchmark against POSIX threads, built with `-DPROTOTHREAD_BENCH=ON`.
- Continuous integration: gcc and clang on Linux and Apple clang on macOS, across
  debug and release configurations, `-std=c99` through `c23`, C++, and the
  address, undefined-behavior and thread sanitizers; plus checks that a
  freestanding build needs no libc symbols and that internal list functions are
  only entered inside a critical section.

### Changed

- **Header-only.** The contents of `protothread_sem.c` and `protothread_lock.c`
  moved into the matching headers, so there is nothing to compile or link.
- **Freestanding.** The headers include only `<stddef.h>`, `<stdint.h>` and
  `<stdbool.h>` unconditionally. Code that relied on getting `malloc`, `memset`
  or `assert` transitively from `protothread.h` must include `<stdlib.h>`,
  `<string.h>` or `<assert.h>` itself.
- Internal names now carry a `pt_i_` or `PT_I_` prefix, so that any name without
  one is public API. Several were visible in the 1.x header and are renamed:
  `pt_wake()`, `pt_get_protothread()`, `pt_create_thread()`, `pt_add_ready()`,
  `pt_link()`, `pt_unlink()` and the other scheduler internals, `PT_WAIT`, and
  `pt_sem_acquire_f()`, `pt_lock_acquire_read_f()` and
  `pt_lock_acquire_write_f()`. Code that uses only the documented API is
  unaffected.
- The struct tags `_pt_sem_env_t`, `_pt_lock_env_t` and `_pt_lock_t` are now
  `pt_sem_env_s`, `pt_lock_env_s` and `pt_lock_s`, because identifiers beginning
  with an underscore are reserved to the C implementation. The typedef names are
  unchanged.
- The license changed from Apache-2.0 to MIT.
- The README was substantially rewritten, with a complete API listing and new
  sections on local variables, wait channels, interrupt safety and lost
  wakeups, deterministic testing, multi-core systems and blocking system calls.

### Removed

- The `protothread-static` and `protothread-shared` CMake targets, and
  `-lprotothread` from the pkg-config file: there is no longer a library to link.
- `pt_set_atexit()`. Do any cleanup yourself once `pt_kill()` has found the
  thread: `if (pt_kill(&c->pt_thread)) cleanup(c);` does exactly what the
  callback did. Removing it drops the minimum RAM per protothread from 32 to 28
  bytes on a 32-bit target, and from 64 to 56 on a 64-bit one.

### Fixed

- The library did not compile with clang, which rejects an indirect `goto` in a
  function containing no address-of-label expression -- the case for any
  protothread function that never blocks.
- An application built with `PT_DEBUG=0` against a library built with the default
  silently corrupted memory, because the setting changes the layout of
  `pt_thread_t` and `pt_func_t`. With nothing precompiled, that mismatch can no
  longer happen.
- The `pt_create` macro ended in a semicolon, so `if (x) pt_create(...); else ...`
  did not compile.
- `protothread_create()` dereferenced a null result from `malloc()`.
- Reader-writer locks queued waiters last-in-first-out and examined only the
  head, so a steady stream of readers could starve a waiting writer, and later
  writers overtook earlier ones. Requests are now granted in arrival order.

## Before 2.0.0

There were no tagged releases before 2.0.0. The earlier code, which CMake and
pkg-config reported as version 1.0 regardless of what changed, is referred to
here as 1.x.

[2.0.0]: https://github.com/LarryRuane/protothread/releases/tag/v2.0.0
