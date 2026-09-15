---
title: 'mini-elfuse 8: Threads'
date: 2026-09-15T17:00:00+02:00
series: ["mini-elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "mini-elfuse", "linux", "macos", "threads"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 8 of a series that builds mini-elfuse, a small version of
[elfuse](https://github.com/sysprog21/elfuse).
[Part 7](../07-mmap/) added `mmap`. This last part runs a program with two
threads, then lists what elfuse does that mini-elfuse leaves out.

## The problem

`threads.c` starts two threads that each add 1 to a shared counter 100000
times, holding a mutex around each addition, and waits for both:

```c
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void *worker(void *arg)
{
    for (int i = 0; i < 100000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return arg;
}

int main(void)
{
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < 2; i++)
        pthread_join(t[i], NULL);
    printf("counter %ld\n", counter);
    return 0;
}
```

The toolchain's glibc 2.28 keeps threads in a separate library, so the
`Makefile` builds this program with `-pthread`:

```make
$(BUILD)/threads: threads.c | $(BUILD)
	$(CROSS)gcc -static -O2 -pthread -o $@ threads.c
```

With part 7's code the program prints `counter 0`. The trace shows why (the
`load`, `entry`, and earlier start-up lines left out):

```sh
$ ../step-7/build/mini-elfuse -v build/threads
...
syscall 96(0x4a80d0, 0x306ff, 0x4a8700, 0x4a8000, 0x7fffd3f, 0xd0)
  -> -38 (0xffffffffffffffda)
syscall 99(0x4a80e0, 0x18, 0x4a8700, 0x4a8000, 0x7fffd3f, 0xd0)
  -> -38 (0xffffffffffffffda)
syscall 134(0x20, 0x7fffc40, 0x0, 0x8, 0x0, 0xd0)
  -> -38 (0xffffffffffffffda)
syscall 134(0x21, 0x7fffc40, 0x0, 0x8, 0x0, 0xd0)
  -> -38 (0xffffffffffffffda)
syscall 135(0x1, 0x7fffdb0, 0x0, 0x8, 0x0, 0xd0)
  -> -38 (0xffffffffffffffda)
syscall 261(0x0, 0x3, 0x0, 0x7fffd98, 0x0, 0xd0)
  -> -38 (0xffffffffffffffda)
...
syscall 222(0x0, 0x210000, 0x0, 0x20022, 0xffffffffffffffff, 0x0)
  -> 268435456 (0x10000000)
syscall 226(0x10010000, 0x200000, 0x3, 0x880, 0x4a8700, 0x0)
  -> 0 (0x0)
syscall 220(0x3d0f00, 0x1020e980, 0x1020f150, 0x1020f780, 0x1020f150, 0x1020f780)
  -> -38 (0xffffffffffffffda)
syscall 220(0x3d0f00, 0x1020e980, 0x1020f150, 0x1020f780, 0x1020f150, 0x1020f780)
  -> -38 (0xffffffffffffffda)
...
counter 0
```

- The thread library's start-up calls `set_tid_address` (96),
  `set_robust_list` (99), `rt_sigaction` (134), `rt_sigprocmask` (135), and
  `prlimit64` (261). All fail with `ENOSYS`, and glibc continues.
- `pthread_create` reserves a stack with `mmap` and `PROT_NONE`, then makes all
  but its lowest 64 KiB readable and writable with `mprotect` (226). The lowest
  part is a guard against the stack growing too far; in mini-elfuse it guards
  nothing, because every page allows everything.
- Then it calls `clone` (220), which fails. `threads.c` does not check the
  result of `pthread_create`, so both threads are missing and the counter stays
  0.

This part adds `clone` and `futex`, the call threads use to wait for each
other, and makes `exit` end a single thread.

## One vCPU per thread

A thread needs its own set of registers, so it needs its own vCPU. As
[elfuse part 10](../../elfuse/10-threads-futexes-and-the-clock/) describes,
Hypervisor.framework ties a vCPU to the host thread that creates it. So `clone`
starts a host thread, and that thread creates the vCPU and runs the same loop
as the first one. `clone` records what the new thread needs:

```c
struct thread_start {
    uint64_t x[31], pc, sp, tls, clear_child_tid;
};
```

The new host thread starts in `thread_main()`:

```c
/* A thread made by clone gets its own vCPU, which Hypervisor.framework ties
 * to the host thread that creates it. It boots like the first one and resumes
 * after the parent's svc with the parent's general-purpose registers.
 */
static void *thread_main(void *arg)
{
    struct thread_start *t = arg;
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    HV(hv_vcpu_create(&vcpu, &vexit, NULL));
    boot_vcpu(vcpu, t->pc, t->sp);
    for (int i = 0; i < 31; i++)
        HV(hv_vcpu_set_reg(vcpu, HV_REG_X0 + i, t->x[i]));
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, t->tls));
    clear_child_tid = t->clear_child_tid;
    free(t);
    run_vcpu(vcpu, vexit);
    return NULL;
}
```

`boot_vcpu()` gives the new vCPU the same page tables, vectors, and processor
settings as the first, starts it at `t->pc`, and sets its stack pointer.
The general-purpose registers come from the parent, with the three changes
elfuse also makes: `x0` is 0, the stack pointer is the new thread's stack, and
`TPIDR_EL0` is the new thread's thread pointer. Unlike elfuse, mini-elfuse does
not copy the floating-point registers. `pc` is the parent's `ELR_EL1`, so both
threads continue at the instruction after the `svc`; glibc tells them apart by
`x0`.

## clone

```c
    case NR_clone: {
        /* clone(flags, stack, parent_tid, tls, child_tid) */
        if (!(a[0] & CLONE_THREAD)) {
            ret = -LINUX_ENOSYS;
            break;
        }
        struct thread_start *t = malloc(sizeof *t);
        for (int i = 0; i < 31; i++)
            t->x[i] = reg(vcpu, HV_REG_X0 + i);
        t->x[0] = 0; /* the new thread sees clone return 0 */
        t->pc = sysreg(vcpu, HV_SYS_REG_ELR_EL1);
        t->sp = a[1];
        t->tls =
            a[0] & CLONE_SETTLS ? a[3] : sysreg(vcpu, HV_SYS_REG_TPIDR_EL0);
        t->clear_child_tid = a[0] & CLONE_CHILD_CLEARTID ? a[4] : 0;

        pthread_mutex_lock(&lock);
        ret = next_tid++;
        pthread_mutex_unlock(&lock);
        int32_t *parent_tid = guest_ptr(a[2], 4);
        if (a[0] & CLONE_PARENT_SETTID && parent_tid)
            *parent_tid = ret;

        pthread_t thread;
        if (pthread_create(&thread, NULL, thread_main, t) != 0)
            die("pthread_create failed");
        pthread_detach(thread);
        break;
    }
```

glibc passes flags `0x3d0f00`. mini-elfuse looks at four of them:

```c
enum {
    CLONE_THREAD = 0x10000,
    CLONE_SETTLS = 0x80000,
    CLONE_PARENT_SETTID = 0x100000,
    CLONE_CHILD_CLEARTID = 0x200000,
};
```

- `CLONE_THREAD` asks for a thread in the same process. Without it, `clone` is
  a request for a new process, the subject of
  [elfuse part 11](../../elfuse/11-processes-without-fork/), and mini-elfuse
  returns `ENOSYS`.
- `CLONE_SETTLS` passes the new thread pointer in `x3`.
- `CLONE_PARENT_SETTID` asks for the new thread id to be written at the
  address in `x2` before `clone` returns. glibc keeps the id there.
- `CLONE_CHILD_CLEARTID` gives an address, in `x4`, to set to 0 when the
  thread exits. glibc passes the same address, and `pthread_join` waits for it
  to become 0.

The other five flags (`CLONE_VM`, `CLONE_FS`, `CLONE_FILES`, `CLONE_SIGHAND`,
and `CLONE_SYSVSEM`) share memory, file system information such as the working
directory, open files, signal handlers, and System V semaphore undo values with
the parent. Threads in one mini-elfuse process share all of these already.
Thread ids start at 2, after the first thread: a process's first thread has the
process id, and elfuse's first process is 1
([elfuse part 11](../../elfuse/11-processes-without-fork/)).

## futex

A glibc mutex keeps its lock state in a 32-bit word in the program's memory.
Taking a free lock is an atomic update of that word, with no system call. When a
thread finds the lock taken, it calls `futex(&word, FUTEX_WAIT, expected)`:
sleep, but only if the word still holds `expected`. When the word shows a
waiter, the thread that unlocks calls `futex(&word, FUTEX_WAKE, 1)`. The check
and the sleep must be atomic, or the waiter can miss the wake meant for it
([elfuse part 10](../../elfuse/10-threads-futexes-and-the-clock/), "Waiting for
each other").

mini-elfuse makes them atomic with one host mutex, `lock`, and one condition
variable for all futex addresses:

```c
/* lock guards brk_cur, mmap_next and next_tid, and every futex wait and
 * wake: one condition variable stands for all futex addresses.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t futex_cond = PTHREAD_COND_INITIALIZER;
static int32_t next_tid = 2;
static __thread uint64_t clear_child_tid;
```

```c
    case NR_futex: {
        /* A woken waiter rechecks the value in user space, so waking every
         * waiter is correct, only slower. The timeout in a[3] is ignored.
         */
        int32_t *val = guest_ptr(a[0], 4);
        int op = a[1] & 0x7f; /* without FUTEX_PRIVATE_FLAG */
        pthread_mutex_lock(&lock);
        ret = 0;
        if (!val)
            ret = -LINUX_EFAULT;
        else if (op == FUTEX_WAKE)
            pthread_cond_broadcast(&futex_cond);
        else if (op != FUTEX_WAIT)
            ret = -LINUX_ENOSYS;
        else if (*val != (int32_t) a[2])
            ret = -LINUX_EAGAIN;
        else
            pthread_cond_wait(&futex_cond, &lock);
        pthread_mutex_unlock(&lock);
        break;
    }
```

`FUTEX_WAIT` compares the word and starts waiting while holding `lock`, and
`pthread_cond_wait()` releases `lock` only once the thread is waiting. A wake
takes `lock` before broadcasting, so it comes either before the comparison,
which then sees the changed word and returns `EAGAIN`, or after the waiter is
asleep. Waking every waiter on every address wakes threads that were not meant
to be woken; glibc checks the word again after every wake, so they wait
again.

elfuse keys wait queues by address in a hash table of 1024 buckets and uses
macOS's own address-wait call for plain waits (elfuse part 10). The timeout
that `FUTEX_WAIT` accepts is ignored here, so a timed wait lasts until a
wake.

The same `lock` now also guards `brk` and `mmap`, because two threads can call
them at once:

```c
        pthread_mutex_lock(&lock);
        uint64_t addr = mmap_next;
        void *p = guest_ptr(addr, a[1]);
        if (p)
            mmap_next += (a[1] + 0xfff) & ~0xfffULL;
        pthread_mutex_unlock(&lock);
```

## exit

`exit` from a thread made by `clone` now ends only that thread, and
`exit_group` ends the process:

```c
    case NR_exit:
        /* exit ends only the calling thread. mini-elfuse does not count
         * threads, so exit from the initial thread ends the process.
         */
        if (!pthread_main_np()) {
            int32_t *tid = guest_ptr(clear_child_tid, 4);
            pthread_mutex_lock(&lock);
            if (tid) {
                *tid = 0;
                pthread_cond_broadcast(&futex_cond);
            }
            pthread_mutex_unlock(&lock);
            HV(hv_vcpu_destroy(vcpu));
            pthread_exit(NULL);
        }
        /* fall through */
    case NR_exit_group:
        exit(a[0]);
```

A thread made by `clone` clears its `CLONE_CHILD_CLEARTID` word and wakes the
waiters, which is how `pthread_join` learns it ended. Then its host thread
destroys its vCPU and exits. `pthread_main_np()` is true on mini-elfuse's first
thread, which runs the program's first thread. Its `exit` still ends the
process, so `hello.S` from part 1 keeps working. On Linux the process would
continue until its last thread exits.

## Running it

```sh
$ build/mini-elfuse build/threads
counter 200000
```

The trace, with the output sent to a file and shown after the start-up calls:

```sh
$ build/mini-elfuse -v build/threads > trace.txt 2>&1
$ cat trace.txt
...
syscall 222(0x0, 0x210000, 0x0, 0x20022, 0xffffffffffffffff, 0x0)
  -> 268435456 (0x10000000)
syscall 226(0x10010000, 0x200000, 0x3, 0x880, 0x4a8700, 0x0)
  -> 0 (0x0)
syscall 220(0x3d0f00, 0x1020e980, 0x1020f150, 0x1020f780, 0x1020f150, 0x1020f780)
  -> 2 (0x2)
syscall 222(0x0, 0x210000, 0x0, 0x20022, 0xffffffffffffffff, 0x0)
  -> 270598144 (0x10210000)
syscall 226(0x10220000, 0x200000, 0x3, 0x880, 0x4a8700, 0x0)
  -> 0 (0x0)
syscall 220(0x3d0f00, 0x1041e980, 0x1041f150, 0x1041f780, 0x1041f150, 0x1041f780)
  -> 3 (0x3)
syscall 98(0x1020f150, 0x0, 0x2, 0x0, 0x1020f150, 0x1041f780)
syscall 99(0x1020f160, 0x18, 0x1020f080, 0x465ca0, 0x1020f7c0, 0x40)
  -> -38 (0xffffffffffffffda)
syscall 99(0x1041f160, 0x18, 0x1041f080, 0x465ca0, 0x1041f7c0, 0x40)
  -> -38 (0xffffffffffffffda)
syscall 98(0x4a1ab8, 0x80, 0x2, 0x0, 0x1041e850, 0xd2d9ec01742c3cac)
  -> -11 (0xfffffffffffffff5)
syscall 98(0x4a1ab8, 0x81, 0x1, 0x0, 0x1020e850, 0xd2d9ec01744d3cac)
  -> 0 (0x0)
syscall 98(0x4a1ab8, 0x81, 0x1, 0x0, 0x1041e850, 0xd2d9ec01742c3cac)
  -> 0 (0x0)
  -> 0 (0x0)
...
```

Lines from the three vCPUs interleave, so a result line does not always belong
to the call just above it.

- `clone` returns thread ids 2 and 3, and each new thread's first call is
  `set_robust_list` (99).
- The main thread's `futex` with op 0, `FUTEX_WAIT`, on `0x1020f150` is
  `pthread_join` waiting for thread 2: that address is the `parent_tid` and
  `child_tid` of the first `clone`, and 2 is the value it expects there.
- Op `0x80` is `FUTEX_WAIT` with the private flag, on the mutex at `0x4a1ab8`.
  One returns -11, `EAGAIN`: the word changed before the thread could wait.
  Op `0x81` is `FUTEX_WAKE` from the thread releasing the mutex.

The number of `futex` calls depends on how often the threads collide: 23 in
the run above, and from 3 to about 200 in other runs. The total is 200000 every
time, because every addition happens under the mutex. `make check` compares
it:

```make
	test "$$($(BUILD)/mini-elfuse $(BUILD)/threads)" = "counter 200000"
```

## What mini-elfuse leaves out

mini-elfuse ends here, at about 600 lines of C. The rest of
[the elfuse series](../../elfuse/01-what-happens-when-you-run-a-program/)
describes what a full implementation adds:

| Area | mini-elfuse | elfuse | elfuse part |
|---|---|---|---|
| memory | all program memory allows everything, in 2 MiB blocks | permissions per region, 4 KiB pages where they differ, TLB flushes | 6 |
| system calls | 16, in a `switch` | 224, a generated table, answers in the shim | 7 |
| files | paths and descriptors passed through | a sysroot, case-sensitive names on APFS, generated `/proc` files | 8 |
| signals | none; a fault ends mini-elfuse | signal frames, faults delivered as signals | 9 |
| threads | one condition variable, no timeouts | wait queues by address, the vDSO clock, a watchdog | 10 |
| processes | no `fork` or `execve` | `fork` as a second elfuse, `execve`, exit statuses | 11 |
| debugging | the fault's registers | crash reports, a GDB stub | 12 |
| x86_64 programs | none | Rosetta for Linux | 13 |
| testing | `make check` | a Linux kernel under QEMU as the reference, proofs | 14 |

## The code

The files are served with this site under `mini-elfuse/step-8/`:

```sh
mkdir step-8 && cd step-8
for f in Makefile mini-elfuse.c vectors.S entitlements.plist hello.S null.S printf.c files.c mmap.c threads.c; do
    curl -fsSO https://henrybear327.github.io/notes/mini-elfuse/step-8/$f
done
make check
```

`Makefile`:

{{< include-file "static/mini-elfuse/step-8/Makefile" "make" >}}

`mini-elfuse.c`:

{{< include-file "static/mini-elfuse/step-8/mini-elfuse.c" "c" >}}

`vectors.S`:

{{< include-file "static/mini-elfuse/step-8/vectors.S" "asm" >}}

`entitlements.plist`:

{{< include-file "static/mini-elfuse/step-8/entitlements.plist" "xml" >}}

`hello.S`:

{{< include-file "static/mini-elfuse/step-8/hello.S" "asm" >}}

`null.S`:

{{< include-file "static/mini-elfuse/step-8/null.S" "asm" >}}

`printf.c`:

{{< include-file "static/mini-elfuse/step-8/printf.c" "c" >}}

`files.c`:

{{< include-file "static/mini-elfuse/step-8/files.c" "c" >}}

`mmap.c`:

{{< include-file "static/mini-elfuse/step-8/mmap.c" "c" >}}

`threads.c`:

{{< include-file "static/mini-elfuse/step-8/threads.c" "c" >}}

## What we learned

- Each thread runs on its own vCPU, created by its own host thread, and starts
  from its parent's general-purpose registers with a new `x0`, stack pointer,
  and thread pointer.
- `clone` writes the new thread id where glibc keeps it and remembers where to
  clear it at exit, which is what `pthread_join` waits on.
- A futex wait is correct as long as the comparison and the sleep happen under
  the same lock that every wake takes; one condition variable for all
  addresses is enough.
- mini-elfuse's shared state (the heap end, the `mmap` counter, and the thread
  ids) needs that lock once two vCPUs make system calls at the same time.
