---
title: 'mini-elfuse 7: mmap'
date: 2026-09-15T16:00:00+02:00
series: ["mini-elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "mini-elfuse", "linux", "macos", "memory"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 7 of a series that builds mini-elfuse, a small version of
[elfuse](https://github.com/sysprog21/elfuse).
[Part 6](../06-files/) added files. This part adds `mmap`, the call that gives
a program memory at an address the kernel picks, optionally filled from a
file.

## The problem

glibc's `malloc` serves large requests with `mmap` instead of the heap, and
programs map files with it. `mmap.c` does both:

```c
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
    /* glibc's malloc takes a request this large straight from mmap */
    size_t n = 1 << 20;
    unsigned char *buf = malloc(n);
    if (!buf) {
        perror("malloc");
        return 1;
    }
    unsigned long sum = 0;
    for (size_t i = 0; i < n; i++)
        buf[i] = i;
    for (size_t i = 0; i < n; i++)
        sum += buf[i];
    free(buf);
    printf("sum %lu\n", sum);

    int fd = open(argv[0], O_RDONLY);
    char *elf = mmap(NULL, 4, PROT_READ, MAP_PRIVATE, fd, 0);
    if (elf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    printf("%.3s\n", elf + 1);
    return 0;
}
```

The 1 MiB buffer holds 4096 runs of the bytes 0 to 255, so the sum is 4096
times 32640, 133693440. The second half maps the first 4 bytes of the program's
own file, which are `7f 45 4c 46`, and prints bytes 1 to 3: `ELF`.

With part 6's code, part of it works (the `load`, `entry`, and start-up lines
left out):

```sh
$ ../step-6/build/mini-elfuse -v build/mmap
...
syscall 222(0x0, 0x110000, 0x3, 0x22, 0xffffffffffffffff, 0x0)
  -> -38 (0xffffffffffffffda)
syscall 214(0x5c0000, 0x5c0000, 0xf0000, 0x492000, 0xffffffffffffffff, 0x0)
  -> 6029312 (0x5c0000)
syscall 214(0x4c0000, 0x4c0000, 0xfffffffffff00000, 0x492000, 0x414c80, 0x12ab10)
  -> 4980736 (0x4c0000)
...
syscall 222(0x0, 0x4, 0x1, 0x2, 0x3, 0x0)
  -> -38 (0xffffffffffffffda)
...
mmap: Function not implemented
...
sum 133693440
```

When `mmap` (222) fails, `malloc` grows the heap with `brk` instead and gives
the memory back after `free`, so the sum is right. The file mapping has no such
fallback. The error appears before the sum because standard error is written at
once and standard output is buffered until exit.

## Handing out addresses

Part 3 already mapped all program memory, `0x400000` to 1 GiB, readable,
writable, and executable. `mmap` has nothing to map, so it only picks an
address. A counter starting at `MMAP_BASE`, 256 MiB, above the stack, moves up
by each mapping's size rounded to whole 4 KiB pages:

```c
#define MMAP_BASE 0x10000000ULL
```

```c
static uint64_t mmap_next = MMAP_BASE;
```

```c
    case NR_mmap: {
        /* Every page is already mapped read-write-execute, so mmap only hands
         * out addresses. They are never reused, so they still read as zero.
         */
        if (a[3] & LINUX_MAP_FIXED || a[1] == 0) {
            ret = -LINUX_EINVAL;
            break;
        }
        uint64_t addr = mmap_next;
        void *p = guest_ptr(addr, a[1]);
        if (p)
            mmap_next += (a[1] + 0xfff) & ~0xfffULL;
        /* A file mapping is a copy, even with MAP_SHARED. */
        if (!p)
            ret = -LINUX_ENOMEM;
        else if (!(a[3] & LINUX_MAP_ANONYMOUS) &&
                 pread(a[4], p, a[1], a[5]) < 0)
            ret = host_ret(-1);
        else
            ret = addr;
        break;
    }
```

The arguments are `mmap(addr, length, prot, flags, fd, offset)`.

- An anonymous mapping, one not backed by a file, must read as zero. The slab
  starts zeroed and no address is handed out twice, so nothing needs clearing.
- `MAP_FIXED` asks for memory at exactly `addr`. The counter could hand out
  that range again later, and it would then not read as zero, so mini-elfuse
  returns `EINVAL` rather than memory somewhere else. A zero length is `EINVAL`
  on Linux too.
- When the next mapping would pass 1 GiB, `guest_ptr()` fails and the call
  returns `ENOMEM`.

```c
enum { LINUX_MAP_FIXED = 0x10, LINUX_MAP_ANONYMOUS = 0x20 };
```

## File mappings

A file mapping is filled with `pread` from the program's descriptor. For
`MAP_PRIVATE`, a copy behaves as Linux specifies: the program's writes never
reach the file. For `MAP_SHARED` they should, and here they do not. elfuse maps
a shared mapping over the slab with the host's `mmap` when the descriptor is
open for writing and the guest address and file offset fall on the Mac's
16 KiB page boundaries. In every other case it copies with `pread`, and it
writes a shared copy back when the program calls `msync`
([elfuse part 6](../../elfuse/06-guest-memory-and-page-tables/), "Two page
sizes").

## munmap and mprotect

```c
    case NR_munmap:
    case NR_mprotect:
        ret = 0;
        break;
```

`munmap` succeeds without doing anything: the address range is never handed
out again, and its memory stays part of the slab. `mprotect` succeeds without
changing permissions, since every page already allows everything. elfuse rewrites page table entries for `mprotect` and asks the shim
to flush cached translations
([elfuse part 6](../../elfuse/06-guest-memory-and-page-tables/), "Changing
permissions later"). Part 8 needs `mprotect` to succeed, because glibc sets up
thread stacks with `mmap` and `mprotect`.

## Running it

```sh
$ build/mini-elfuse build/mmap
sum 133693440
ELF
```

The calls, with the output sent to a file and shown from the first `mmap`:

```sh
$ build/mini-elfuse -v build/mmap > trace.txt 2>&1
$ cat trace.txt
...
syscall 222(0x0, 0x110000, 0x3, 0x22, 0xffffffffffffffff, 0x0)
  -> 268435456 (0x10000000)
syscall 215(0x10000000, 0x110000, 0x4906c0, 0x490000, 0x2000000, 0x0)
  -> 0 (0x0)
syscall 80(0x1, 0x7fff5f0, 0x7fff5f0, 0x46ac80, 0xffffffff, 0x490270)
  -> 0 (0x0)
syscall 56(0xffffffffffffff9c, 0x7ffffe5, 0x0, 0x0, 0x7ffffe5, 0x490270)
  -> 3 (0x3)
syscall 222(0x0, 0x4, 0x1, 0x2, 0x3, 0x0)
  -> 269549568 (0x10110000)
syscall 64(0x1, 0x495500, 0x12, 0x1, 0x5e8, 0x12)
sum 133693440
ELF
  -> 18 (0x12)
syscall 94(0x0, 0x0, 0x30, 0x494700, 0x5e8, 0x12)
```

- `malloc` asks for `0x110000` bytes with `prot` 3 (`PROT_READ | PROT_WRITE`)
  and flags `0x22` (`MAP_PRIVATE | MAP_ANONYMOUS`): the megabyte plus glibc's
  own bookkeeping, rounded to the 64 KiB pages glibc assumes (part 5). It gets
  `0x10000000`, and `free` returns it with `munmap` (215).
- The file mapping asks for 4 bytes of descriptor 3 with `PROT_READ` and
  `MAP_PRIVATE`. It gets `0x10110000`, right after the first mapping, whose
  range is not reused.

[Part 8](../08-threads/) runs a program with threads.

## The code

The files are served with this site under `mini-elfuse/step-7/`:

```sh
mkdir step-7 && cd step-7
for f in Makefile mini-elfuse.c vectors.S entitlements.plist hello.S null.S printf.c files.c mmap.c; do
    curl -fsSO https://henrybear327.github.io/notes/mini-elfuse/step-7/$f
done
make check
```

`Makefile`:

{{< include-file "static/mini-elfuse/step-7/Makefile" "make" >}}

`mini-elfuse.c`:

{{< include-file "static/mini-elfuse/step-7/mini-elfuse.c" "c" >}}

`vectors.S`:

{{< include-file "static/mini-elfuse/step-7/vectors.S" "asm" >}}

`entitlements.plist`:

{{< include-file "static/mini-elfuse/step-7/entitlements.plist" "xml" >}}

`hello.S`:

{{< include-file "static/mini-elfuse/step-7/hello.S" "asm" >}}

`null.S`:

{{< include-file "static/mini-elfuse/step-7/null.S" "asm" >}}

`printf.c`:

{{< include-file "static/mini-elfuse/step-7/printf.c" "c" >}}

`files.c`:

{{< include-file "static/mini-elfuse/step-7/files.c" "c" >}}

`mmap.c`:

{{< include-file "static/mini-elfuse/step-7/mmap.c" "c" >}}

## What we learned

- With all program memory mapped in advance, `mmap` only has to choose
  addresses; a counter that never goes back keeps new memory zero.
- A file mapping can be a copy made with `pread`, which matches Linux for
  private mappings and not for shared ones.
- `munmap` and `mprotect` can succeed without doing anything when memory is
  never reused and every page allows everything.
