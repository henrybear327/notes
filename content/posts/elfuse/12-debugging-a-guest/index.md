---
title: 'elfuse 12: Debugging a guest'
date: 2026-09-09T21:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "debugging", "gdb"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 12 of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 11](../11-processes-without-fork/) covered processes. This part covers
what elfuse reports when it fails and how a debugger can stop, inspect, and step
a Linux program running inside it.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac.

## The problem

A debugger stops a program, reads and changes its registers and memory, and lets
it continue, one instruction or up to a breakpoint. On Linux, debuggers do this
through `ptrace`, a system call that lets one process control another. Under
elfuse, the program's registers live in a vCPU, and part 2's rule applies: only
the thread running that vCPU may read or change them. A macOS debugger attached
to the elfuse process would see elfuse's own code, not the Linux program.

There are two kinds of failure to tell apart. When the Linux program crashes,
for example by reading address 0, that is a Linux signal (part 9), and the
program ends as it would on Linux. When elfuse or the shim hits a state it
cannot handle, elfuse itself stops, and it prints a crash report.

## A crash report

The simplest way to see one is the watchdog from part 10. This program loops
forever without a system call:

```c
int main(void)
{
    volatile unsigned long n = 0;
    for (;;)
        n++;
}
```

With `--timeout 1`, the watchdog checks once a second and stops a run that two
checks in a row find unfinished (the path to elfuse and the home directory
shortened):

```sh
$ build/elfuse --timeout 1 ./spin; echo "exit=$?"
20:13:16 ERROR src/syscall/proc.c:4596: elfuse: vCPU execution timed out after 1s
20:13:16 ERROR src/syscall/proc.c:4602: elfuse: timeout state: PC=0x4005b0 CPSR=0x60000000
20:13:16 ERROR src/syscall/proc.c:4613: elfuse: ESR_EL1=0x56000000 FAR_EL1=0x0 ELR_EL1=0x43a60c SCTLR_EL1=0x34d0d985

╔══════════════════════════════════════════════════════════╗
║                   elfuse crash report                    ║
╚══════════════════════════════════════════════════════════╝

## Environment
- elfuse version: 73d8246b
- macOS: 26.6.2 (Darwin 25.6.0)
- hardware: Apple M4

## Crash
- type: TIMEOUT

## Binary
- path: ~/elfuse-blog.noindex/demos/./spin
- cmdline: ./spin

## Memory layout
guest_size  = 0x10000000000 (1048576 MB, 40-bit IPA)
...

## How to report
File an issue at:
  https://github.com/sysprog21/elfuse/issues/new
...
exit=124
```

The report is written as a bug report, with Markdown headings, ready to paste
into a GitHub issue. The run took about two seconds, because the watchdog acts
on the second tick that finds the same run unfinished. The exit status is 124.
The three lines above the report say where the vCPU was: at `PC=0x4005b0`,
inside the loop.

The report can also print the vCPU's registers and a walk of the page tables
for the program counter, but this one did not. It prints them only when the
vCPU handle is nonzero
([crashreport.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/debug/crashreport.c#L382)),
and the main thread's vCPU handle is 0, which `docs/internals.md` notes is a
valid handle.

`src/debug/crashreport.h` lists seven kinds of crash
([crashreport.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/debug/crashreport.h#L20-L28)):

```c
typedef enum {
    CRASH_TIMEOUT,         /* vCPU execution timed out (alarm) */
    CRASH_BAD_EXCEPTION,   /* HVC #2: guest exception at EL1 */
    CRASH_UNEXPECTED_HVC,  /* Unknown HVC immediate value */
    CRASH_UNEXPECTED_EC,   /* Unhandled exception class at EL2 */
    CRASH_UNEXPECTED_EXIT, /* hv_vcpu_run returned unknown reason */
    CRASH_ELR_ZERO,        /* ELR_EL1=0 after exec (register sync bug) */
    CRASH_HV_CHECK,        /* Hypervisor.framework API call failed */
} crash_type_t;
```

`CRASH_BAD_EXCEPTION` is the `hvc #2` from part 5: an exception the shim has no
handler for, such as one that lands in the 14 handler slots that should never
fire, or an `svc` with a nonzero immediate. Many Hypervisor.framework calls are
wrapped in one of two macros in `src/hvutil.h`. `HV_CHECK` prints the failed
call and exits with 1. `HV_CHECK_CTX`, used around `hv_vcpu_run()`, also prints
a crash report
([hvutil.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/hvutil.h#L17-L40)).

## Attaching GDB

For the program itself, elfuse contains a stub for GDB's remote serial
protocol: the packet protocol GDB uses to control a program through a separate
stub, over a serial line, a pipe, or a network connection. `--gdb PORT` starts
the stub on that port, listening on the local machine only, and
`--gdb-stop-on-entry` holds the program before its first instruction until a
debugger connects and resumes it
([Debugging With GDB Or LLDB](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/usage.md#debugging-with-gdb-or-lldb)).

With the program from part 1, in one terminal:

```sh
$ build/elfuse --gdb 12345 --gdb-stop-on-entry build/test-hello
20:10:36 INFO  src/core/guest.c:551: guest: primary slab 1024 GiB (40-bit) mapped
20:10:36 INFO  src/debug/gdbstub.c:1301: GDB stub listening on localhost:12345
20:10:36 INFO  src/debug/gdbstub.c:1305: Connect with: aarch64-linux-gnu-gdb -ex "target remote :12345" <binary>
20:10:36 INFO  src/debug/gdbstub.c:1325: Waiting for GDB to attach...
20:10:37 INFO  src/debug/gdbstub.c:1211: GDB client connected from 127.0.0.1:50205
20:10:37 INFO  src/debug/gdbstub.c:1366: GDB attached, starting guest
hello
```

In another, a GDB for aarch64 Linux programs:

```sh
$ aarch64-linux-gnu-gdb -nx -batch \
    -ex 'target remote :12345' -ex 'info registers pc' -ex 'x/8i $pc' \
    -ex 'break *0x400010' -ex 'continue' -ex 'info registers x0 x1 x2 x8' \
    -ex 'x/s $x1' -ex 'stepi' -ex 'info registers pc x0' -ex 'continue' \
    build/test-hello
0x0000000000400000 in _start ()
pc             0x400000            0x400000 <_start>
=> 0x400000 <_start>:	mov	x0, #0x1                   	// #1
   0x400004 <_start+4>:	adr	x1, 0x400020 <msg>
   0x400008 <_start+8>:	mov	x2, #0x6                   	// #6
   0x40000c <_start+12>:	mov	x8, #0x40                  	// #64
   0x400010 <_start+16>:	svc	#0x0
   0x400014 <_start+20>:	mov	x0, #0x0                   	// #0
   0x400018 <_start+24>:	mov	x8, #0x5d                  	// #93
   0x40001c <_start+28>:	svc	#0x0
Breakpoint 1 at 0x400010

Breakpoint 1, 0x0000000000400010 in _start ()
x0             0x1                 1
x1             0x400020            4194336
x2             0x6                 6
x8             0x40                64
0x400020 <msg>:	"hello\n"
0x0000000000400014 in _start ()
pc             0x400014            0x400014 <_start+20>
x0             0x6                 6
Remote connection closed
```

GDB stops at `_start`, runs to the `svc`, and shows the arguments of `write` in
`x0`, `x1`, and `x2` and its number in `x8`, with the string at `x1`. A single
step over the `svc` runs the whole system call, which is when `hello` appears in
the first terminal, and leaves the result, 6, in `x0`. LLDB can attach the same
way, with `gdb-remote 12345`.

## How the stub stops a program

A breakpoint is a point where the program stops. Debuggers on Linux usually set
one by writing a `BRK` instruction into the program's code, which traps when
executed. elfuse's stub does not change the program's code. It uses hardware
breakpoints: the processor's debug registers hold addresses, and the processor
raises an exception when the program counter reaches one. Watchpoints, which
stop the program when it reads or writes an address, use a second set of debug
registers
([gdbstub.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/debug/gdbstub.h#L8-L21)).
The stub's tables hold 16 of each, but only as many as the processor implements
take effect: 6 breakpoints and 4 watchpoints on this M4. Even GDB's ordinary
`break`, which asks for a software breakpoint, gets a hardware one
([gdbstub.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/debug/gdbstub.c#L690-L693)):
"treat as hardware breakpoint since hardware support is available and this
avoids I-cache issues".

Single-stepping is a temporary hardware breakpoint on the next instruction
([gdbstub.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/debug/gdbstub.c#L857-L860)):
"placing a temporary hardware breakpoint at PC+4. This covers straight-line
instructions; branch-accurate stepping would require an instruction decoder."
Stepping over a branch that is taken stops wherever the next breakpoint is, not
at the branch target.

These debug exceptions do not pass through the shim. elfuse asks
Hypervisor.framework to send them straight to the host, so they go from EL0 to
EL2 and skip EL1. The run loop copies the stop address into `ELR_EL1` so that
the register snapshot, described next, shows where the program stopped
([proc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/proc.c#L4227-L4231)).

## Taking a snapshot

GDB's requests arrive on the stub's own thread, but only a vCPU's thread may
access its registers. `docs/internals.md` describes how elfuse splits the work
([GDB Stub](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#gdb-stub)):

> Because Hypervisor.framework register access must happen on the owning
> thread, the stopped vCPU snapshots its own state; the GDB-handler thread
> reads and updates the snapshot, and the owning thread restores the modified
> state on resume ...

The same pattern serves a debugger running inside the guest. A Linux program
can trace a thread or a `clone(CLONE_VM)` child of its own process with
`ptrace`, and elfuse implements the basic requests
([Snapshot Protocol](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#snapshot-protocol)).
The traced thread copies its registers into a snapshot before it stops, the
tracer reads and writes the snapshot, and the traced thread applies the changes
when it resumes. A `BRK` instruction in a guest program reaches elfuse through
the shim as `hvc #10` and becomes a stop for its tracer, or a `SIGTRAP` if it
has none
([proc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/proc.c#L3512-L3513)).

The stub does not support x86_64 programs, which run through Rosetta (part 13).

## Diagnostic switches

Earlier parts used most of elfuse's diagnostic switches
([Diagnostic Environment Variables](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/usage.md#diagnostic-environment-variables)):

| Switch | What it shows | Used in |
|---|---|---|
| `-v` | every exit, every system call and its result, the boot page tables | parts 1 to 4, 6, 7, 9, 11, 13 |
| `ELFUSE_STARTUP_TRACE=steps` | the time of each load and boot step | part 3 |
| `ELFUSE_STARTUP_TRACE=syscalls` | a count of system calls, frozen at the first `execve` | parts 1, 10 |
| `ELFUSE_SHIM_STATS=1` | the shim's fast-path counters at exit | parts 7, 10 |
| `ELFUSE_DISABLE_TLBI_RANGE=1` | turns off the ranged TLB flush from part 6 | |

The syscall count stops at the first `execve`, so steady-state calls after an
exec do not swamp it. A program that never calls `execve`, like those in parts
1 and 10, is counted until it exits.
The last switch is for a narrower question: whether a bug caused by a stale
translation comes from the ranged flush or from somewhere else.

## What we learned

- A crash in the Linux program is a Linux signal. An exception the shim cannot
  handle or the watchdog firing makes elfuse stop with a crash report written as
  an issue template.
- `--gdb` starts a GDB remote protocol stub, and GDB or LLDB can stop the
  program, read registers and memory, set breakpoints, and step.
- All breakpoints are hardware breakpoints, so elfuse never writes into the
  program's code. Single-step stops at the next address in memory, not at a
  branch target.
- Debugger threads never access a vCPU's registers directly. The stopped vCPU's
  own thread takes a snapshot, the debugger edits it, and the owner writes it
  back.

[Part 13](../13-x86-64-via-rosetta/) is about x86_64 programs: how elfuse runs
code for a different processor on an Apple Silicon Mac, using Apple's own
translator.
