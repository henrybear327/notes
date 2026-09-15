---
title: 'mini-elfuse 1: Loading a Linux program'
date: 2026-09-15T10:00:00+02:00
series: ["mini-elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "mini-elfuse", "linux", "macos", "elf"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 1 of a series that builds mini-elfuse, a small version of
[elfuse](https://github.com/sysprog21/elfuse). The
[elfuse series](../../elfuse/01-what-happens-when-you-run-a-program/) explains
how elfuse runs Linux programs on a Mac. This series writes a program that
does the same for simple programs and keeps only the parts needed to see how
each piece works. By part 5 it runs a static C program that calls `printf`,
cross-compiled on the Mac; parts 6 to 8 add files, `mmap`, and threads.

Each part adds to the code of the part before and ends with the complete code.
The code for each part is also served with this site, one directory per part.

All code and output in this series come from an Apple M4 Mac with macOS 26.6.2,
Homebrew's `clang` 23.1.1, and the `aarch64-linux-gnu-gcc` 15.2.0 cross compiler
with glibc 2.28 from the
[messense/macos-cross-toolchains](https://github.com/messense/homebrew-macos-cross-toolchains)
Homebrew tap.

## The plan

[elfuse part 3](../../elfuse/03-from-launch-to-exit/) splits a run into five
steps: load, boot, run, translate, and return. The parts of this series, with
the elfuse parts that cover the same topics:

| Part | Adds | elfuse part |
|---|---|---|
| 1 | reading a Linux program into memory | 4 |
| 2 | a virtual machine that runs the program to its first system call | 2, 5 |
| 3 | page tables | 6 |
| 4 | the `write` and `exit` system calls | 7 |
| 5 | what glibc needs before `main`: the initial stack, `brk`, `uname` | 4, 7 |
| 6 | files | 7, 8 |
| 7 | `mmap` | 6 |
| 8 | threads | 10 |

## The problem

A Linux program is an ELF file. Before its first instruction runs, the parts
of the file that hold code and data must be in memory at the addresses the
file names. On Linux the kernel does that. mini-elfuse has to do it itself,
and it needs memory to put the program in.

## A test program

The first test program is a short assembly file, like `tests/hello.S` in
elfuse. It writes `hello\n` to standard output with the `write` system call
(number 64) and ends with `exit` (number 93):

```asm
/* write(1, "hello\n", 6), then exit(0) */
    .text
    .globl _start
_start:
    mov x0, #1
    adr x1, msg
    mov x2, #6
    mov x8, #64         /* write */
    svc #0
    mov x0, #0
    mov x8, #93         /* exit */
    svc #0
msg:
    .ascii "hello\n"
```

The cross compiler builds it into a Linux program. `-nostdlib` leaves out the
C library and its startup code, which has a `_start` of its own. `-static`
matters from part 5 on, for C programs:

```sh
$ mkdir -p build
$ aarch64-linux-gnu-gcc -nostdlib -static -o build/hello hello.S
$ aarch64-linux-gnu-readelf -h -l build/hello
ELF Header:
  Magic:   7f 45 4c 46 02 01 01 00 00 00 00 00 00 00 00 00
  Class:                             ELF64
  Data:                              2's complement, little endian
  ...
  Type:                              EXEC (Executable file)
  Machine:                           AArch64
  ...
  Entry point address:               0x4000d4
  Start of program headers:          64 (bytes into file)
  ...
  Size of program headers:           56 (bytes)
  Number of program headers:         2
  ...
Program Headers:
  Type           Offset             VirtAddr           PhysAddr
                 FileSiz            MemSiz              Flags  Align
  LOAD           0x0000000000000000 0x0000000000400000 0x0000000000400000
                 0x00000000000000fa 0x00000000000000fa  R E    0x10000
  NOTE           0x00000000000000b0 0x00000000004000b0 0x00000000004000b0
                 0x0000000000000024 0x0000000000000024  R      0x4
...
```

Only the `LOAD` header asks for memory: copy `0xfa` bytes from offset 0 of the
file to address `0x400000`. The first instruction is at `0x4000d4`, after the
ELF header, the two program headers, and a build-id note that the linker
placed in the same range.

## The slab

The guest's memory is one block of the mini-elfuse process's own memory, the
slab from [elfuse part 2](../../elfuse/02-what-a-virtual-machine-is/). Guest
address X is byte X of the slab. mini-elfuse uses 1 GiB and keeps the part
below 4 MiB for itself:

```c
/* Guest address X is slab[X]. mini-elfuse keeps the memory below USER_BASE
 * for itself; the program gets the rest.
 */
#define GUEST_SIZE (1ULL << 30)
#define USER_BASE 0x400000ULL
```

`main()` reserves it with `mmap`:

```c
    slab = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
                MAP_ANON | MAP_PRIVATE, -1, 0);
```

macOS backs a page of this memory with physical memory only when something
first uses it, so the untouched part uses none.

## Reading the header

macOS has no `elf.h`, so mini-elfuse declares the two structures it reads. An
ELF header is 64 bytes and a program header is 56:

```c
struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct elf64_phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
```

`load_elf()` reads the header and refuses the file unless its first four bytes
are `7f 45 4c 46` and it is a 64-bit, little-endian aarch64 executable
(`ET_EXEC`) with 56-byte program headers:

```c
    struct elf64_ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != sizeof eh ||
        memcmp(eh.e_ident, "\177ELF", 4) != 0)
        die("%s: not an ELF file", path);
    /* e_ident[4] == 2: 64-bit, e_ident[5] == 1: little-endian */
    if (eh.e_ident[4] != 2 || eh.e_ident[5] != 1 || eh.e_type != ET_EXEC ||
        eh.e_machine != EM_AARCH64 ||
        eh.e_phentsize != sizeof(struct elf64_phdr))
        die("%s: not a 64-bit aarch64 executable", path);
```

A position-independent program (`ET_DYN`) would need a load address picked
for it, and a dynamically linked one needs its dynamic linker loaded as well
([elfuse part 4](../../elfuse/04-loading-a-linux-program/)). mini-elfuse
supports neither, so every test program in this series is built with
`-static`.

## Copying segments

For each program header, `load_elf()` rejects a dynamically linked program,
skips everything that is not `PT_LOAD`, checks where the segment goes, and
reads it from the file straight into the slab:

```c
        if (ph.p_type == PT_INTERP)
            die("%s: dynamically linked, build it with -static", path);
        if (ph.p_type != PT_LOAD)
            continue;

        void *dst = guest_ptr(ph.p_vaddr, ph.p_memsz);
        if (!dst || ph.p_filesz > ph.p_memsz)
            die("%s: segment at 0x%llx does not fit", path, ph.p_vaddr);
        /* The slab is fresh anonymous memory, so the rest of p_memsz is
         * already zero.
         */
        if (pread(fd, dst, ph.p_filesz, ph.p_offset) != (ssize_t) ph.p_filesz)
            die("%s: cannot read segment at 0x%llx", path, ph.p_vaddr);
```

A segment can be larger in memory than in the file (`p_memsz` above
`p_filesz`) for variables that start at zero. elfuse zeroes that part; a new
slab is already zero, so mini-elfuse skips it.

The address and size come from the file, so `guest_ptr()` checks them before
anything is copied:

```c
/* Host address of guest range [addr, addr + len), or NULL unless the whole
 * range is program memory.
 */
static void *guest_ptr(uint64_t addr, uint64_t len)
{
    if (addr < USER_BASE || addr > GUEST_SIZE || len > GUEST_SIZE - addr)
        return NULL;
    return slab + addr;
}
```

The check never computes `addr + len`. Both are 64-bit numbers from the file,
and their sum can wrap around to a small value that passes a bound check,
the problem elfuse part 4 describes. `GUEST_SIZE - addr` cannot wrap, because
the condition before it has already established `addr <= GUEST_SIZE`. Later
parts check every address a program passes to a system call with this
function, and `brk` has bounds of its own, so a program cannot make mini-elfuse
read or write outside program memory.

## Running it

`-v` prints each loaded segment and the entry point. Nothing runs yet:

```sh
$ build/mini-elfuse -v build/hello
load 0x400000-0x4000fa
entry 0x4000d4
```

Both match the `readelf` output. [Part 2](../02-a-vm-and-the-first-system-call/)
creates a virtual machine and starts the program at `0x4000d4`.

## The code

The files are served with this site under `mini-elfuse/step-1/`. To fetch,
build, and run them, with the cross compiler installed:

```sh
mkdir step-1 && cd step-1
for f in Makefile mini-elfuse.c hello.S; do
    curl -fsSO https://henrybear327.github.io/notes/mini-elfuse/step-1/$f
done
make check
```

`Makefile`:

{{< include-file "static/mini-elfuse/step-1/Makefile" "make" >}}

`mini-elfuse.c`:

{{< include-file "static/mini-elfuse/step-1/mini-elfuse.c" "c" >}}

`hello.S`:

{{< include-file "static/mini-elfuse/step-1/hello.S" "asm" >}}

## What we learned

- A static Linux program is loaded by copying each `PT_LOAD` segment to the
  address its program header names.
- The guest's memory is one `mmap`ed slab, and guest address X is slab byte X.
- Every address that comes from the program is checked against program memory
  before use, without computing a sum that can wrap.
