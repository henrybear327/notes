---
title: 'elfuse 5: Booting a VM with no kernel'
date: 2026-09-09T14:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "hypervisor", "assembly"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 5 of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 4](../04-loading-a-linux-program/) put a Linux program and its stack into
guest memory. This part is the boot step: the shim, the code elfuse places at
the guest's EL1, and how the vCPU gets from its first instruction to the
program's.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac.

## The problem

After loading, the program is in memory, but the processor is not ready to run
it. Address translation is off. EL1 has no handler table and no stack. And
once the program runs, every exception it causes lands at EL1, where on Linux
the kernel would be. The shim has to save the program's registers, pass the
call to elfuse, and restore the registers exactly as they were.

The shim is `src/core/shim.S`: 1854 lines of arm64 assembly that build to 7508
bytes. It is assembly because the first instructions of an exception handler run
before anything has been saved. They must not change any register, and compiled
C code does not guarantee that.

## From source file to guest memory

The shim is not a separate file at run time. `mk/shim.mk` assembles it, strips
the result down to raw instruction bytes, and turns those bytes into a C array
that is compiled into elfuse:

```make
# shim.S -> shim.o -> shim.bin -> shim_blob.h (C byte array)
```

At boot, elfuse copies that array into its slot near the top of the slab
([bootstrap.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/bootstrap.c#L493)).
The objdump tool can disassemble the assembled shim. The objects are Mach-O
files, so LLVM's `llvm-objdump` reads them where the GNU aarch64 one does not.

## The registers elfuse sets first

Before the first `hv_vcpu_run()`, elfuse writes the vCPU's registers in
`guest_bootstrap_create_vcpu()`
([bootstrap.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/bootstrap.c#L707-L779)).
These are the ones that matter for booting:

| Register | Value | Why |
|---|---|---|
| `VBAR_EL1` | shim + `0x800` | EL1's handler table |
| `TTBR0_EL1` | the top page table | where the MMU starts each translation |
| `ELR_EL1` | the program's entry point | where `eret` will go |
| `SPSR_EL1` | `0` | the processor state `eret` restores; 0 means EL0 |
| `SP_EL0` | the program's stack | from part 4 |
| `SP_EL1` | top of the shim's data block | the shim's own stack |
| `PC` | the shim's first byte | where the vCPU starts |
| `X0` | `SCTLR_EL1` with bit 0 set | the value that turns the MMU on |

The end of the register setup is the unusual part:

```c
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, shim_ipa));
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5));
    vcpu_zero_gprs(vcpu);

    sctlr_with_mmu = SCTLR_RES1 | SCTLR_M | SCTLR_C | SCTLR_I | SCTLR_DZE |
                     SCTLR_UCT | SCTLR_UCI;
    HV_CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0, sctlr_with_mmu));
```

elfuse writes `SCTLR_EL1` without `SCTLR_M`, the MMU bit, and hands the value
with the bit set to the shim in `x0`. `CPSR` holds the processor's current
state, and `0x3c5` selects EL1 with interrupts held back. The vCPU starts in
the shim, at EL1, with address translation off.

## Turning the MMU on from inside

The shim's first comment says why elfuse does not set the bit directly
([shim.S](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/shim.S#L298-L306)):

```asm
_start:
    /* Host has configured all system registers EXCEPT SCTLR.M (MMU enable).
     * Apple's Hypervisor.framework requires the MMU to be enabled DURING vCPU
     * execution (via HVC #4), not before hv_vcpu_run(). Setting SCTLR.M=1 via
     * hv_vcpu_set_sys_reg before start causes permission faults on the first
     * instruction fetch.
     *
     * Host passes the final SCTLR value (with M=1) in X0 before start.
     */
```

The shim does not write `SCTLR_EL1` with `msr` either. It asks elfuse to write
it, from inside the running vCPU. These are the first instructions the vCPU
runs:

```text
$ llvm-objdump -d build/shim.o
...
       0: aa0003e9     	mov	x9, x0
       4: d508831f     	tlbi	vmalle1is
       8: d508711f     	ic	ialluis
       c: d5033b9f     	dsb	ish
      10: d5033fdf     	isb
      14: d2800080     	mov	x0, #0x4                ; =4
      18: aa0903e1     	mov	x1, x9
      1c: d4000082     	hvc	#0x4
      20: d5033fdf     	isb
      24: d2800000     	mov	x0, #0x0                ; =0
      28: d2800001     	mov	x1, #0x0                ; =0
...
      9c: d280001e     	mov	x30, #0x0               ; =0
      a0: d69f03e0     	eret
```

1. Keep the `SCTLR_EL1` value from `x0`.
2. Flush any cached address translations (the TLB from part 2) and cached
   instructions, so nothing stale is used once translation starts. Part 6 covers
   both caches.
3. `hvc #4` with `x0 = 4` (the register, `SCTLR_EL1`) and `x1` = the value. This
   is the exit numbered `[0]` in part 2's trace. elfuse writes the register and
   resumes the vCPU at `0x20`, the address in part 2's `[1] vcpu_run
   PC=0xfeffdf6020`.
4. Zero all 31 general registers, so the program starts with no leftover
   values.
5. `eret`. `ELR_EL1` holds the program's entry point and `SPSR_EL1` says EL0, so
   the processor drops to EL0 at `0x400000`.

The program is now running, and the shim only runs again when the program
causes an exception.

## The handler table

`VBAR_EL1` points at the shim's exception vector table: the table of handlers
from part 2. It has 16 slots of `0x80` bytes each, starting `0x800` bytes into
the shim. The slots come in four groups: exceptions taken from EL1 itself (two
groups, by which stack pointer was in use), from 64-bit code at EL0, and from
32-bit code at EL0. Each group has four kinds. The one that matters is
"synchronous": an exception caused by the instruction being executed, such as
`svc` or an access to unmapped memory. The other three kinds are interrupts,
which hardware such as a timer raises whatever instruction is running, and
hardware errors, which a VM with no devices should never see.

Only two slots do anything
([shim.S](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/shim.S#L352-L386)):
synchronous exceptions from EL1 at offset `0x200`, and synchronous exceptions
from EL0 at offset `0x400`. The other 14 report the exception to elfuse with
`hvc #2` and stop. In the built shim:

```text
$ llvm-objdump -d --start-address=0x800 --stop-address=0x808 build/shim.o
...
     800: d2800005     	mov	x5, #0x0                ; =0
     804: 140001e1     	b	0xf88 <bad_exception>
$ llvm-objdump -d --start-address=0xc00 --stop-address=0xc04 build/shim.o
...
     c00: 140000e7     	b	0xf9c <svc_handler>
$ llvm-objdump -d --start-address=0xc80 --stop-address=0xc88 build/shim.o
...
     c80: d2809005     	mov	x5, #0x480              ; =1152
     c84: 140000c1     	b	0xf88 <bad_exception>
```

Slot `0x400`, at `0x800 + 0x400 = 0xc00`, is a single branch. Its neighbors
first put their own offset in `x5`, so elfuse can report which slot fired.
Slot `0x400` must not do that.

## The vector-entry rule

Programs rely on some registers keeping their values across calls. In an
ordinary function call on arm64, `x19` to `x28` are callee-saved: a function
that uses them must restore them before returning. `x9` to `x15` are
caller-saved: a function may overwrite them, so a caller that needs them keeps
its own copy.

A system call is stricter. On Linux, `svc` preserves every general-purpose
register except `x0`, which carries the result. Compilers and C libraries depend
on that and do not save `x9` to `x15` around a system call. The shim's header
states the consequence
([shim.S](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/shim.S#L63-L65)):
"the compiler does not save scratch X9-X15 across an SVC, so the handler must
save and restore all 31 GPRs." (GPR means general-purpose register, `x0` to
`x30`.)

So nothing may change a register before the shim saves them.
`docs/internals.md` calls this the
[critical vector-entry rule](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#critical-vector-entry-rule),
and gives `mov x5, #offset` in slot `0x400` as the example of what would
break it: every system call would come back with `x5` changed. The other slots
may write `x5` because they stop the guest and never return.

## Save, forward, restore

`svc_handler`
([shim.S](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/shim.S#L412-L459))
runs in this order.

1. `SAVE_GPRS` pushes all 31 registers onto the shim's stack, in a 256-byte
   frame.
2. It reads `ESR_EL1`, the system register that records why the exception
   happened. Bits 31 to 26 hold the exception class, and `0x15` means `svc`
   from 64-bit code. Other classes, such as memory faults, go elsewhere.
3. A short chain of comparisons on the syscall number picks out the few calls
   the shim can answer by itself, such as `getpid`. Part 7 covers those.
4. Everything else goes to `handle_svc_0`.

`handle_svc_0` copies the program's values back into the registers from the
frame, leaving the frame on the stack, and executes `hvc #5`
([shim.S](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/shim.S#L1699-L1732)).
elfuse then reads the syscall number from `x8` and the arguments from `x0` to
`x5` directly, handles the call, writes the result into `x0`, and writes a
request into `x8`. When the vCPU resumes, the shim looks at `x8`:

| `x8` | What the shim does next |
|---|---|
| 0 | check `x7` for a `ptrace` stop (part 12), then restore `x1` to `x30` from the frame and `eret` |
| 1, 3, 4 | first flush cached address translations (part 6) |
| 2 | flush cached translations and instructions, throw the frame away, and `eret`, because elfuse replaced the registers (part 9) |

Overwriting the program's `x8` here breaks nothing, because the restore puts
the saved `x8` back. elfuse writes `x8` on every return, even when there is
nothing to flush
([syscall.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/syscall.c#L2904-L2906)):
"the pre-syscall X8 is the syscall number (always non-zero) and would
spuriously TLBI on every return."

The common case is two tests, `cbz x8` and `cbz x7`, followed by the restore
and `eret`. The program continues at the instruction after its `svc`, with the
result in `x0` and every other register unchanged.

![One write call as a timeline in three lanes. The program's svc #0 enters the shim at vector offset 0x400, which branches to svc_handler. SAVE_GPRS builds a 256-byte frame; a fast path can eret from here with no exit. Otherwise LOAD_GPRS and hvc #5 exit to elfuse, which handles the write, sets x0 to 6 and x8 to 0, and calls hv_vcpu_run. The shim tests x8 and x7, restores x1 to x30, and erets to the next instruction with x0 = 6.](figure-4.svg)

## Numeric labels

In assembly, a label like `1:` can be defined many times, and `b 1f` means
"branch to the next `1:` after this point". Moving code can change which `1:`
is next, and the assembler does not report that the branch now goes somewhere
else. The comment on the shim's return path records one case
([shim.S](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/shim.S#L1810-L1814)):

> Named target for every jump that lands here, and deliberately without a
> numeric label. A bare 'b 1f' from the identity fast path in svc_handler would
> resolve to the next forward '1:', which sits inside handle_inst_abort, and
> silently re-route the identity result into a W^X toggle. Caught the hard way
> during the prior P2 attempt; ...

In that earlier attempt, a fast-path result such as `getpid`'s went into the
handler that changes page permissions instead of returning to the program. A
numeric label caused the same kind of misroute in the code that flushes
translations, where the branch skipped the `x7` check.
`scripts/check-svc-tails.py`, part of `make check`, reads the shim's return
paths and fails when a numeric label there resolves outside them or a path
other than one listed exception skips that check
([check-svc-tails.py](https://github.com/sysprog21/elfuse/blob/73d8246b/scripts/check-svc-tails.py#L10-L19)).

## The shim's data block

The shim keeps data in a 2 MiB block next to its code: cached values such as
the process id that its fast paths return, a store of random bytes, and a flag
elfuse sets to turn the fast paths off. The top of the same block is the shim's
stack; part 3's trace showed `SP_EL1=0xff00000000`.

If the program could write to that block, it could set the results of its own
system calls. So the block's page-table entries allow EL1 and deny EL0
([bootstrap.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/bootstrap.c#L308-L317)):
"the guest must not directly read or write the identity cache, attention flag,
urandom bitmap, or ring, any of which would let it spoof its own syscall
results." The program could still try to make elfuse write there for it, by
passing an address in the block as a buffer to `read`. elfuse's check of guest
addresses refuses pages marked EL1-only for that reason
([guest.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.c#L1441-L1449)).

## What we learned

- elfuse starts the vCPU in the shim at EL1 with the MMU off. The shim flushes
  caches, asks elfuse to switch the MMU on with `hvc #4`, zeroes the
  registers, and `eret`s to the program at EL0.
- Of the 16 slots in the shim's handler table, only synchronous exceptions from
  EL0 and EL1 are handled. The EL0 slot is a single branch, because nothing may
  change a register before the shim saves all 31.
- A system call is saved in a 256-byte frame, forwarded with `hvc #5`, and
  restored. elfuse returns the result in `x0` and a request in `x8`.
- The shim's data block is readable only at EL1, so the program cannot change
  the results the shim returns.

[Part 6](../06-guest-memory-and-page-tables/) is about the page tables the
boot step writes: how one flat slab becomes a Linux address space, and how
elfuse provides 4 KiB pages on a Mac that uses 16 KiB ones.
