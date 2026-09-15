---
title: 'elfuse 4: Loading a Linux program'
date: 2026-09-09T13:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "elf"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 4 of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 3](../03-from-launch-to-exit/) split a run into five steps. This part is
the first one, load: turning a file on disk into memory a program can run in.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac. The Linux files used here come from the Alpine Linux fixtures
that `tests/fetch-fixtures.sh` downloads into `externals/test-fixtures/`.

## The problem

A program file is built for particular addresses. `test-hello` from part 1 was
linked to run at `0x400000`, and a program built that way may contain
addresses fixed at build time, such as pointers to its own data, that are only
right if every byte lands exactly where the file says. When the first
instruction runs, the program also expects its command-line arguments and
environment variables on the stack, in a layout Linux defines.

On Linux the kernel sets all of this up when a program starts. Under elfuse
there is no Linux kernel, so elfuse does it before the vCPU starts.

## What the file says

An ELF file starts with a header that says what kind of file it is, followed
by program headers. A program header describes one segment: a range of the
file that must be copied to a given address, with given permissions. The
`readelf` tool from binutils prints both:

```sh
$ aarch64-elf-readelf -h -l build/test-hello
ELF Header:
  Magic:   7f 45 4c 46 02 01 01 00 00 00 00 00 00 00 00 00
  Class:                             ELF64
  Data:                              2's complement, little endian
  ...
  Type:                              EXEC (Executable file)
  Machine:                           AArch64
  Entry point address:               0x400000
  ...
Program Headers:
  Type           Offset             VirtAddr           PhysAddr
                 FileSiz            MemSiz              Flags  Align
  LOAD           0x0000000000010000 0x0000000000400000 0x0000000000400000
                 0x0000000000000026 0x0000000000000026  R E    0x10000
```

The program has one program header of type `PT_LOAD`, the type that means
"copy this into memory". It says: take `0x26` bytes starting at offset
`0x10000` in the file and place them at address `0x400000`, readable and
executable. The entry point, the address of the first instruction to run, is
also `0x400000`.

## Checking the header

`elf_load()` in `src/core/elf.c` opens the file, and `elf_load_fd()` reads the
header and refuses anything elfuse cannot run
([elf.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/elf.c#L465-L499)):

- The first four bytes must be `7f 45 4c 46`.
- The class must be 64-bit and the byte order little-endian.
- The machine must be aarch64 or x86_64. x86_64 programs take a different path,
  covered in part 13.
- The type must be an executable (`EXEC`) or a position-independent one
  (`DYN`).

Then it walks the program headers and records every `PT_LOAD`. A program
header is data from the file, and the file might be damaged or crafted to
attack the loader. A segment's end address is its start plus its size, and
both are 64-bit numbers from the file, so the sum can overflow and wrap around
to a small number. elfuse computes such sums with a helper that fails instead
of wrapping
([elf.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/elf.c#L47-L68)):

```c
/* a + b, or 0 when the sum does not fit in 64 bits.
 *
 * ELF address fields are unconstrained uint64_t, so the addresses derived from
 * them (segment end, program header GPA) can only be formed with the carry
 * checked. Saturating to UINT64_MAX instead is worse than rejecting: every
 * consumer adds a load base to the result, so the clamp only relocates the wrap
 * into the consumer, where the bound check it defeats reads "elf_end >
 * guest_size" and silently passes.
 */
/*@
  requires \valid(sum);
  assigns *sum;
  ensures \result != 0 ==> *sum == a + b;
 */
static bool elf_add_no_wrap(uint64_t a, uint64_t b, uint64_t *sum)
{
    if (a > UINT64_MAX - b)
        return false;

    *sum = a + b;
    return true;
}
```

Clamping an overflowing end to the largest 64-bit value does not help: later
code adds another offset to it, which wraps anyway, and the size check after
that compares a small wrapped number and passes. So elfuse rejects the file.
The block between `/*@` and `*/` is a specification of what the function
requires and guarantees, written so that a tool can prove the code meets it.
Part 14 explains how.

## Copying segments into the slab

Guest address X is slab byte X (part 2), so placing a segment is a read from
the file straight into the slab. `elf_map_segments_fd()`, which
`elf_map_segments()` calls, does it in two passes
([elf.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/elf.c#L817-L845)).
The first pass zeroes the parts of each segment the file does not supply. A
segment's size in memory can be larger than its size in the file, for
variables that start out as zero. The second pass reads the file bytes:

```c
        if (pread(fd, (uint8_t *) guest_base + gpa, filesz,
                  info->segments[i].offset) != (ssize_t) filesz) {
```

The source explains the two passes: zero filling runs to the end of the page
the segment ends in, which can reach into the next segment, so zeroing
everything first means no fill can overwrite file data already in place.

## Programs that need other files

`test-hello` contains everything it runs. Most programs do not. They call
functions in shared libraries: files of compiled code, such as the C library,
that many programs use and the system loads at run time. A program built that
way is dynamically linked. Busybox from Alpine Linux is an example:

```sh
$ file externals/test-fixtures/rootfs/bin/busybox
externals/test-fixtures/rootfs/bin/busybox: ELF 64-bit LSB pie executable, ARM aarch64, version 1 (SYSV), dynamically linked, interpreter /lib/ld-musl-aarch64.so.1, ...
```

The Linux kernel does not load the libraries. It loads a helper named
in the program, the dynamic linker, and starts it instead of the program. The
dynamic linker opens the shared libraries, fills in the addresses the program
needs, and then jumps to the program's entry point. The program names its
dynamic linker in a program header of type `PT_INTERP` ("interpreter"):

```sh
$ aarch64-elf-readelf -l externals/test-fixtures/rootfs/bin/busybox
Elf file type is DYN (Position-Independent Executable file)
Entry point 0x7500
...
  INTERP         0x0000000000000238 0x0000000000000238 0x0000000000000238
                 0x000000000000001a 0x000000000000001a  R      0x1
      [Requesting program interpreter: /lib/ld-musl-aarch64.so.1]
  LOAD           0x0000000000000000 0x0000000000000000 0x0000000000000000
                 0x00000000000d1a4c 0x00000000000d1a4c  R E    0x10000
  LOAD           0x00000000000dc728 0x00000000000ec728 0x00000000000ec728
                 0x0000000000003911 0x00000000000043e8  RW     0x10000
...
```

Two more things differ. The type is `DYN`: a position-independent
executable, or PIE, whose code works at any address. Its segments start at
address 0, and the loader picks where they go, called the load base.
elfuse loads a PIE program at 4 MiB
([bootstrap.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/bootstrap.c#L445-L446)):

```c
        boot->elf_load_base =
            (boot->elf_info.e_type == ET_DYN) ? PIE_LOAD_BASE : 0;
```

`PIE_LOAD_BASE` is `0x00400000`, so busybox's entry point `0x7500` ends up at
`0x407500`. The second `LOAD` also has a larger size in memory (`0x43e8`) than
in the file (`0x3911`); the first pass zeroes that difference and the rest of
its last page.

## Where the dynamic linker comes from

The dynamic linker's path, `/lib/ld-musl-aarch64.so.1`, is a Linux path. The
Mac has no such file:

```sh
$ build/elfuse externals/test-fixtures/rootfs/bin/busybox echo hi
/lib/ld-musl-aarch64.so.1: No such file or directory
19:07:41 ERROR src/core/bootstrap.c:263: failed to load interpreter: /lib/ld-musl-aarch64.so.1
```

A sysroot is a directory that holds a Linux system's files, laid out as they
would be on Linux. The `--sysroot` option tells elfuse to look up absolute
guest paths inside it first. The Alpine fixture is one:

```sh
$ build/elfuse --sysroot externals/test-fixtures/rootfs /bin/busybox echo hi
19:07:41 WARN  src/core/sysroot.c:409: sysroot externals/test-fixtures/rootfs is case-insensitive; ...
hi
```

The warning is about file names that differ only in case, the subject of part
8. With `-v` (timestamps removed, other lines omitted), the loader shows where
everything went:

```text
DEBUG src/core/bootstrap.c:406: ELF entry=0x7500, 2 segments, load range [0x0, 0xf0b10), machine=aarch64
DEBUG src/core/bootstrap.c:254: loading interpreter: externals/test-fixtures/rootfs/lib/ld-musl-aarch64.so.1
DEBUG src/core/bootstrap.c:286: interpreter loaded at base=0xff00000000, entry=0xff00069670, 2 segments
DEBUG src/core/bootstrap.c:495: shim loaded at offset 0xfeffdf6000 (7508 bytes)
DEBUG src/core/bootstrap.c:667: SP=0x7ffc420, entry=0xff00069670 (via interpreter)
```

The dynamic linker is itself a position-independent ELF file. elfuse loads it
with the same two functions, at `0xff00000000`, 4 GiB below the end of the 1
TiB slab. The vCPU's first instruction is then the dynamic linker's entry
point, not the program's
([bootstrap.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/bootstrap.c#L655-L657)):

```c
        boot->entry_point = (boot->interp_base != 0)
                                ? (boot->interp_info.entry + boot->interp_base)
                                : (boot->elf_info.entry + boot->elf_load_base);
```

## The initial stack

The program also expects a stack. When a Linux program starts, the stack
pointer points at a block the kernel prepared, which `build_linux_stack()` in
`src/core/stack.c` builds instead
([stack.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/stack.c#L145-L158)):

```c
    /* Linux initial stack layout (growing from high to low):
     *   [ 16 random bytes for AT_RANDOM ]
     *   [ "aarch64\0" for AT_PLATFORM ]
     *   [ environment strings ]
     *   [ argument strings ]
     *   [ padding to 16-byte alignment ]
     *   [ AT_NULL (0, 0) ]
     *   [ auxv entries (key, value) pairs ]
     *   [ NULL (end of envp) ]
     *   [ envp[0], envp[1], ... ]
     *   [ NULL (end of argv) ]
     *   [ argv[argc-1] ... argv[0] ]
     *   [ argc ]                    <-- SP points here
     */
```

The pointer arrays and the auxiliary vector sit where Linux puts them. The
strings at the top and the order of the auxiliary vector entries differ from
Linux; programs find both through pointers and keys.

`argc` is the number of arguments, and `argv` and `envp` are arrays of pointers
to the argument and environment strings. `main(int argc, char **argv)` in a C
program receives the first two. Above them sits the auxiliary vector, or auxv:
a list of (key, value) pairs in which the loader passes information, such as
where the program was loaded, to the program and especially to the dynamic
linker.

A program can read its own auxiliary vector from `/proc/self/auxv`. Busybox's
`od` prints it as pairs of 64-bit numbers (the sysroot warning is left out
here and below):

```sh
$ build/elfuse --sysroot externals/test-fixtures/rootfs /bin/busybox od -A d -t x8 /proc/self/auxv
0000000 0000000000000007 000000ff00000000
0000016 0000000000000021 000000000000f000
0000032 0000000000000006 0000000000001000
0000048 0000000000000003 0000000000400040
0000064 0000000000000004 0000000000000038
0000080 0000000000000005 0000000000000009
0000096 0000000000000009 0000000000407500
0000112 000000000000000b 00000000000003e8
0000128 000000000000000c 00000000000003e8
0000144 000000000000000d 00000000000003e8
0000160 000000000000000e 00000000000003e8
0000176 0000000000000017 0000000000000000
0000192 000000000000001a 0000000000004181
0000208 0000000000000010 00000000edb3fffb
0000224 0000000000000011 0000000000000064
0000240 0000000000000033 0000000000001400
0000256 0000000000000019 0000000007fffff0
0000272 000000000000001f 0000000007ffffdb
0000288 000000000000000f 0000000007ffffe8
0000304 0000000000000000 0000000000000000
0000320
```

The keys are numbers Linux defines. The ones that connect to this post:

| Key | Name | Value | Meaning |
|---|---|---|---|
| `0x07` | `AT_BASE` | `0xff00000000` | where the dynamic linker was loaded |
| `0x21` | `AT_SYSINFO_EHDR` | `0xf000` | the vDSO, a page of helper code elfuse provides (part 10) |
| `0x06` | `AT_PAGESZ` | `0x1000` | memory comes in 4096-byte pages |
| `0x03` | `AT_PHDR` | `0x400040` | the program headers, 64 bytes into the loaded file |
| `0x09` | `AT_ENTRY` | `0x407500` | the program's entry point, for the dynamic linker to jump to |
| `0x0b` to `0x0e` | `AT_UID` and others | `0x3e8` | user and group id 1000, elfuse's default guest identity |
| `0x19` | `AT_RANDOM` | `0x7fffff0` | the address of 16 random bytes near the top of the stack |
| `0x00` | `AT_NULL` | | end of the list |

`AT_PAGESZ` says 4 KiB, but the Mac underneath uses 16 KiB pages. Part 6 covers
the difference.

`AT_RANDOM` is the one entry that must be unpredictable
([stack.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/stack.c#L200-L211)).
glibc and musl take the stack canary from these 16 bytes, and glibc also takes
its pointer guard, which scrambles saved code pointers. Neither works if an
attacker can guess the value.

## Scripts

A script such as a shell script is not an ELF file, but on Linux it can still
be run directly if its first line starts with `#!` and names an interpreter.
The kernel runs the interpreter with the script's path as an argument. The
elfuse launcher does the same in a loop in `src/main.c`, because the
interpreter can itself be a script
([main.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/main.c#L634-L735)).
The loop stops after `ELF_SHEBANG_MAX_DEPTH` levels, which is 5, matching the
limit in the Linux kernel
([elf.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/elf.h#L210-L214)).

```sh
$ printf '#!/bin/busybox sh\necho "running $0 with $# args: $*"\n' \
    > externals/test-fixtures/rootfs/tmp/hello2.sh
$ chmod +x externals/test-fixtures/rootfs/tmp/hello2.sh
$ build/elfuse --sysroot externals/test-fixtures/rootfs /tmp/hello2.sh a b
running /tmp/hello2.sh with 2 args: a b
```

elfuse read the `#!` line, loaded `/bin/busybox` from the sysroot, and passed
it `sh`, the script path, and the two arguments.

## Heap and stack addresses

Loading also decides two more addresses
([bootstrap.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/bootstrap.c#L469-L478)).
The heap, the memory a program allocates while it runs, grows with `brk`, the
traditional call for more memory. It starts right after the loaded program, but
no lower than 16 MiB. The stack is 8 MiB, and its top is 8 MiB above the start
of that region rounded up to 2 MiB, but no lower than `0x08000000`. For
`test-hello` the top is `0x08000000`, which is why its `SP` in part 3 was
`0x7ffc430`, below that top.

## What we learned

- An ELF file's program headers list segments. `PT_LOAD` segments are copied
  into the slab at their addresses, and `PT_INTERP` names a dynamic linker.
- elfuse checks the addresses and sizes in the program headers and rejects a
  file whose segment addresses overflow, instead of clamping them.
- A position-independent program is placed at 4 MiB. A dynamically linked
  program starts in its dynamic linker, which elfuse loads from the sysroot.
- The initial stack holds `argc`, `argv`, the environment, and the auxiliary
  vector, which gives the page size as 4 KiB.

[Part 5](../05-booting-a-vm-with-no-kernel/) covers the boot step: the shim,
elfuse's assembly code at EL1, and how the vCPU gets from its first instruction
to the program's.
