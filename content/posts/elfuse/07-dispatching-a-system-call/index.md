---
title: 'elfuse 7: Dispatching a system call'
date: 2026-09-09T16:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "syscalls"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 7 of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 5](../05-booting-a-vm-with-no-kernel/) followed a system call from the
program to the shim and out through `hvc #5`, and
[part 3](../03-from-launch-to-exit/) named the functions on the other side.
This part covers that side: how elfuse finds the handler, how it converts macOS
results into Linux ones, and which calls the shim answers without an exit.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac.

## The problem

elfuse handles 224 different system calls. Each takes its arguments in its own
way, and most have to be carried out with macOS calls whose numbers, flags,
structures, and error codes differ from Linux's. A handler also has to be fast,
because a system call that reaches elfuse costs an exit and a return to the
guest. A comment in `proc.c` records the time for that
([proc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/proc.c#L2907-L2909)):
"a 1226 ns bare HVF round trip", HVF being short for Hypervisor.framework. That
time does not include the handler's own work.

This post uses one small program that makes three kinds of call:

```c
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
    for (int i = 0; i < 1000; i++)
        getpid();

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(1)};
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *) &to, sizeof(to)) < 0)
        printf("connect: errno %d (%s)\n", errno, strerror(errno));

    if (syscall(425 /* io_uring_setup */, 1, NULL) < 0)
        printf("io_uring_setup: errno %d (%s)\n", errno, strerror(errno));
    return 0;
}
```

It asks for its process id a thousand times, opens a socket, the endpoint a
program uses for network connections, and connects to a TCP port on which
nothing listens, and calls `io_uring_setup`, a Linux system call elfuse does
not have. The toolchain's glibc headers have no `SYS_io_uring_setup` name, so
the number is written out. Built as a static program and run:

```sh
$ aarch64-linux-gnu-gcc -static -O2 -o journey journey.c
$ build/elfuse ./journey
connect: errno 111 (Connection refused)
20:59:44 WARN  src/syscall/syscall.c:2841: unimplemented syscall 425 (x0=0x1, x1=0x0, x2=0x0, x3=0x2, x4=0x490270, x5=0x451117)
io_uring_setup: errno 38 (Function not implemented)
```

## The dispatch table

The list of system calls elfuse handles is one text file,
[`src/syscall/dispatch.tbl`](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/dispatch.tbl),
one line per call:

```text
# Format:
#   SYS_<name> sc_<wrapper> <extra>
...
SYS_write sc_write 0
```

The third column is 0 when the wrapper needs only the first three arguments and
1 when it may use more. At build time, `scripts/gen-syscall-dispatch.py` turns
the file into `build/dispatch.h`, a list in the form of a C macro:

```c
#define SYSCALL_TABLE_ENTRIES(_)                                   \
...
    _(SYS_write, sc_write, 0)          \
```

`src/syscall/syscall.c` expands that list into the dispatch table, an array
indexed by syscall number
([syscall.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/syscall.c#L2380-L2385)):

```c
static const syscall_entry_t syscall_table[SC_TABLE_SIZE] = {
#define _(nr, sc_handler, extra) \
    [nr] = { .handler = sc_handler, .needs_extra_regs = (extra) != 0 },
    SYSCALL_TABLE_ENTRIES(_)
#undef _
};
```

`[nr] = {...}` puts each entry at the position of its number, `SYS_write` being
64, and every number without a line stays empty. The generator refuses a name
it does not know, a duplicate line, and a wrapper that does not exist in
`syscall.c`.

## Wrappers

A wrapper casts the six register values to a handler's argument types and calls
the handler. Most, 187 of 224, are a single use of one of three macros
([syscall.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/syscall.c#L162-L164)):

```c
 *   SC_FORWARD(name, expr): cast args and forward to sys_xxx / signal_xxx
 *   SC_LOCKED(name, expr):  same, but hold mmap_lock during the call
 *   SC_STUB(name, val):     return a constant (alias for SC_FORWARD)
```

`sc_write` is `SC_FORWARD(sc_write, sys_write(g, (int) x0, x1, x2))`. The file
has 172 uses of `SC_FORWARD`, 6 of `SC_LOCKED`, and 9 of `SC_STUB`. The locked
ones, such as `mprotect`, hold a lock so that no other thread of the program
changes the memory layout during the call (threads are part 10). The stubs
return a fixed answer: `mlock`, which asks the kernel to keep pages in memory,
returns 0 without doing anything.

## The dispatcher

`syscall_dispatch()` runs for every `hvc #5`
([syscall.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/syscall.c#L2543)).
In order:

1. It reads the syscall number from `x8`.
2. If `-v` is off, a few calls take a fast path. Process-identity calls and a
   few other simple ones that reach elfuse are answered directly, and `read`
   and `write` on ordinary files, pipes (one-way channels between processes),
   and sockets go straight to the macOS file descriptor without the table when
   nothing special applies.
3. Otherwise it looks up the number in the table.
4. It reads the arguments. Reading a register is itself a Hypervisor.framework
   call, so `x0` to `x2` are always read and `x3` to `x5` only when the table's
   third column is 1, when the slot is empty (so the warning
   can show them), or under `-v`
   ([syscall.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/syscall.c#L2787-L2795)).
   Without `-v`, a wrapper that uses `x3` behind a 0 in the table gets 0.
5. It calls the wrapper.
6. For an empty slot it logs a warning and returns `-ENOSYS`
   ([syscall.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/syscall.c#L2834-L2843)).
7. It writes the result into `x0` and the flush request into `x8` (part 6).

With `-v` (timestamps removed), the `io_uring_setup` call shows steps 1, 6, and
7:

```text
DEBUG src/syscall/syscall.c:2807: syscall 425@0x418c30(0x1, 0x0, 0x1fda, 0x2, 0x490270, 0x451117)
WARN  src/syscall/syscall.c:2841: unimplemented syscall 425 (x0=0x1, x1=0x0, x2=0x1fda, x3=0x2, x4=0x490270, x5=0x451117)
DEBUG src/syscall/syscall.c:2853:   -> -38 (0xffffffffffffffda)
```

`-38` is `-ENOSYS`. The C library's `syscall()` wrapper turns a negative return
into `-1` with `errno` set to 38, which is the program's second line of output.
A Linux program that checks for `io_uring` and falls back to another method
works the same way here as on an old Linux kernel.

## Translating results

`connect` is carried out with the macOS `connect`. macOS reports a refused
connection as `errno` 61; Linux calls it 111. The Linux program printed 111,
and `-v` shows elfuse returning it for syscall 203, `connect`:

```text
DEBUG src/syscall/syscall.c:2807: syscall 203@0x419edc(0x3, 0x7ffc270, 0x10, 0x3, 0x7ffc280, 0xd98be6e190d4bb9d)
DEBUG src/syscall/syscall.c:2853:   -> -111 (0xffffffffffffff91)
```

Handlers convert with `linux_errno()` in `src/syscall/translate.c`, built on a
table of 42 errors whose numbers differ, plus a few separate cases
([translate.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/translate.c#L28-L71)).
An excerpt:

```c
#define ERRNO_MAP(_)                                                     \
    /* POSIX divergent values */                                         \
    _(EDEADLK,         LINUX_EDEADLK)         /* mac 11 -> linux 35 */   \
    _(EAGAIN,          LINUX_EAGAIN)          /* mac 35 -> linux 11 */   \
...
    _(ECONNREFUSED,    LINUX_ECONNREFUSED)    /* mac 61 -> linux 111 */  \
    _(ELOOP,           LINUX_ELOOP)           /* mac 62 -> linux 40 */   \
...
    _(ENOSYS,          LINUX_ENOSYS)          /* mac 78 -> linux 38 */   \
```

Numbers 1 to 34 are the same on both systems, except that macOS uses 11 for
`EDEADLK` where Linux uses 11 for `EAGAIN`. An error with no entry and a number
above 34 becomes `EINVAL` ("invalid argument")
([translate.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/translate.c#L120-L130)).
`linux_errno()` returns the negative Linux number, ready for `x0`.

Error numbers are the simplest case. Flags have different bit values, such as
the `AT_*` flags on calls like `unlinkat`, and structures have different
layouts, such as the `struct stat` that `stat` fills in. Each of those has its
own conversion, and `docs/internals.md` describes several under
[Syscall Translation Boundary](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#syscall-translation-boundary).

## Calls the shim answers

The program asked for its process id a thousand times. None of those calls
reached elfuse. `ELFUSE_SHIM_STATS=1` prints the shim's counters when the
program exits:

```sh
$ ELFUSE_SHIM_STATS=1 build/elfuse ./journey
...
shim-stats (pid=1)
  ATTN_BAIL            0
...
  IDENTITY_HIT         1000
  URANDOM_HIT          0
...
```

`IDENTITY_HIT` counts calls the shim answered from its identity cache: the
process id, parent process id, user and group ids, and thread id. elfuse writes
all but the thread id into the shim's data block (part 5) and the thread id into
the system register `CONTEXTIDR_EL1`, and the shim reads them without an exit
([shim.S](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/shim.S#L461-L475)):

```asm
identity_class_fast:
    mrs x12, tpidr_el1               /* shim-globals base */
    ldar w13, [x12]                  /* attention flag, acquire */
    cbnz w13, attn_bail              /* slow-path required */
    cmp x11, #6                       /* bias == 6 ==> gettid (178) */
    b.eq gettid_fast
    add x12, x12, #8                 /* skip attention -> identity[0] */
    ldr x0, [x12, x11, lsl #3]       /* identity[bias] for 172..177 */
    COUNTER_INC CB_IDENTITY_HIT
    b svc_restore_eret
```

`tpidr_el1` holds the address of the data block. The first word there is the
attention flag. When it is set, the fast paths are skipped and the call exits
to elfuse. elfuse sets it while the cached user and group ids change, while
a signal (part 9) is waiting, an interval timer is armed, or the process is
exiting, while a debugger's interrupt is pending, and for the whole run under
`-v`
([shim-globals.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/shim-globals.h#L60-L71)).
With `-v`, all thousand calls go through elfuse, so the log can show them:

```sh
$ build/elfuse -v ./journey 2>&1 | grep -c 'syscall 172@'
1000
```

The shim answers a few other calls the same way. `getpgid(0)` and `getsid(0)`
read two more cached values. `read` from `/dev/urandom`
and `getrandom` of up to 256 bytes come out of a 4096-byte store of random
bytes that elfuse refills. `futex`, the call threads use to wait for each other,
gets an answer at EL1 when waiting would be pointless; part 10 has the
measurements behind that one.

## Syscall coverage check

`scripts/check-syscall-coverage.py` runs as part of `make check` and fails when
a name in the table appears nowhere in `tests/`, either as a call like `write(`
in C code or as a number like `SYS_write`. It is a text search, as its docstring
says: "Best-effort syscall coverage audit for dispatch.tbl against tests/". It
accepts libc names for calls whose C wrapper has a different name, such as
`pread` for `pread64`, and 14 listed exceptions with a reason each
([check-syscall-coverage.py](https://github.com/sysprog21/elfuse/blob/73d8246b/scripts/check-syscall-coverage.py#L16-L50)).
The check also fails if `tests/` references a call on the exception list in
one of those forms, so an unneeded exception cannot later hide a removed test.

## What we learned

- `dispatch.tbl` lists the 224 system calls elfuse handles. A generator turns
  it into a table indexed by syscall number, and a wrapper per call, usually
  one line, unpacks the registers.
- `syscall_dispatch()` skips `x3` to `x5` for calls the table marks as needing
  only three arguments, since every register read costs a framework call, and
  returns `-ENOSYS` for numbers with no entry.
- Handlers translate macOS results into Linux ones: error number 11 and those
  above 34, flags, and structure layouts all differ.
- The shim answers identity calls, small random reads, and some futex calls
  without an exit. An attention flag sends them to elfuse when a cached answer
  might be stale, a signal, timer, debugger stop, or exit is pending, or `-v`
  needs to log them.

[Part 8](../08-files-and-paths/) follows the calls that take a path: how
`/etc/passwd` inside the guest becomes a file on the Mac, and why file names
that differ only in case need special handling.
