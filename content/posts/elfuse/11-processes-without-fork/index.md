---
title: 'elfuse 11: Processes without fork'
date: 2026-09-09T20:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "processes"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 11 of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 10](../10-threads-futexes-and-the-clock/) covered threads inside one
process. This part is about making new processes: `fork`, `execve`, and waiting
for a child to finish, on a hypervisor that allows one virtual machine per
macOS process.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac.

## The problem

Linux programs start other programs in two steps. `fork` creates a copy of the
calling process: a child with the same memory, the same open files, and the same
registers, whose main visible differences are its own PID, its parent's PID, and
the value `fork` returns. The child then usually calls `execve`, which replaces
the running program with a different one in the same process. The parent calls
`wait` or `waitpid` to learn how the child ended. Each process has a process id,
a PID. A child that has exited but has not yet been waited for is a zombie: it
keeps only its PID, exit status, and resource usage until its parent collects
it.

A shell does this for every external command it runs. Under elfuse, a constraint
from part 2 applies: Hypervisor.framework allows one VM per macOS process, and a
copy of a guest needs a VM of its own.

## A fork, from both sides

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    int shared = 42;
    pid_t child = fork();
    if (child == 0) {
        shared++;
        printf("child: pid %d, parent %d, shared = %d\n", getpid(), getppid(), shared);
        fflush(stdout);
        sleep(2);
        exit(7);
    }
    int status;
    waitpid(child, &status, 0);
    printf("parent: pid %d, child %d exited with %d, shared = %d\n", getpid(),
           child, WEXITSTATUS(status), shared);
    return 0;
}
```

```sh
$ aarch64-linux-gnu-gcc -static -O2 -o fork fork.c
$ build/elfuse ./fork
child: pid 2, parent 1, shared = 43
parent: pid 1, child 2 exited with 7, shared = 42
```

Linux semantics hold: the child sees its own PID and its parent's, its change
to `shared` does not reach the parent, and the parent collects exit status 7.
From the Mac's side, listing the processes while the child sleeps shows two
elfuse processes (home directory shortened):

```text
  PID  PPID ARGS
 4849  4109 fork (aarch64-linux)
 4850  4849 ~/elfuse-blog.noindex/wt/build/elfuse --fork-child 8
```

The first is the parent. elfuse renames its macOS process after the guest
program. The second is a new elfuse, started by the first, holding the child.
With `-v`, a separate run logs the fork (selected lines, timestamps removed):

```text
DEBUG src/syscall/syscall.c:2306: clone(flags=0x1200011, stack=0x0, ptid=0x0, tls=0x0, ctid=0x10000d0)
DEBUG src/runtime/forkipc.c:1565: clone(flags=0x1200011, vfork=0)
DEBUG src/runtime/forkipc.c:161: fork-child: pid=2 ppid=1
DEBUG src/core/guest.c:696: guest: CoW fork: mapped 1024 GiB from shm (ipa=40 bits)
DEBUG src/runtime/forkipc.c:223: fork-child: CoW fork via shm fd
DEBUG src/runtime/forkipc.c:2070: clone: child pid=2 (host=77712)
DEBUG src/runtime/forkipc.c:548: fork-child: entering vCPU loop
```

The C library's `fork` is the `clone` system call, and the lines come from both
processes, interleaved.

## Starting a second elfuse

`src/runtime/forkipc.c` states the approach at the top of its header comment
([forkipc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/forkipc.c#L8-L10)):
"macOS HVF allows only one VM per process, so fork spawns a new elfuse process
and serializes the full VM state (registers, memory, FDs) over a socketpair."

A socketpair is two connected sockets, one for each end, so the two processes
can send each other bytes. It can also carry open file descriptors: a message
with `SCM_RIGHTS` hands a copy of a descriptor to the process at the other end.
That is how the child gets the guest's open files.

The sequence
([Fork, Clone, And execve](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#fork-clone-and-execve)):

1. The parent creates the socketpair and reserves a guest PID for the child, so
   a child can never run untracked.
2. It starts `elfuse --fork-child <fd>` with `posix_spawn`, the POSIX call for
   starting a program as a new process
   ([forkipc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/forkipc.c#L1636-L1706)).
3. It pauses its other threads, so the child cannot receive memory that another
   thread is halfway through changing. Only the calling thread exists in the
   child, as POSIX requires
   ([forkipc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/forkipc.c#L1742-L1748)).
4. It sends the state: registers, the file descriptor table, process ids,
   memory regions. The stream's header starts with a magic number that spells
   `ELFQ`, which is bumped whenever the format changes incompatibly, so a child
   refuses a stream it cannot read
   ([fork-state.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/fork-state.h#L19-L22)).
5. The child creates its own VM, restores the calling thread's registers with
   `x0` set to 0, and starts running at EL0.

Step 5 does not go through the shim's start-up code from part 5, for a reason
the source gives
([forkipc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/forkipc.c#L403-L406)):
"The shim entry zeros ALL GPRs before ERET, which would destroy callee-saved
registers (X19-X28, FP, LR) that the guest expects preserved across the clone()
syscall". FP and LR are `x29` and `x30`.

## Memory at fork

Registers and descriptors are small; the guest's memory is not. elfuse avoids
copying it, using copy-on-write from part 6.

At start-up, elfuse backs the slab with an unlinked temporary file, mapped
`MAP_SHARED`: the `cow_shm_upgrade` row in part 3's timing. At fork, it makes an
APFS clone of that file with `fclonefileat`. A clone is a new file that shares
all of the original's blocks on disk until one of them is written, so making it
is mostly a metadata update. The parent sends the clone's descriptor over the
socketpair, and the child maps it `MAP_SHARED`. The clone is its own file, so
the child's writes land in it and never reach the parent's file. The log line
`CoW fork: mapped 1024 GiB from shm` above is that mapping.

The parent does not remap its own slab, for a reason the source gives
([forkipc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/forkipc.c#L1775-L1778)):
"hv_vm_map caches the host VA->PA mapping, and a MAP_FIXED remap invalidates it
(the parent then reads stale memory and writev returns EFAULT)."

![Two elfuse processes. The parent, guest pid 1, holds one VM with vCPUs for its main thread and two more threads, and a slab mapped MAP_SHARED on an unlinked file. It starts the child with posix_spawn and sends registers, the fd table, and a descriptor over a socketpair. The child, started as elfuse --fork-child with guest pid 2, has a VM of its own with one vCPU, the thread that called fork with x0 = 0. The parent's slab file is cloned with fclonefileat, and the child maps the clone MAP_SHARED.](figure-8.svg)

If the clone fails, for example because `/tmp` is not on APFS, a native guest
gets the parent's live file instead, and an x86_64 guest run through Rosetta
(part 13) falls back to copying memory over the socket in 1 MiB pieces
([forkipc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/forkipc.c#L1830-L1851)).
`docs/internals.md` says this copy-on-write path is "roughly 50x faster than the
legacy IPC copy path on large guest memories".

A fork still starts a whole macOS process. This loop forks and waits 50 times:

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 50; i++) {
        pid_t c = fork();
        if (c == 0)
            _exit(0);
        waitpid(c, NULL, 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    printf("50 forks: %.1f ms each\n", ms / 50);
    return 0;
}
```

```sh
$ build/elfuse ./forkloop
50 forks: 114.2 ms each
```

Three runs gave 113 to 116 ms per fork, on a Mac that was also running other
work (load average about 9). A shell script spends that time on every external
command it runs.

## Replacing the program

`execve` does not need a new process. elfuse loads the new program into the
same VM, and the guest PID stays the same, as on Linux (the sysroot warning
from part 4 is left out):

```sh
$ build/elfuse --sysroot externals/test-fixtures/rootfs /bin/busybox sh -c \
    'echo "before exec: pid $$"; exec /bin/busybox sh -c "echo after exec: pid \$\$"'
before exec: pid 1
after exec: pid 1
```

`sys_execve()` in `src/syscall/exec.c` closes the descriptors marked
close-on-exec, rebuilds guest memory and page tables with the loader from part
4, and restarts the vCPU at the shim's first instruction, with the MMU off, as
at boot. The source explains why it cannot resume the vCPU where it stopped
([exec.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/exec.c#L73-L85)):
rebuilding reuses the page-table pages, so "the vCPU's hardware TLB can still
hold walk-cache entries that point into recycled page-table pages", and the
only safe moment to flush them is with translation off, which is how `_start`
begins.

`execve` from a thread other than the main one is harder. Linux destroys all the
other threads and keeps going on the calling thread. In elfuse, the process
exits when the main thread's run loop ends, so a non-main thread hands its
`execve` to the main thread through a one-slot state machine with four states:
empty, published, taken, done
([exec.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/exec.c#L716-L761)).
If the `execve` succeeds, the thread that asked no longer exists, so the main
thread clears the request itself. If it fails, the result stays in the slot
for the asking thread to read.

If the main thread was waiting in a system call when the request came, elfuse
interrupts that wait. Should the `execve` then fail, the interrupted call is
re-run with the mechanism from part 9, because on Linux it would never have been
interrupted. A wait that had already used part of a relative timeout usually
returns `EINTR` instead.

## Exiting and waiting

`exit_group`, the call behind `exit()`, ends every thread. Its handler marks the
process as exiting, wakes anything sleeping, and forces every vCPU to exit. It
does not wait for them itself; the main thread does, after its run loop ends
([syscall.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/syscall.c#L935-L945)).

A parent may exit before its children. On Linux, an orphan is adopted by its
nearest subreaper ancestor, or else by process 1, the init process, which waits
for it so it does not stay a zombie. Under elfuse the first guest process is
PID 1, so it is that fallback for its descendants
([Process Lifecycle And Guest PID 1](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#process-lifecycle-and-guest-pid-1)).
elfuse processes in one run share a small registry file that records each
child's exit status until its parent, or its adopter, collects it. The docs note
one limit: a program run directly as PID 1 that never waits for adopted
children leaves their status in that registry, because elfuse adds no hidden
init process.

## What we learned

- Hypervisor.framework allows one VM per process, so elfuse forks by starting a
  second elfuse with `posix_spawn` and sending it the guest's state over a
  socketpair, including open files.
- Guest memory is passed as an APFS clone of the file behind the slab, which
  shares blocks with the parent's file until either side writes, so the slab
  itself is not copied at fork time.
- `execve` stays in the same process and VM and restarts the vCPU at the shim's
  first instruction. An `execve` from another thread is handed to the main
  thread.
- The first guest process is PID 1 and adopts orphans that have no closer
  subreaper, with exit statuses kept in a registry shared by the run's elfuse
  processes.

[Part 12](../12-debugging-a-guest/) is about debugging: what elfuse prints when
something breaks, and how GDB or LLDB can attach to a guest whose registers only
the vCPU's own thread may access.
