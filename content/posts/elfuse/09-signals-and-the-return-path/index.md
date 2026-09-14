---
title: 'elfuse 9: Signals and the return path'
date: 2026-09-09T18:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "signals"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 9 of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 8](../08-files-and-paths/) covered the calls that take a path. This part
is about signals: how elfuse interrupts a program to run one of its functions,
and how the program then resumes where it stopped.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac.

## The problem

A signal is a notification delivered to a process: a child process exited, a
pipe's reader went away, a timer expired, the user pressed Ctrl-C, or the
program accessed memory it may not use. A program can install a handler, a
function the kernel calls when the signal arrives. It can also block signals,
which holds them back until it unblocks them; the set of blocked signals is its
signal mask.

A signal can arrive at any instruction. To run a handler, the kernel has to save
every register the program was using, make the program jump to the handler, and
afterwards put every register back so the interrupted code continues. On Linux
the kernel does that. Under elfuse there is no Linux kernel, and during a
system call the shim also holds a saved copy of the program's registers
(part 5).

## A handler that runs and returns

This program sends itself `SIGUSR1` and reports where its stack is in `main`
and in the handler:

```c
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static void on_signal(int sig, siginfo_t *info, void *ctx)
{
    (void) ctx;
    printf("handler: signal %d from pid %d, stack near %p\n", sig,
           (int) info->si_pid, (void *) &sig);
}

int main(void)
{
    struct sigaction sa = {.sa_sigaction = on_signal, .sa_flags = SA_SIGINFO};
    sigaction(SIGUSR1, &sa, NULL);

    long kept = 12345;
    printf("main: stack near %p\n", (void *) &kept);
    fflush(stdout);
    kill(getpid(), SIGUSR1);
    printf("main: back, kept = %ld\n", kept);
    return 0;
}
```

```sh
$ aarch64-linux-gnu-gcc -static -O2 -o signals signals.c
$ build/elfuse ./signals
main: stack near 0x7ffc1e0
handler: signal 10 from pid 1, stack near 0x7ffaf7c
main: back, kept = 12345
```

The handler ran with signal 10, `SIGUSR1`, sent by process 1, the guest's own
id. Its stack is about 4.7 KB below `main`'s: that space holds the signal frame
described next. `main` continued with its local variable intact.

## Building the signal frame

`src/syscall/signal.c` delivers a signal the way Linux's arm64 kernel does
([signal.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/signal.c#L8-L13)):

> When a signal is queued (e.g., SIGPIPE from write() to broken pipe), signal
> emulation builds an rt_sigframe on the guest stack matching the kernel's
> setup_rt_frame layout, then redirects the vCPU to the guest's signal
> handler. The guest handler eventually calls rt_sigreturn (SYS 139), which
> restores the saved register state from the frame.

The signal frame is a block written onto the program's own stack, below its
current stack pointer. It holds a `siginfo` record (which signal, who sent it)
and a `ucontext` record: every general register, the stack pointer, the program
counter, the processor state, the signal mask, and the floating-point
registers. Because the layout matches Linux's, a handler that inspects
the `ucontext` argument sees what it would see on Linux.

Then elfuse points the vCPU at the handler
([signal.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/signal.c#L2353-L2387)):

- the stack pointer becomes the frame's address;
- the address `eret` returns to becomes the handler;
- `x0` holds the signal number, and with `SA_SIGINFO`, `x1` and `x2` point at
  the `siginfo` and `ucontext` records;
- `x30`, the link register a function returns through, points at a helper in
  the vDSO, the page of helper code from part 4.

The helper is three instructions: `mov x8, #139; svc #0; ret`. When the handler
returns, it lands there and makes system call 139, `rt_sigreturn`. A C library
can also supply a return address of its own when it installs a handler, and
Linux on arm64 uses it when the library asks. elfuse always uses the vDSO
helper instead, which makes the same call
([signal.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/signal.c#L2373-L2379)).

`rt_sigreturn` reads the frame back and restores every register, and the
program resumes at the instruction after `kill`. Before restoring anything, it
checks a random value stored in the frame and refuses a saved program counter
that points into the shim's memory, so a forged frame cannot send execution into
EL1 code
([signal.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/signal.c#L2599-L2626)).

With `-v` (timestamps removed), the trace shows the return going through the
helper, which the vDSO places at `0xf0e0`:

```text
DEBUG src/syscall/syscall.c:2807: syscall 129@0x405a64(0x1, 0xa, 0xfbad2884, 0x1b, 0x5e8, 0x1b)
DEBUG src/syscall/syscall.c:2853:   -> 0 (0x0)
DEBUG src/syscall/proc.c:4567: elfuse: [13] vcpu_run PC=0xfeffdf7bf0
DEBUG src/syscall/proc.c:4567: elfuse: [14] vcpu_run PC=0xfeffdf7bf0
DEBUG src/syscall/proc.c:4029: elfuse: HVC #5
DEBUG src/syscall/syscall.c:2807: syscall 139@0xf0e4(0x34, 0x492440, 0x1, 0x1fcd, 0x1, 0x490270)
DEBUG src/syscall/proc.c:4078: elfuse: post-sigreturn state: pending=0x0 global_blocked=0x0 thread_blocked=0x0 signal_pending=0
```

Syscall 129 is `kill` with signal `0xa`, 10. The handler runs between that call
and syscall 139, whose `svc` is at `0xf0e4`, the second instruction of the
helper.

## The shim's saved frame

`kill` is a system call, so when elfuse builds the signal frame, the shim still
has the 256-byte copy of the program's registers it saved on the way in. On the
normal return path the shim restores `x1` to `x30` from that copy (part 5). Here
that would overwrite the handler's `x1`, `x2`, and `x30` with the values from
before the call.

elfuse prevents that with the `x8` request from part 5. Value 2 means "discard
the saved frame and `eret` with the registers as they are". The file's header
lists three places that replace a program's registers
([signal.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/signal.c#L15-L38)):

| What replaces the registers | Shim holds a saved frame? | `x8` |
|---|---|---|
| a signal delivered on the way back from a system call | yes | 2 |
| `rt_sigreturn` | yes, from the `rt_sigreturn` call itself | 2 |
| a signal delivered while the program was running at EL0 | no | not used |

The third row covers a program that is computing, not making calls. To
interrupt it, elfuse forces the vCPU to exit. The registers are then in the
vCPU itself, with no shim frame, so elfuse writes the new program counter and
stack pointer directly. `execve`, which replaces the whole program, needs no
marker either, because it restarts the shim from its first instruction (part
11).

![Two stacks during signal delivery. On the program's EL0 stack, main's frame sits above the signal frame, which holds siginfo and a ucontext with the saved registers, and the handler's frame is below it. On the shim's EL1 stack, the saved frame of the kill call is crossed out because x8 = 2 makes the shim discard it. Five steps: elfuse writes the signal frame, erets into the handler, the handler returns into the vDSO, rt_sigreturn reads the frame, and main continues after kill.](figure-7.svg)

## Faults become signals

A program that reads address 0 gets `SIGSEGV`, whose default action is to end
the process:

```c
int main(void)
{
    volatile int *p = 0;
    return *p;
}
```

```sh
$ aarch64-linux-gnu-gcc -static -O0 -o segv segv.c
$ build/elfuse ./segv; echo "exit=$?"
exit=139
$ build/elfuse -v ./segv 2>&1 | sed 's/^[0-9:]* //' | grep -E 'HVC #11|data fault'
DEBUG src/syscall/proc.c:4029: elfuse: HVC #11
DEBUG src/syscall/proc.c:3849: elfuse: EL0 data fault at 0x0 PC=0x40072c (ESR=0x92000007 FSC=0x7) -> SIGSEGV/MAPERR
```

The read faulted at EL0, and the processor entered the shim's handler table
from part 5. The shim forwarded the fault with `hvc #11`, and elfuse turned it
into `SIGSEGV`. With no handler installed, the process ended, and elfuse exits
with 139, which is 128 plus the signal number 11, the status a Linux shell
reports for a process killed by `SIGSEGV`.

## Signals from outside

Signals also come from outside the guest, and elfuse is a macOS process that
receives macOS signals. It uses two of them internally: `SIGUSR2`, which
another elfuse process sends to announce a guest signal for this one, and
`SIGALRM`, the watchdog's timer signal (part 10). Both are blocked on
every thread and taken by one dedicated thread that never runs a vCPU
([proc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/proc.c#L2883-L2893)).
The source explains:

> The reason is Apple HVF: when either signal is delivered to a vCPU thread
> while it is inside hv_vcpu_run, the run aborts with HV_EXIT_REASON_UNKNOWN
> instead of the clean HV_EXIT_REASON_CANCELED that hv_vcpus_exit() produces
> for a vCPU caught between runs.

That thread records what happened and, when a vCPU has to act, asks the vCPU
threads to exit. Each vCPU thread then delivers any guest signal itself, which
fits the rule from part 2 that a vCPU's registers are changed only from its own
thread.

elfuse also ignores the macOS `SIGPIPE` and catches `SIGBUS` for its own memory
access. Other macOS signals, such as `SIGINT`, keep their macOS default action.
Pressing Ctrl-C sends
`SIGINT` to the processes in the terminal's foreground, and that includes
elfuse. I sent `SIGINT` to elfuse while it ran a Busybox shell script with
`trap "echo guest handler ran" INT`: the elfuse process ended at once and the
guest's trap did not run. A Linux program cannot catch Ctrl-C under elfuse at
this commit.

The guest's own real-time timer is kept inside elfuse for a related reason. On
macOS, `alarm()` and `setitimer(ITIMER_REAL)` share one timer per process, and
elfuse's watchdog already uses it
([signal.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/signal.c#L232-L236)).
A guest `alarm()` records an expiry time, and elfuse queues `SIGALRM` when it
notices the time has passed.

## Running a call again

A signal handler is one way elfuse changes where the program resumes. The
other is re-running a system call. On `svc` entry, `ELR_EL1` holds the address
after the 4-byte `svc` instruction. Subtracting 4 makes `eret` land on the
`svc` again, with `x8` still holding the syscall number, so the program repeats
the call
([syscall.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/syscall.c#L2503-L2514)).

elfuse uses this only when it interrupted a waiting call itself, in
the case part 11 describes, where Linux would not have interrupted it. When
the next call arrives, elfuse checks that it is the repeated one: the
address and the syscall number must both match. The address alone is not
enough, because the shim can answer a repeated call without an exit (part 7),
and a later, different call from the same `svc` instruction in the C library
would then look like the repeat
([syscall.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/syscall.c#L2555-L2560)).

## What we learned

- elfuse delivers a signal by writing a Linux-format signal frame onto the
  program's stack and pointing the vCPU at the handler, with `x30` pointing at
  the vDSO helper that calls `rt_sigreturn`.
- `rt_sigreturn` restores every register from the frame after checking it.
- When a signal or `rt_sigreturn` replaces the registers during a system call,
  elfuse sets `x8` to 2 so the shim discards its saved frame.
- Memory faults at EL0 reach elfuse through the shim and become `SIGSEGV`. Most
  macOS signals sent to elfuse keep their macOS default action, so Ctrl-C ends
  elfuse without running a guest handler.

[Part 10](../10-threads-futexes-and-the-clock/) is about threads: what a thread
costs when each one is a virtual CPU, how threads wait for each other, and how
a program reads the time without a system call.
