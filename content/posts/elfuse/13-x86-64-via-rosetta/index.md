---
title: 'elfuse 13: x86_64 programs through Rosetta'
date: 2026-09-09T22:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "rosetta", "x86_64"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 13 of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 12](../12-debugging-a-guest/) covered debugging. Every program so far was
built for arm64, the instruction set of the Mac's processor. This part is about
Linux programs built for x86_64, the instruction set of Intel and AMD
processors, and how elfuse runs them without decoding any x86_64 instruction
itself.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac. The x86_64 programs are the Alpine Linux fixtures that
`INCLUDE_X86_64=1 tests/fetch-fixtures.sh` downloads.

## The problem

Part 1 said the processor is not the problem for arm64 Linux programs. For
x86_64 programs it is: an Apple Silicon processor cannot execute x86_64
instructions.

One way around this is to interpret: decode and carry out one instruction at a
time, which is slow. The faster way is binary translation: turn a block of
x86_64 code into equivalent arm64 code once, then run the arm64 version directly
each time the block runs. QEMU does this for many processors. Apple's Rosetta
does it for Mac programs, and Apple also ships a Rosetta for Linux, meant for
Linux virtual machines on a Mac. elfuse runs that translator.

## Running an x86_64 program

```sh
$ file externals/test-fixtures/x86_64-musl/staticbin/bin/busybox
externals/test-fixtures/x86_64-musl/staticbin/bin/busybox: ELF 64-bit LSB executable, x86-64, version 1 (SYSV), statically linked, ...
$ build/elfuse externals/test-fixtures/x86_64-musl/staticbin/bin/busybox uname -m
x86_64
$ build/elfuse --sysroot externals/test-fixtures/x86_64-musl/rootfs /bin/busybox echo hi from x86_64
hi from x86_64
```

(The sysroot warning from part 4 is left out.) elfuse picks the path from the
machine field of the ELF header (part 4), so no option is needed. Both static
and dynamically linked x86_64 programs run. Translation adds time: over nine
runs each, on a Mac under load (load average about 9), the median time for
`busybox true` was 20.6 ms for the arm64 build and 499.1 ms for the x86_64 build
through Rosetta.

## What elfuse loads

Rosetta for Linux is itself an arm64 Linux program:

```sh
$ ls /Library/Apple/usr/libexec/oah/RosettaLinux/
rosetta
rosettad
```

For an x86_64 program, elfuse loads `rosetta` with the loader from part 4 and
starts it, instead of loading the program. Some of the `-v` lines about Rosetta
show it (timestamps removed):

```text
DEBUG src/core/bootstrap.c:406: ELF entry=0x41a7f1, 4 segments, load range [0x400000, 0x4fe1d8), machine=x86_64-via-rosetta
DEBUG src/core/rosetta.c:211: rosetta: ELF entry=0x800000026000 load=[0x800000000000,0x8000001a50e0)
DEBUG src/core/rosetta.c:307: rosetta: GPA=0xefee00000 VA=0x800000000000 size=2 MiB kbuf_gpa=0xeeee00000 ttbr1=0xeff010000
DEBUG src/core/rosetta.c:113: rosetta: using /proc/self/fd/3 as caps binary path for long target externals/test-fixtures/x86_64-musl/staticbin/bin/busybox
DEBUG src/core/rosetta.c:510: rosetta_finalize: argv=[/Library/Apple/usr/libexec/oah/RosettaLinux/rosetta, externals/test-fixtures/x86_64-musl/staticbin/bin/busybox, ...], target_fd=3
DEBUG src/core/bootstrap.c:667: SP=0x7ffc390, entry=0x800000026000 (via rosetta)
```

The first line reads busybox's header and finds an x86_64 program. After the
shim's start-up code, the first instruction to run is Rosetta's entry point,
`0x800000026000`. elfuse does not load the x86_64 file into guest memory;
Rosetta maps it itself once it runs
([bootstrap.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/bootstrap.c#L323-L324)):

> Rosetta guests never load the x86_64 ELF or its interpreter into guest
> memory; rosetta itself reads the target via fd 3 once it is running.

On Linux, running a program through an interpreter chosen by its file format is
the job of binfmt_misc, a kernel feature that runs a registered program with the
original file as an argument. elfuse imitates that
([rosetta.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/rosetta.c#L424-L425)):
Rosetta's arguments are `[rosetta, busybox's path, original arguments...]`, and
elfuse opens busybox at file descriptor 3 and publishes that number in the
auxiliary vector as `AT_EXECFD` (part 4). Rosetta reads the x86_64 program from
there, translates it, and runs it. Every Linux system call the translated code
makes goes through the shim and elfuse, as for an arm64 program.

## Mapping Rosetta at 128 TiB

Rosetta is linked to run at `0x800000000000`, which is 128 TiB. Part 2's slab
maps guest address X to slab byte X, and no slab here is that large. For an
x86_64 guest, elfuse also creates the VM with 48-bit guest physical addresses,
and on this Mac the large slab sizes then fail:

```text
INFO  src/core/guest.c:556: guest: hv_vm_map 1024 GiB failed (-85377021), trying smaller slab
INFO  src/core/guest.c:556: guest: hv_vm_map 256 GiB failed (-85377021), trying smaller slab
INFO  src/core/guest.c:551: guest: primary slab 64 GiB (36-bit) mapped
DEBUG src/core/bootstrap.c:425: IPA size: 48 bits (64 GiB primary)
```

So Rosetta's virtual addresses are not slab offsets. elfuse copies Rosetta's
bytes into the slab at a low guest physical address (the IPA from part 2),
`0xefee00000` in the log above, and writes page-table entries that map virtual
address `0x800000000000` to that physical address
([guest.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.h#L192-L197)):

> Rosetta segments use va_base != 0 to install a non-identity mapping: the
> rosetta ELF is statically linked at 0x800000000000 (128 TiB) but its bytes
> live in the primary buffer at a low GPA.

According to elfuse's source, Rosetta also expects memory at addresses in the
top half of the 64-bit range, starting at `0xFFFFFFFFF0000000`, where Linux
kernels live. arm64 processors translate the two halves of the address space
with separate page tables: `TTBR0_EL1` points at the lower half's tables, and
`TTBR1_EL1` at the upper half's, which Linux uses for the kernel. elfuse
normally leaves the upper half off. For Rosetta it turns it on and maps 256 MiB
there, called the kbuf, and maps the same memory again at the matching
lower-half address, because Rosetta sometimes strips the top bits of a pointer
before using it
([guest.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.h#L147-L158)).

## Rosetta's VM checks

Rosetta for Linux checks early at start-up that it runs inside Apple's
Virtualization.framework. It asks through `ioctl` calls, commands sent to a file
descriptor, and exits if the answers are wrong. elfuse answers them
([rosetta.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/rosetta.h#L49-L59)):

```c
/* Rosetta's Virtualization.framework probe ioctls. Rosetta issues these on an
 * open fd very early at startup to verify that it is running inside a supported
 * VZ environment. Without affirmative responses, rosetta prints "Rosetta is
 * only intended to run on Apple Silicon ..." and exits.
 *
 * Reverse-engineered from the rosetta binary; values captured via strace in a
 * VZ Linux VM.
 */
#define ROSETTA_VZ_CHECK 0x80456125 /* Returns 69-byte signature */
#define ROSETTA_VZ_CAPS 0x80806123  /* Returns 128-byte capability blob */
#define ROSETTA_VZ_ACTIVATE 0x6124  /* Activate VZ mode (just returns 1) */
```

The capability answer includes the program's path in a 42-byte field. The
fixture's path is longer, which is what the `/proc/self/fd/3` line in the trace
above is about: elfuse puts that shorter name, which leads to the same file, in
the field instead.

## Translating ahead of time

Translating code as it runs costs time on every launch. Rosetta can also ask a
helper daemon, `rosettad`, to translate a whole program ahead of time (AOT) and
keep the result. elfuse answers the daemon's socket itself
([rosetta.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/rosetta.h#L100-L102)):

> Rosetta opens AF_UNIX SOCK_SEQPACKET and connects to a socket; macOS lacks
> SOCK_SEQPACKET for AF_UNIX so elfuse intercepts socket(SEQPACKET) with
> socketpair(SOCK_STREAM) and runs a handler thread on the other end.

When Rosetta sends a program to translate, the handler thread computes the
SHA-256 hash of the program and looks for `<hash>.aot` in
`~/.cache/elfuse-rosettad`. On a miss, it translates by running Apple's
`rosettad` as an arm64 Linux program, through elfuse itself:
`elfuse rosettad translate <input> <output>`, which rewrites its own arguments
to start the real translator
([main.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/main.c#L286-L297)).

With an empty home directory, the first run leaves one file in
`~/.cache/elfuse-rosettad`, named by the hash of the busybox binary:

```sh
$ mkdir -p ~/elfuse-blog.noindex/home-cold
$ export HOME=~/elfuse-blog.noindex/home-cold
$ build/elfuse externals/test-fixtures/x86_64-musl/staticbin/bin/busybox true
$ ls ~/.cache/elfuse-rosettad
2571bdcaa3bc1bcc0b5d01c71d1c3b9ed21981f6bf1e98c458a86551cbd4545c.aot
$ shasum -a 256 externals/test-fixtures/x86_64-musl/staticbin/bin/busybox
2571bdcaa3bc1bcc0b5d01c71d1c3b9ed21981f6bf1e98c458a86551cbd4545c  externals/test-fixtures/x86_64-musl/staticbin/bin/busybox
```

Later runs reuse the file. For this small program the launch time did not
change measurably: four runs took 474, 514, 685, and 376 ms, with the machine
under the same load.

## Limitations

- `--gdb` refuses x86_64 programs, because the stub would show Rosetta's arm64
  state, not the x86_64 program's (part 12):

  ```text
  20:14:22 ERROR src/main.c:741: --gdb is not supported for x86_64 guests; the current stub only exposes the translated aarch64 view
  ```

- `docs/internals.md` records two behaviors that come from Rosetta itself:
  `SA_RESETHAND`, a flag that resets a signal handler after one delivery, is
  hidden by Rosetta's own signal state, and one form of thread creation can
  hang
  ([x86_64-via-Apple-Rosetta](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#x86_64-via-apple-rosetta)).
- A fork of an x86_64 guest never shares the parent's live memory file (part
  11), because reading memory that is still changing corrupts Rosetta's own
  data structures
  ([forkipc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/forkipc.c#L1784-L1791)).

## What we learned

- elfuse runs x86_64 Linux programs by loading Apple's Rosetta for Linux, an
  arm64 program, and giving it the x86_64 file on descriptor 3, the way
  binfmt_misc would.
- Rosetta is linked at 128 TiB, beyond the slab, so elfuse maps it there with
  page-table entries whose virtual address differs from the physical one,
  which arm64 guests never need, and enables the upper-half page tables for
  Rosetta's kernel addresses.
- elfuse answers Rosetta's reverse-engineered virtual machine checks and serves
  the `rosettad` socket itself, keeping translations in a cache keyed by
  SHA-256.
- Translated system calls go through the same shim and handlers as arm64 ones,
  and the debugger stub does not support x86_64 programs.

[Part 14](../14-testing-and-proofs/), the last part, covers how elfuse is
tested against a real Linux kernel and how its arithmetic is proved.
