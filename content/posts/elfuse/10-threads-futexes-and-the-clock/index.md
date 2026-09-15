---
title: 'elfuse 10: Threads, futexes, and the clock'
date: 2026-09-09T19:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "threads"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 10 of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 9](../09-signals-and-the-return-path/) covered signals. This part is about
three things multi-threaded programs do often: start threads, wait for each
other, and read the clock. elfuse has fast paths for the last two.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac.

## The problem

A thread is a separate line of execution inside one process. Threads share the
process's memory and run at the same time, each with its own registers and
stack. A web server or a compiler may run dozens.

Threads that share memory need to take turns. A mutex (mutual exclusion lock)
lets one thread at a time into a piece of code, and a condition variable lets a
thread sleep until another thread signals that something changed. Programs also
read the time often, for timeouts and for measuring.

Under elfuse every thread is a vCPU, and every system call that reaches elfuse
costs a round trip of roughly a microsecond (part 7). Programs take locks and
read the clock too often to spend that on each call.

## One thread, one vCPU

`src/runtime/thread.h` describes the model in its first lines
([thread.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/thread.h#L8-L15)):

```c
 * Maintains a table of guest threads. Each thread has its own HVF vCPU running
 * on a dedicated host pthread. The main thread is registered at startup; worker
 * threads are added via clone(CLONE_THREAD). A _Thread_local pointer provides
 * O(1) access to the current thread's entry from any syscall handler.
 *
 * SP_EL1 allocation: each thread gets a 4KiB EL1 exception stack carved from
 * the shim data region (g->shim_data_base + 2MiB). Thread 0 (main) gets the
 * top, thread N gets offset -(N * 4096).
```

The table has a fixed size, `#define MAX_THREADS 64`. This program starts
threads until one fails:

```c
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t go = PTHREAD_COND_INITIALIZER;
static int released;

static void *worker(void *arg)
{
    (void) arg;
    pthread_mutex_lock(&lock);
    while (!released)
        pthread_cond_wait(&go, &lock);
    pthread_mutex_unlock(&lock);
    return NULL;
}

int main(void)
{
    pthread_t t[100];
    int n = 0, err = 0;
    while (n < 100 && (err = pthread_create(&t[n], NULL, worker, NULL)) == 0)
        n++;
    printf("created %d threads besides main", n);
    if (err)
        printf(", then: %s", strerror(err));
    printf("\n");
    pthread_mutex_lock(&lock);
    released = 1;
    pthread_cond_broadcast(&go);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; i++)
        pthread_join(t[i], NULL);
    return 0;
}
```

```sh
$ aarch64-linux-gnu-gcc -static -O2 -pthread -o threads threads.c
$ build/elfuse ./threads
19:58:39 ERROR src/runtime/forkipc.c:763: clone_thread: thread table full
created 63 threads besides main, then: Resource temporarily unavailable
```

With `main` and 63 more threads the table was full, and the next
`pthread_create` was refused with `EAGAIN`, the error Linux returns when a
thread limit is reached. On Linux this program would create all 100.

## Creating a thread

`pthread_create` in the C library calls `clone` with `CLONE_THREAD` and the
flags that share memory, open files, and signal handlers with the caller.
elfuse handles it in `sys_clone_thread()` in `src/runtime/forkipc.c`. The
parent's side reserves a table slot, a thread id, and a shim stack, copies its
own registers, and starts a macOS thread. The new thread then creates its own
vCPU
([forkipc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/forkipc.c#L912-L927)):

> Worker pthread entry: creates the HVF vCPU on this thread (required by Apple
> HVF, since the vCPU is bound to the creating thread), configures all
> registers from parent state, then enters the run loop.

That order follows from Hypervisor.framework's thread rule (part 2): a vCPU is
bound to the thread that created it. The new vCPU gets the parent's registers
with three changes:

- `x0` is 0, which is how `clone` tells the new thread it is the child;
- the stack pointer is the new thread's stack, allocated by the C library;
- `TPIDR_EL0` is the new thread's thread pointer, the register the C library
  uses to find the thread's private variables.

It starts at EL0, at the instruction after the parent's `svc`, without going
through the shim's start-up code
([forkipc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/forkipc.c#L1037-L1039)).
The parent waits until the new thread's vCPU is set up, then returns its thread
id.

## Waiting for each other

A modern C library builds mutexes on a futex ("fast user-space mutex"). The
lock is a 32-bit word in the program's memory. Taking a free lock is an atomic
compare-and-swap on that word: the processor changes the word only if it still
holds the expected value, all in one step. Releasing a lock nobody waits for is
also a single atomic update. No system call is involved. Only when a thread
must wait does it call `futex(&word, FUTEX_WAIT, expected)`: "sleep, but only
if the word still holds `expected`". Another thread releasing the lock calls
`futex(&word, FUTEX_WAKE, 1)`.

The check and the sleep have to be atomic. If the word changes between them,
the waiter could miss the wake meant for it and keep sleeping. elfuse has two
ways to do that.

The general one is a hash table of wait queues keyed by the word's address
([futex.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/futex.c#L12-L16)):

```c
 * Atomicity: The critical FUTEX_WAIT race (guest writes futex word after the
 * current read but before the waiter sleeps) is prevented by holding the bucket
 * lock across the word-read + enqueue + cond_wait sequence. FUTEX_WAKE also
 * acquires the same bucket lock, so a wake cannot slip between the read and the
 * wait.
```

The table has 1024 buckets. The comment records why
([futex.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/futex.c#L148-L168)):
with 64 buckets, "eight threads each waking their own private futex measured
801 ns per wake, worse than the same test at four threads", and at 1024 with a
better hash, "316 ns per wake against 801, and eight threads now beat four".

The second is `os_sync_wait_on_address`, which macOS has offered since 14.4:
the same "sleep if the word still holds this value" operation. elfuse uses it
for plain `FUTEX_WAIT`. It differs from Linux in one detail, which elfuse
corrects
([futex.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/futex.c#L929-L950)):

> Compare-after-block re-check. Darwin folds two distinct Linux outcomes into a
> single rc>=0: a genuine wake, and the racy "value moved off expected between
> the pre-check and the in-kernel compare" case that Linux reports as -EAGAIN.

After macOS returns, elfuse reads the word again and reports `EAGAIN` if it has
moved.

## Answering in the shim

Many `FUTEX_WAIT` calls turn out to be unnecessary: by the time the call
arrives, another thread has already released the lock and the word has moved.
Linux answers those with `EAGAIN`. The shim does the same check at EL1 without
an exit, and before giving up it re-reads the word up to 4096 times, in case the
other thread is about to release. The count was measured
([shim.S](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/shim.S#L541-L547)),
on four threads contending for one mutex, median of nine runs:

| Re-reads before exiting | Seconds |
|---|---|
| none | 0.74 |
| 256 | 1.01 |
| 1024 | 0.31 |
| 2048 | 0.25 |
| 4096 | 0.23 |

Re-reading in a loop like this is called spinning, and giving up to sleep is
parking. The comment explains the 256 row: "A spin too short to win the handoff
pays for itself twice, once in the spin and once in the park it failed to
avoid". The shim also answers `FUTEX_WAKE` itself when no thread is waiting in
the hash bucket of that address.

The loop checks the attention flag from part 7 every 64 re-reads, so a thread
spinning in the shim still notices a signal. To count the fast-path answers,
four threads each take and release one mutex a million times:

```c
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *worker(void *arg)
{
    (void) arg;
    for (int i = 0; i < 1000000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main(void)
{
    pthread_t t[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    printf("counter = %ld\n", counter);
    return 0;
}
```

```sh
$ ELFUSE_SHIM_STATS=1 build/elfuse ./contend 2>&1 | grep -E 'counter|FUTEX'
counter = 4000000
  FUTEX_EAGAIN_HIT     3571571
  FUTEX_EFAULT_HIT     0
  FUTEX_SHAPE_BAIL     0
  FUTEX_MATCH_BAIL     497
  FUTEX_WAKE_HIT       2592273
  FUTEX_WAKE_WAITER_BAIL 8453
```

About 3.6 million waits and 2.6 million wakes were answered at EL1. The two
nonzero `BAIL` counters, about nine thousand together, count fast-path attempts
that had to go on to elfuse.

## Reading the clock

A vDSO ("virtual dynamic shared object") is a small page of code the Linux
kernel maps into every process, shaped like a shared library. Its functions run
in user mode, so a program can call `clock_gettime` without a system call.
elfuse builds its own vDSO page at address `0xf000` and tells the program with
`AT_SYSINFO_EHDR` (part 4)
([vdso.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/vdso.c#L8-L14)):

```c
 * Builds a minimal vDSO ELF image in guest memory exposing versioned
 * __kernel_{rt_sigreturn,clock_getres,clock_gettime,gettimeofday,getcpu}.
 * clock_gettime and gettimeofday are CNTVCT-based fast-path trampolines that
 * serve CLOCK_MONOTONIC (clockid 1) and CLOCK_REALTIME (clockid 0) inline
 * without trapping; clock_getres serves the common nsec-resolution clockids
 * inline; getcpu always returns cpu=0/node=0 (elfuse models one CPU);
 * rt_sigreturn remains a 12-byte SVC trampoline.
```

`CNTVCT_EL0` is a processor register that counts timer ticks at a fixed rate
and that code at EL0 may read. Apple Silicon ticks at 24 MHz; `sysctl
hw.tbfrequency` prints `24000000` on this Mac. The vDSO's `clock_gettime` reads
the register, then turns ticks into time relative to an anchor: a (tick count,
time) pair elfuse refreshes when the vDSO falls back to the system call and the
pair is missing or stale.

A few details make that safe:

- elfuse updates the anchor with a seqlock, a counter that is odd while an
  update is in progress. The vDSO code reads the counter before and after
  reading the anchor and falls back to the system call if it was odd or
  changed.
- The code does the division without a divide instruction. Ticks to
  nanoseconds is `(delta * 699050666) >> 24`, a multiply-then-shift of the kind
  Linux's arm64 vDSO uses.
- It refuses anchors older than `2^22` ticks, about 0.175 s, and makes the
  system call instead, which refreshes the anchor. That catches clock
  adjustments on the Mac. The cap also keeps the nanosecond sum below two
  seconds, so carrying into seconds needs one compare instead of a division
  ([vdso.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/vdso.c#L242-L243)).
- If the attention flag is set, it makes the system call, as the shim's fast
  paths do.

The machine code is not written in assembly. `vdso.c` builds most instructions
with small C encoder functions, such as `enc_movz_x()`, and checks each
fast-path trampoline's declared size at compile time.

Whether a program benefits depends on its C library. This program reads the
monotonic clock a million times:

```c
#include <stdio.h>
#include <time.h>

int main(void)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int i = 0; i < 1000000; i++)
        clock_gettime(CLOCK_MONOTONIC, &now);
    double ns = (now.tv_sec - start.tv_sec) * 1e9 + (now.tv_nsec - start.tv_nsec);
    printf("1000000 calls, %.1f ns per call\n", ns / 1e6);
    return 0;
}
```

Built twice, once against the cross toolchain's static glibc and once against
the static musl C library from the Alpine sysroot:

```sh
$ aarch64-linux-gnu-gcc -static -O2 -o clock-glibc clock.c
$ R=externals/test-fixtures/rootfs
$ aarch64-linux-gnu-gcc -O2 -static -nostdinc -nostdlib -isystem $R/usr/include -o clock-musl \
    $R/usr/lib/crt1.o $R/usr/lib/crti.o clock.c $R/usr/lib/libc.a \
    $(aarch64-linux-gnu-gcc -print-libgcc-file-name) $R/usr/lib/crtn.o
$ build/elfuse ./clock-glibc
1000000 calls, 1338.2 ns per call
$ ELFUSE_STARTUP_TRACE=syscalls build/elfuse ./clock-musl
1000000 calls, 5.7 ns per call
=== syscall histogram (guest exit) ===
 count     total_ms     avg_us     max_us  name
     1        0.057      57.00      57.00  SYS_ioctl
     1        0.049      49.00      49.00  SYS_writev
     1        0.005       5.00       5.00  SYS_exit_group
     1        0.001       1.00       1.00  SYS_set_tid_address
     1        0.001       1.00       1.00  SYS_clock_gettime
total: 5 syscalls, 0.113 ms
wall:  5.977 ms (syscalls = 1.9% of wall)
```

The musl build made one real `clock_gettime` call, which set the anchor, and
answered the other 1,000,000 in the vDSO. The static glibc from this toolchain
never looks up the vDSO (its `libc.a` contains no reference to
`__kernel_clock_gettime`), so every call was a system call. Timings vary with
host load, and the Mac was busy during these runs. Over seven runs each:

| Build | Median | Fastest | Slowest |
|---|---|---|---|
| static glibc | 1570.3 ns | 1336.3 ns | 2010.9 ns |
| static musl | 5.0 ns | 4.6 ns | 6.6 ns |

## The watchdog

A guest can also run for a long time without making any call, and its vCPU
thread stays inside `hv_vcpu_run()` all that time. The main thread's run loop
arms a repeating macOS timer, every 10 seconds by default (`--timeout`), and
writes a counter before and after each `hv_vcpu_run()`. If two timer ticks in a
row see the same unfinished run, elfuse stops the vCPUs and reports a timeout
(part 12).

The timer used to be set and cleared around every run. The comment explains why
that changed
([proc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/proc.c#L2907-L2913)):
"that was two real syscalls per guest syscall: measured 693 ns for the pair,
against a 1226 ns bare HVF round trip." The main thread paid that on every
host-served syscall; worker threads did not.

## What we learned

- Each guest thread is a macOS thread running its own vCPU, created on that
  thread. elfuse allows 64 threads per process.
- Futexes let uncontended locks skip the kernel. elfuse makes wait-if-unchanged
  atomic with a locked hash table or macOS's `os_sync_wait_on_address`, and the
  shim answers pointless waits and wakes without an exit.
- elfuse's vDSO computes the time from the processor's tick counter and an
  anchor elfuse maintains. A C library that uses the vDSO reads the clock in a
  few nanoseconds; one that does not makes a system call per read.
- A repeating watchdog timer on the main thread catches a guest that stops
  making progress, at a fixed cost instead of a cost per system call.

[Part 11](../11-processes-without-fork/) is about processes: how a Linux
program copies itself with `fork` when Hypervisor.framework allows only one VM
per macOS process.
