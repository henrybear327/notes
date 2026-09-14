---
title: 'elfuse 14: Testing and proofs'
date: 2026-09-09T23:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "testing", "frama-c"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 14, the last of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 13](../13-x86-64-via-rosetta/) was the last part on how elfuse runs Linux
programs. This part is about how elfuse is checked against Linux behavior, and
ends with a recap of the series and a glossary.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac.

## The problem

elfuse reimplements Linux system calls. A test written by the author of a
handler checks that author's understanding of Linux, which can be wrong. Some
code also turns addresses and lengths from the guest into memory accesses in
the elfuse process, and a malicious program can pass values that no test uses.

elfuse handles the first problem by running its tests on a real Linux kernel
as well, and the second by proving the arithmetic.

## Comparing with a Linux kernel

`tests/test-matrix.sh` runs the same test programs in more than one way
([test-matrix.sh](https://github.com/sysprog21/elfuse/blob/73d8246b/tests/test-matrix.sh#L8-L13)):

```bash
# Modes:
#   elfuse-aarch64 : run binaries on macOS via build/elfuse
#   qemu-aarch64   : run binaries natively inside qemu-system-aarch64 (boots an
#                    Alpine minirootfs initramfs that the fixture script
#                    downloads on demand)
#   all            : run both modes back-to-back
```

QEMU runs a complete arm64 virtual machine (on a Mac through
Hypervisor.framework, elsewhere in software), and here it boots a real Linux
kernel with Alpine Linux. A test program such as `test-file-ops` runs once
under elfuse and once under that kernel, and both runs have to pass.
`docs/testing.md` states the purpose ([Test
Matrix](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/testing.md#test-matrix)):

> The goal is not to compare performance. The goal is to compare
> guest-observable behavior against a ground-truth Linux AArch64 environment so
> that any divergence in syscall translation, procfs emulation, or process
> semantics is caught early.

A third mode, `elfuse-x86_64`, runs Rosetta acceptance scripts. It has no
reference kernel, so it checks that x86_64 programs work rather than that they
match Linux exactly. Each mode has a minimum number of passing tests: 250 for
elfuse, 225 for QEMU, 71 for x86_64
([test-matrix.sh](https://github.com/sysprog21/elfuse/blob/73d8246b/tests/test-matrix.sh#L1480-L1486)).
The QEMU minimum could be one higher, and the comment explains why it is not:
"A floor too low costs nothing; an unobserved one asserts a run that did not
happen."

A test may skip QEMU only when a real boot showed it cannot pass there
([test-matrix.sh](https://github.com/sysprog21/elfuse/blob/73d8246b/tests/test-matrix.sh#L280-L283)):
"Do not add a test here just because it *might* behave differently; confirm it
first the same way."

## Expected failures

Sometimes elfuse knowingly differs from Linux. Deleting the check would hide
the difference, and a failing check would keep the suite red. elfuse records
such cases as XFAIL rows, "expected failure", which print both Linux's value
and elfuse's. For example:

```sh
$ make test-dir-union-alias
...
  plain fork cross-close         XFAIL: Linux 403, elfuse 354
...
  union fork cross-close         XFAIL: Linux 405, elfuse 0
  plain two aliases inside the child OK

test-dir-union-alias: 23 passed, 0 failed - PASS
2 expected failures printed above, neither passed nor failed
```

`docs/testing.md` gives the reasoning: deleting the row instead "is what lets a
known divergence become an unknown one"
([testing.md](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/testing.md?plain=1#L520-L525)).
Linux's value is recorded in the test; if elfuse's value moves, the row says so.

## When a test hangs

The test runners wrap nearly every test with a sampler
([hang-sample.sh](https://github.com/sysprog21/elfuse/blob/73d8246b/tests/lib/hang-sample.sh#L7-L11)):

```bash
# A hang that only reproduces under suite load is otherwise reported as a bare
# "timeout after Ns" with nothing to diagnose. Both test entry points
# (tests/driver.sh and tests/lib/test-runner.sh) arm this around every
# invocation: it samples the live process shortly before timeout(1) kills it,
# and the caller keeps the output only when the watchdog actually fired.
```

The "watchdog" here is the test's time limit, enforced by the `timeout`
command, not elfuse's own `--timeout`. About eight seconds before the time
limit, the sampler records each thread's state with `ps -M` and a one-second
call-tree profile of every thread with the macOS `sample` tool.

## Sanitizers

A sanitizer is a compiler option that adds checks to a program while it runs.
AddressSanitizer (ASAN) catches invalid memory access,
UndefinedBehaviorSanitizer (UBSAN) catches operations C leaves undefined, such
as signed overflow, and ThreadSanitizer (TSAN) catches data races between
threads. `make check-asan`, `check-ubsan`, and `check-tsan` rebuild elfuse with
one of them and run a chosen part of the tests
([tests.mk](https://github.com/sysprog21/elfuse/blob/73d8246b/mk/tests.mk#L139-L144)):
"a representative subset of manifest sections that stress concurrency, memory,
fork, and signal machinery (where ASAN/UBSAN/TSAN actually find bugs)".

## Proving the arithmetic

Tests show that code works for the inputs someone tried. For code that turns
guest numbers into host memory accesses, elfuse also proves that no input can
make the arithmetic go wrong. `src/proved/gva.h` gives the reason
([gva.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/proved/gva.h#L8-L10)):

> A guest controls the address, the length, and (through mmap/mprotect) much of
> the page-table content these read, so an arithmetic slip here is a host
> out-of-bounds access rather than a guest fault.

The tool is Frama-C, a platform for analyzing C programs. Its WP plugin (weakest
precondition) reads a function together with its contract, a specification
written in ACSL (ANSI/ISO C Specification Language) inside special comments.
From the two it derives proof obligations: logical formulas that are true only
if the code meets the contract. It hands them to automatic provers, programs
that search for a proof of a formula; elfuse uses Alt-Ergo and Z3. With the
`-wp-rte` option, it adds obligations that rule out runtime errors such as
signed overflow, division by zero, an out-of-range shift, or an invalid or
out-of-bounds dereference. Unsigned wraparound is defined in C, so it is covered
only where a contract states it, as `rejects_only_on_wrap` below does.

Frama-C's C library does not model some macOS headers, such as `sys/mount.h`
and `sys/event.h`, so C files that include them cannot be analyzed. Most of the
arithmetic therefore lives in small headers under `src/proved/`, most of which
need only `stdint.h`, and the C files include them. Part 6's search for a free
address range uses `align_up_ok()` from `src/proved/align.h`, which rounds a
number up to a multiple
([align.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/proved/align.h#L56-L70)):

```c
/*@
  requires align > 0;
  requires \valid(out);
  assigns *out;
  ensures binary: \result == 0 || \result == 1;
  ensures rejects_only_on_wrap:
            \result != 0 <==> (x % align == 0
                              || (x / align + 1) * align <= UINT64_MAX);
  ensures aligned:
            \result != 0 ==> (\exists integer k; *out == k * align);
  ensures never_below: \result != 0 ==> *out >= x;
  ensures rounds_up_once: \result != 0 ==> *out < x + align;
  ensures untouched_on_reject: \result == 0 ==> *out == \old(*out);
 */
static inline int align_up_ok(uint64_t x, uint64_t align, uint64_t *out)
```

Read line by line:

- `requires` lines are preconditions: what the caller must guarantee. Here the
  alignment is not 0 and `out` points at valid memory.
- `assigns *out` says the function changes nothing else.
- Each `ensures` line is a guarantee about the result. It returns 0 or 1. It
  refuses exactly when the rounded value would not fit in 64 bits, and never
  otherwise. On success the result is a multiple of `align`, is at least `x`,
  and is less than `x + align`. On refusal `*out` is untouched.

Each target runs with `make verify-<name>`:

```sh
$ make verify-align
...
  PROVE   src/proved/align.h (Frama-C WP: weakest-precondition prover)
          claim: for ANY address, alignment, and search window,
                 these compute no out-of-bounds access and no overflow
                 - align_up_ok
                 - window_fits
          memory model: typed;  data model: gcc_x86_64
          (data model is type widths only, and matches arm64
           macOS on width, order, alignment and char signedness)
  PROVED   24 of 24 proof obligations discharged by alt-ergo/z3
           (holds for the ACSL contracts in src/proved/align.h;
            the region-array walk around them stays test-covered, not proved)
```

The last line says what the proof does not cover, and `docs/internals.md` says
it for all of them
([internals.md](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md?plain=1#L1820-L1822)):
"the I/O around the proved arithmetic (`pread`, `malloc`) stays test-covered,
and preconditions at call sites in files the analyzer cannot parse are
review-only." `mk/verify.mk` defines 21 such targets. The ELF check from part 4,
`elf_add_no_wrap()`, is proved by `verify-elf`, one of them.

## Checking the proofs

Among other checks, elfuse looks for three ways a passing proof can cover less
than intended.

The first is a proof with too few obligations. An emptied function body or a
dropped contract leaves WP fewer obligations, and the rest can all still pass.
`scripts/check-wp-result.py` requires a minimum number of obligations per target
([check-wp-result.py](https://github.com/sysprog21/elfuse/blob/73d8246b/scripts/check-wp-result.py#L9-L10)).

The second is an unproved contract. When a proved function calls
another function with a contract, WP assumes that contract without proving it,
unless the second function is also in the proof set. That has happened
([check-acsl-coverage.py](https://github.com/sysprog21/elfuse/blob/73d8246b/scripts/check-acsl-coverage.py#L4-L10)).
`hex_nibble`, a helper of the GDB stub's packet parser, sat outside its proof
set. Replacing its body with `return 15;`, which contradicts its own contract,
still produced "PROVED 74 of 74", because the proofs that used it relied on its
contract and nothing else.

`scripts/check-acsl-coverage.py` fails when any function with a contract is
missing from its target's proof set.

The third is a contract too weak to reject a bug. Mutation testing checks for
that: make a small deliberate bug in a copy of the code and confirm the proof
fails. `scripts/check-mutants.py` holds 121 such bugs across the 21 targets.
The four for `align.h`:

```sh
$ python3 scripts/check-mutants.py --target align
  RESOURCE verify-align     align_up_ok                  drop the overflow guard (the top multiple wraps to a lower address)  (prover exhausted, not refuted; load 1.6/cpu)
  RESOURCE verify-align     align_up_ok                  round down instead of up (the result can sit below the input)  (prover exhausted, not refuted; load 1.6/cpu)
  RESOURCE verify-align     window_fits                  test the sum instead of the difference (the sum wraps first)  (prover exhausted, not refuted; load 1.6/cpu)
  RESOURCE verify-align     window_fits                  accept a window that overruns the limit by one  (prover exhausted, not refuted; load 1.6/cpu)

  4 mutations, 4 caught (4 resource verdicts)
    4 caught by exhausting the prover rather than by refutation, at the default budget only (1.6/cpu at the end).
    What makes that evidence is the baseline: it proves every goal, and each mutant exhausts on the one its own function owns.
    That an exhausted mutant is unprovable rather than slow is not measured here; --escalate SECONDS measures it.
...
```

All four were caught by exhausting the prover, which is why each mutant runs at
the proof's own time budget, and the script calls lowering it unsound
([check-mutants.py](https://github.com/sysprog21/elfuse/blob/73d8246b/scripts/check-mutants.py#L30-L35)):

> A broken contract does not get refuted; the goal simply becomes unprovable
> and the prover grinds until the budget expires, so "caught" is reported as a
> timeout. A goal that is merely hard but still true times out the same way.
> The two are indistinguishable by verdict, so a shorter budget silently
> converts a genuine MISS into a "caught" and hides exactly the gap this file
> exists to find.

## Git hooks

A Git hook is a script Git runs at a fixed moment, such as before recording a
commit. The first `make` in a fresh clone of elfuse installs hooks that run the
same checks as the project's continuous integration (CI), the servers that
test every pull request: formatting, banned functions, and the commit message
rules
([CONTRIBUTING.md](https://github.com/sysprog21/elfuse/blob/73d8246b/CONTRIBUTING.md?plain=1#L1256-L1262)).
The build file explains why they install themselves
([common.mk](https://github.com/sysprog21/elfuse/blob/73d8246b/mk/common.mk#L22-L25)):
"Left to "make install-hooks" they are enforced on whoever read the README
carefully, which is not the population that needs them".

## Series recap

1. A program asks the kernel for everything with `svc`, and Linux and macOS
   ask differently.
2. Hypervisor.framework lets a process run a VM whose EL1 is elfuse's shim, so
   a system call becomes an exit into elfuse.
3. elfuse runs a program in five steps: load, boot, then run, translate, and
   return for each system call.
4. The loader reads ELF program headers, loads the dynamic linker from a
   sysroot, and builds the stack and auxiliary vector.
5. The shim has elfuse turn on the MMU with `hvc #4` while the VM runs, saves
   all 31 registers on every system call, and forwards it with `hvc #5`.
6. One slab becomes a Linux address space through page tables that elfuse
   writes, with 4 KiB guest pages on a 16 KiB host.
7. A generated table sends each system call to its handler, handlers translate
   macOS results, and the shim answers some calls with no exit.
8. One function resolves every path, and on a case-insensitive disk, sysroot
   names with uppercase or non-ASCII bytes are stored escaped, so names that
   differ only in case stay distinct.
9. Signals get a Linux signal frame on the program's stack, and `rt_sigreturn`
   restores it.
10. Each thread is a vCPU, futex waits are answered in the shim when pointless,
    and the vDSO reads the clock in a few nanoseconds, without a system call.
11. `fork` starts a second elfuse and gives it the guest's memory as an APFS
    clone.
12. Crash reports cover elfuse's failures, and a GDB stub debugs the program
    through register snapshots.
13. x86_64 programs run through Apple's Rosetta for Linux, hosted as an arm64
    guest.
14. A real Linux kernel under QEMU checks behavior, and Frama-C proofs, checked
    by mutation, cover the arithmetic.

## Glossary

| Term | Meaning | Part |
|---|---|---|
| ELF | the Linux executable file format | 1 |
| system call | a request from a program to the kernel | 1 |
| register | a storage slot inside the processor | 1 |
| exception level | a privilege level: EL0 programs, EL1 kernels, EL2 hypervisors | 2 |
| hypervisor | software that runs whole operating systems as guests | 2 |
| vCPU | a virtual processor, run by a host thread | 2 |
| exit | the hypervisor handing control back to the host thread | 2 |
| page table | the tree the MMU uses to translate addresses and check permissions | 2 |
| TLB | the processor's cache of address translations | 2 |
| slab | the region of elfuse's memory that is the guest's memory | 2 |
| shim | elfuse's assembly code at the guest's EL1 | 2 |
| dispatch table | the array from syscall number to handler | 3 |
| dynamic linker | the program that loads shared libraries at start-up | 4 |
| sysroot | a directory holding a Linux system's files | 4 |
| auxiliary vector | values the loader passes to a starting program | 4 |
| exception vector table | the block of handler code, one slot per kind of exception and where it came from | 5 |
| TLBI | an instruction that flushes cached translations | 6 |
| demand paging | supplying memory for a page only when it is first used | 6 |
| copy-on-write | sharing pages until one side writes | 6 |
| errno | the number that says why a system call failed | 7 |
| case folding | treating names that differ only in case as the same | 8 |
| procfs | the `/proc` file system, generated on request | 8 |
| signal | a notification delivered to a process, possibly to a handler | 9 |
| signal frame | the saved state written to the stack before a handler runs | 9 |
| futex | the kernel call threads use to wait for each other | 10 |
| vDSO | kernel-provided code that runs in user mode | 10 |
| fork | creating a copy of a process | 11 |
| zombie | an exited process whose status nobody has collected | 11 |
| GDB remote protocol | the protocol GDB uses to control a program through a stub | 12 |
| binary translation | converting code for one processor into code for another | 13 |
| XFAIL | a recorded, expected difference from Linux | 14 |
| sanitizer | compiler-added run-time checks for memory errors, undefined behavior, or races | 14 |
| contract | a function's stated preconditions and guarantees, written in ACSL | 14 |
| mutation testing | checking tests or proofs by confirming they reject deliberate bugs | 14 |

## What we learned

- elfuse compares its behavior with a real Linux kernel booted under QEMU,
  running the same test programs in both places.
- Known differences are recorded as XFAIL rows that print both values, so a
  change in elfuse's value shows in the output.
- Arithmetic on guest-controlled numbers is proved with Frama-C to meet
  contracts that rule out wraparound and out-of-bounds access, and three extra
  checks catch proofs with too few obligations, proofs that rest on unproved
  contracts, and contracts too weak to reject a deliberate bug.
- A mutation also counts as caught when the prover runs out of time, so mutants
  run at the proof's own time budget.
