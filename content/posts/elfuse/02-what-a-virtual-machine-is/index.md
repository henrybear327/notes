---
title: 'elfuse 2: What a virtual machine is'
date: 2026-09-09T11:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "hypervisor"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 2 of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 1](../01-what-happens-when-you-run-a-program/) showed that a Linux
program asks the kernel for everything through the `svc` instruction, and
that elfuse answers those requests itself. This part explains how a macOS
process receives those requests.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac with 32 GiB of memory.

## The problem

In part 1, `svc` switched the processor into kernel mode and jumped to an
address the kernel had chosen. On a Mac that kernel is macOS. The elfuse
process runs in user mode like any other program, so if the Linux program's
`svc` ran directly on the Mac, it would land in the macOS kernel and elfuse
would never see it.

elfuse cannot change the macOS kernel. It needs the processor to deliver the
Linux program's `svc` to code that elfuse controls. The processor feature that
makes this possible is virtualization.

## More than two modes

Part 1 described two modes. The Arm architecture defines four privilege levels,
called exception levels and numbered EL0 to EL3; a higher number is allowed to
do more. Three of them matter here:

- EL0 runs applications. It is user mode from part 1.
- EL1 runs an operating system kernel. It is kernel mode from part 1.
- EL2 runs a hypervisor: a program that runs whole operating systems as its
  guests.

A guest is the software running under a hypervisor: a kernel at EL1 and its
programs at EL0. The host is the real machine and the operating system that
owns it. On a Mac, EL2 belongs to macOS.

## Exceptions

The processor moves up a level only through an exception: an event that stops
the running code and transfers control to a handler at the same or a higher
level. An exception that code causes on purpose is also called a trap. `svc` is
a trap from EL0 to EL1. `hvc` ("hypervisor call") is the same idea one level
up, a trap from EL1 to EL2. Other exceptions are faults, such as reading an
address that has no memory behind it, or interrupts from devices such as
timers.

The code that causes an exception does not choose the handler. Each level above
EL0 has a table of handlers, blocks of code at fixed offsets set up in advance
by the software at that level. When the handler is done, it executes `eret`
("exception return"), which returns to the level the exception came from and
resumes the interrupted code.

The settings that control the processor itself live in system registers.
`VBAR_EL1` holds the address of EL1's handler table, `SCTLR_EL1` switches
features such as address translation, the mapping from the addresses a program
uses to memory locations, on and off, and `ELR_EL1` holds the address that
`eret` returns to. Code at EL0 cannot change them.

## Hypervisor.framework

macOS lets ordinary processes use its EL2 through Hypervisor.framework, a system
library. A process calls it to create a virtual machine (VM), give the VM
memory, and create virtual processors for it. The process must be signed with an
entitlement, a permission that macOS checks when the process asks for a VM:

```sh
$ codesign -d --entitlements - build/elfuse
Executable=.../build/elfuse
[Dict]
	[Key] com.apple.security.hypervisor
	[Value]
		[Bool] true
```

A virtual processor, or vCPU, is a full set of processor state (registers and
system registers) that the process runs on one of its own threads, the separate
lines of execution inside a process. The thread calls `hv_vcpu_run()`. The real
processor then executes guest instructions, at the guest's EL1 and EL0, until
the guest does something that needs the hypervisor, such as `hvc`. The
processor traps to EL2, macOS stops the guest, and `hv_vcpu_run()` returns to
the thread. That return is called an exit. The thread reads the exit
information `hv_vcpu_run()` filled in and the vCPU's registers, deals with the
exit, and calls `hv_vcpu_run()` again.

This is how elfuse receives the program's requests. elfuse puts the Linux
program at the guest's EL0. The program's `svc` traps to the guest's EL1, not
to macOS. At EL1 elfuse places a small handler called the shim, which forwards
the requests it cannot answer itself with `hvc`. The `hvc` becomes an exit, and
the exit arrives in elfuse's own thread.

![The elfuse process on the left runs a vCPU thread that calls hv_vcpu_run. The virtual machine on the right holds the Linux program at EL0 and the shim at EL1. svc goes from EL0 to EL1, hvc #5 goes from EL1 to EL2 in the macOS kernel, which returns to the vCPU thread as an exit.](figure-2.svg)

## Watching the exits

The `-v` flag logs every exit. These are the lines of `build/elfuse -v
build/test-hello` that show the vCPU loop, for the program from part 1, with
timestamps removed:

```text
DEBUG src/syscall/proc.c:4567: elfuse: [0] vcpu_run PC=0xfeffdf6000
DEBUG src/syscall/proc.c:4029: elfuse: HVC #4
DEBUG src/syscall/proc.c:4134: elfuse: HVC #4 set reg 4 = 0x34d0d985
DEBUG src/syscall/proc.c:4567: elfuse: [1] vcpu_run PC=0xfeffdf6020
DEBUG src/syscall/proc.c:4029: elfuse: HVC #5
DEBUG src/syscall/syscall.c:2807: syscall 64@0x400010(0x1, 0x400020, 0x6, 0x0, 0x0, 0x0)
hello
DEBUG src/syscall/syscall.c:2853:   -> 6 (0x6)
DEBUG src/syscall/proc.c:4567: elfuse: [2] vcpu_run PC=0xfeffdf7bf0
DEBUG src/syscall/proc.c:4029: elfuse: HVC #5
DEBUG src/syscall/syscall.c:2807: syscall 93@0x40001c(0x0, 0x400020, 0x6, 0x0, 0x0, 0x0)
```

The number in brackets numbers the calls to `hv_vcpu_run()` from 0, and `PC`
is the address the vCPU is about to execute. The whole program needed three
calls:

1. The vCPU starts in the shim at `0xfeffdf6000`, not in the program. The
   shim flushes stale translations, then issues `hvc #4`, which asks elfuse to
   write register 4 of its own list, `SCTLR_EL1`. The value's lowest bit
   switches address translation on. Part 5 explains why the shim has to ask
   instead of elfuse setting it up front.
2. The vCPU resumes at `0xfeffdf6020`, the instruction after that `hvc`. The
   shim executes `eret` into the program at EL0, the program runs until its
   first `svc`, the shim turns that into `hvc #5`, and elfuse handles
   `write`.
3. The vCPU resumes inside the shim, which returns to the program. The
   program's second `svc` becomes another `hvc #5`, this time for `exit`, and
   elfuse stops.

Between exits, every instruction ran directly on the processor. elfuse only
sees what the shim forwards.

## Guest memory is host memory

A VM has no memory of its own; the process supplies memory from its own address
space. elfuse reserves one large region with `mmap`, the call a process uses to
ask its kernel for memory, and maps it into the VM with `hv_vm_map`
([src/core/guest.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.c#L513-L545)):

```c
g->host_base = mmap(NULL, try_size, PROT_READ | PROT_WRITE,
                    MAP_ANON | MAP_PRIVATE, -1, 0);
...
ret = hv_vm_map(g->host_base, GUEST_IPA_BASE, try_size,
                HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
```

Between the two calls, elfuse remaps the region onto a temporary file, which
part 11 uses for `fork`. This series calls that region the slab. `-v` reports
its size:

```text
INFO  src/core/guest.c:551: guest: primary slab 1024 GiB (40-bit) mapped
```

That is 1024 GiB on a Mac with 32 GiB of memory. The size depends on what
Hypervisor.framework accepts on the Mac, up to 1 TiB: `docs/internals.md` gives
64 GiB for an M2. The reservation is address space, not memory. Memory is handed
out in fixed-size blocks called pages, and macOS gives a page of the slab real
memory only when something first uses it, so untouched parts of the slab use no
physical memory.

`GUEST_IPA_BASE` is 0, so the start of the slab is the guest's address 0, and
guest address `0x400000`, where the program's code sits, is host address
`host_base + 0x400000` in the elfuse process. When elfuse handles `write`, it
checks that the program may read guest address `0x400020` and then hands macOS
the pointer `host_base + 0x400020` to write the six bytes of `hello\n`.

## Page tables

Programs do not use physical addresses, the actual positions in memory. Every
address a program uses is a virtual address, which the processor translates
to a physical address on each access. The part of the processor that does this
is the MMU (memory management unit). It follows a page table: a tree of tables
in memory, set up by the kernel, that maps ranges of virtual addresses to
physical ones and records what each range allows. A range can be readable and
writable (RW) or readable and executable (RX), and it can be closed to EL0
entirely.

A VM adds a second translation. The guest's page tables turn a virtual address
into what the guest uses as a physical address, called an IPA (intermediate
physical address). The hypervisor then turns the IPA into real memory;
`hv_vm_map` is how elfuse sets up that second step. elfuse writes the guest's
page tables itself, and for a native arm64 program it makes every virtual
address equal to its IPA, which is also its offset in the slab.

`-v` prints some of the page-table entries elfuse writes before the vCPU starts.
One of them:

```text
DEBUG src/core/bootstrap.c:199: L2[8]=0x60000001000765
```

`L2` is the third level of the tree, where each entry covers 2 MiB. Entry 8
covers the range starting at 8 × 2 MiB, address `0x01000000`, which is where
the program's growable memory region starts (part 6). The value decodes with
the constants in
[src/core/guest.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.c#L141-L152):

| Part of the value | Constant | Meaning |
|---|---|---|
| `0x01000000` | | the range maps to IPA `0x01000000` |
| `0x001` | `PT_BLOCK` | valid entry that maps memory directly |
| `0x004` | `PT_ATTR1` | ordinary memory |
| `0x040` | `PT_AP_RW_EL0` | readable and writable at EL0 and EL1 |
| `0x020`, `0x300`, `0x400` | `PT_NS`, `PT_SH_ISH`, `PT_AF` | bookkeeping flags |
| `0x60000000000000` | `PT_UXN`, `PT_PXN` | never executable at EL0 or EL1 |

Walking the tree on every memory access would be slow, so the processor keeps
a small cache of recent translations, the TLB (translation lookaside buffer).
When elfuse changes a page table while the guest runs, the TLB may still hold
the old translation, and it has to be flushed. Part 6 shows how.

## An empty machine

The VM that elfuse builds has no kernel, no disk, and no devices. Its EL1 holds
the shim, 7508 bytes of code:

```text
DEBUG src/core/bootstrap.c:495: shim loaded at offset 0xfeffdf6000 (7508 bytes)
```

There is no kernel to boot. Over 30 runs of `build/elfuse
build/test-hello` on this Mac, including starting the elfuse process itself,
the median was 11.9 ms, the fastest 10.9 ms, and the slowest 20.4 ms.

## Hypervisor.framework constraints

Three of the constraints `docs/internals.md` lists under
[Hypervisor.framework Constraints](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#hypervisorframework-constraints)
come up in later parts:

- A process can own one VM. Linux programs copy themselves with `fork`, and a
  copy needs its own guest, so elfuse starts a second elfuse process for it
  (part 11).
- A vCPU's registers can be read and changed only from the thread
  that runs it. That shapes how elfuse delivers signals, the notifications a
  process receives, and serves debuggers (parts 9 and 12).
- Address translation must be switched on from inside the running guest,
  which is why the vCPU starts in the shim and exits with `hvc #4` (part 5).

## What we learned

- Arm has privilege levels EL0 for programs, EL1 for kernels, and EL2 for
  hypervisors. Code moves up a level only through an exception, to a handler
  set up in advance, and returns with `eret`.
- Hypervisor.framework lets a macOS process own a VM. A vCPU runs guest code on
  one of the process's threads until an exit hands control back to it.
- elfuse puts the Linux program at EL0 and its shim at EL1, so the program's
  `svc` reaches the shim and the shim's `hvc` reaches elfuse.
- Guest memory is one slab of the elfuse process's own memory; for a native
  arm64 program, guest address X is slab byte X.
- Page tables translate addresses and set permissions, and the TLB caches the
  translations.

[Part 3](../03-from-launch-to-exit/) follows the steps elfuse takes between
`build/elfuse build/test-hello` and the program's exit, and names the source
file for each.
