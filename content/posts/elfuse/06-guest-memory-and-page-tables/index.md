---
title: 'elfuse 6: Guest memory and page tables'
date: 2026-09-09T15:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "memory"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 6 of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 5](../05-booting-a-vm-with-no-kernel/) got the program running at EL0.
This part is about the memory it runs in: how elfuse turns one flat slab into
the address space a Linux program expects, and keeps that address space
correct while the program changes it.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac running macOS 26.6.2.

## The problem

A Linux program does more with memory than read and write it. It asks for more
with `mmap`. It changes permissions with `mprotect`, for example to make data
read-only once it has been set up. It maps files so that reading memory reads
the file. It expects a read of address 0 to fail, and it has been told, through
`AT_PAGESZ` in part 4, that memory comes in 4 KiB pages.

elfuse has to meet all of these with one slab and the page tables it writes
itself, on a Mac whose own pages are 16 KiB.

## The address space

A program can read its layout from `/proc/self/maps`, one line per region with
its addresses, permissions, and name. Busybox from part 4, run through elfuse,
with the home directory shortened to `~`:

```sh
$ build/elfuse --sysroot externals/test-fixtures/rootfs /bin/busybox cat /proc/self/maps 2>/dev/null | sed "s#$HOME#~#"
400000-4d2000 r-xp 00000000 00:00 0                                      ~/elfuse-blog.noindex/wt/externals/test-fixtu
4ec000-4f0000 r--p 000dc728 00:00 0                                      ~/elfuse-blog.noindex/wt/externals/test-fixtu
4f0000-4f1000 rw-p 000e0000 00:00 0                                      ~/elfuse-blog.noindex/wt/externals/test-fixtu
1000000-1001000 ---p 00000000 00:00 0 
1001000-1002000 rw-p 00001000 00:00 0                                    [heap]
7800000-7801000 ---p 00000000 00:00 0                                    [stack-guard]
7801000-8000000 rw-p 00000000 00:00 0                                    [stack]
feffdf6000-feffdf8000 r-xp 00000000 00:00 0                              [shim]
feffe00000-ff00000000 ---p 00000000 00:00 0                              [shim-data]
ff00000000-ff000a2000 r-xp 00000000 00:00 0                              externals/test-fixtures/rootfs/lib/ld-musl-aarch64.so.1
ff000bf000-ff000c0000 r--p 000afb00 00:00 0                              externals/test-fixtures/rootfs/lib/ld-musl-aarch64.so.1
ff000c0000-ff000c3000 rw-p 000b0000 00:00 0                              externals/test-fixtures/rootfs/lib/ld-musl-aarch64.so.1
```

The first three names are cut off. elfuse stores each region's name in a
64-byte field
([guest.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.h#L291)),
so a path longer than 63 characters is truncated, which Linux does not do.

Parts 4 and 5 placed each region:

- Busybox is at 4 MiB. Its code is `r-x`, readable and executable, and its data
  is `rw-`. The `r--` range is data the dynamic linker made read-only after
  filling it in.
- The heap starts at 16 MiB. The C library grew that region with `brk` and then
  made its first page inaccessible with `mmap(..., PROT_NONE, ...)`, as a
  guard.
- The 8 MiB stack ends at `0x8000000`, with an inaccessible guard page
  under it, so a stack that grows too far faults instead of overwriting other
  memory.
- The shim and its data are near the top. `[shim-data]` shows as `---p`
  because EL0 cannot access it (part 5).
- The dynamic linker is at `0xff00000000`.

Address 0 has no mapping: elfuse removes it at boot, so reading it faults
([bootstrap.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/bootstrap.c#L138)).
The vDSO page at `0xf000` from part 4 is mapped but not listed here, where
Linux would list it as `[vdso]`. Two regions are missing from this program's
map because it never used them: `mmap` places code-only mappings from
`0x10000000` and all other mappings from 8 GiB. The full layout is in
`docs/internals.md` under [Memory
Layout](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#memory-layout):

![The guest address space from low to high, not to scale: the vDSO page at 0xf000, the program at 0x400000, the brk region from 0x1000000, the 8 MiB stack at 0x7800000, code mappings from 0x10000000, data mappings from 0x200000000, then the 16 MiB runtime reserve (unmapped guard at 0xfeff000000, page-table pool at 0xfeff010000, shim code at 0xfeffdf6000, shim data and EL1 stack at 0xfeffe00000), and the dynamic linker at 0xff00000000.](figure-5.svg)

The 16 MiB below the dynamic linker is elfuse's runtime reserve. Besides the
shim's code and data from part 5, it holds the page-table pool: the pages
elfuse writes the guest's page tables into.

## Blocks and pages

Part 2 described page tables as a tree. elfuse's tree has up to four levels
([guest.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.c#L25-L30)).
An entry at level 0 covers 512 GiB, at level 1 1 GiB, at level 2 2 MiB, and at
level 3 4 KiB. An entry at level 2 can map its whole 2 MiB directly, as a
block, or point to a level-3 table of 512 separate 4 KiB pages.

A block is cheaper: one entry instead of a 4 KiB table of 512. But a block has
one set of permissions. elfuse uses blocks where it can and splits a block into
pages where one range needs more than one permission. Busybox needs that at
4 MiB, where its code and its data share one 2 MiB range:

![The page-table tree for busybox: TTBR0_EL1 points to L0 entry 0, then L1 entry 0, then an L2 table. L2 entry 0 is a table for the vDSO page, entry 2 is a table, and entry 8 starts as a 2 MiB read-write block. Entry 2 leads to an L3 table for 0x400000 to 0x5fffff with 512 entries of 4 KiB: code from 0x400000 to 0x4d2000 is read and execute, data from 0x4ec000 to 0x4f0fff is read-only after start-up except for its last page, and the rest has no mapping.](figure-6.svg)

`-v` shows the split (timestamps removed): `L2[2]` ends in `003`, the mark of a
table, while `L2[8]`, the heap, starts as a block:

```text
DEBUG src/core/bootstrap.c:199: L2[2]=0xfeff019003
DEBUG src/core/bootstrap.c:199: L2[8]=0x60000001000765
```

`guest_split_block()` builds the 512 pages with the block's old permissions,
then elfuse changes the ones that differ. Swapping a block for an equivalent
table changes no translation, so the cached translations in the TLB still give
the right answer, and elfuse issues no flush. The source cites Arm's FEAT_BBM
level 2, which allows such a swap without the usual break-before-make sequence,
and says Apple Silicon implements it
([guest.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.c#L3276-L3282)).

## Changing permissions later

Permissions also change after start-up. Busybox's `r--` range came from a call
the dynamic linker made while the program was already running, visible with
`-v`:

```text
DEBUG src/syscall/syscall.c:2807: syscall 226@0xff0006b0c8(0x4ec000, 0x4000, 0x1, 0x400000, 0x4b1704, 0x400000)
```

Syscall 226 is `mprotect`: make 16 KiB starting at `0x4ec000` read-only
(`0x1`). elfuse rewrites the four page entries. This time the processor may
have the old, writable translations in its TLB, and it would keep using them
until they are dropped.

Dropping cached translations is a TLB invalidation, TLBI for short, and the
instructions that do it cannot run at EL0. So elfuse records what needs
dropping and tells the shim in `x8` on the way back from the syscall (part 5).
There are three sizes of flush, and elfuse picks the smallest that covers the
change
([guest.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.h#L731-L737)):

| `x8` | Flush | Used for |
|---|---|---|
| 3 | one `TLBI VAE1IS` per page | up to 16 pages |
| 4 | one `TLBI RVAE1IS` for a range | up to 64 pages, on processors that support it |
| 1 | `TLBI VMALLE1IS`, everything | anything larger |

A smaller flush keeps more cached translations, and each one kept avoids a
four-level walk later. `mprotect` on 16 KiB touches 4 pages, so it gets the
per-page flush. The comment on the limit of 16 pages gives its reason
([guest.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.h#L318-L324)):
"16 pages == 64 KiB covers RELRO and other typical mprotect / munmap targets."
RELRO is the read-only-after-relocation range from the map above.

Processors also cache decoded instructions. When a change makes a page
executable, old instructions cached for that address must go too. elfuse sets
`x11` for that case, and the shim adds an instruction cache flush (`ic`) to the
TLB flush.

## Writable and executable

Some programs write machine code into memory and then run it. JavaScript engines
do this: a JIT (just-in-time) compiler turns frequently run code into machine
instructions while the program runs. That needs memory that is writable,
executable, or both.

`docs/internals.md` lists this as a Hypervisor.framework rule
([Hypervisor.framework Constraints](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#hypervisorframework-constraints)):
"W^X is enforced even with `SCTLR.WXN=0`. A page-table entry cannot be both
writable and executable." W^X reads "write xor execute". elfuse has a handler
for that rule: when a program writes to an executable page or executes a
writable page it is allowed to, the shim reports the fault with `hvc #9`, and
elfuse flips that one 4 KiB page between read-write and read-execute
([proc.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/proc.c#L3417-L3446)).
If the program was never allowed the access, the same handler delivers a
segmentation fault instead, as Linux would.

The repository also has a probe that asks Hypervisor.framework directly,
`tests/test-rwx.c`. It builds a small VM of its own and runs code from a page
that is writable and executable at once. On the Mac used for this series:

```sh
$ make test-rwx
...
TEST 1: Baseline: RX execution                   PASS
TEST 2: Baseline: RW write                       PASS
TEST 3: RWX 2MiB block (write+exec)
    RWX works! Written code executed successfully (x0=42)
PASS
TEST 4: RWX 4KiB page  (write+exec)
    RWX works! Written code executed (4KiB page, x0=42)
PASS

Results: 4/4 passed

CONCLUSION: Apple HVF allows RWX page table entries at stage-1
when SCTLR_EL1.WXN=0. Self-modifying code works without W^X toggling.
```

elfuse itself does not strip either permission. A request for readable,
writable, and executable memory becomes a page entry with all three
([guest.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.c#L3193-L3211)).
This program writes two instructions into such memory, calls them, then
rewrites and calls them again:

```c
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

int main(void)
{
    unsigned int code[] = {0x52800540, 0xd65f03c0}; /* mov w0, #42; ret */
    unsigned char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int (*fn)(void) = (int (*)(void)) p;

    for (int round = 0; round < 2; round++) {
        code[0] = 0x52800540 | ((unsigned) round << 5); /* 42, then 43 */
        memcpy(p, code, sizeof(code));
        __builtin___clear_cache((char *) p, (char *) p + sizeof(code));
        printf("round %d returned %d\n", round, fn());
    }
    return 0;
}
```

```sh
$ aarch64-linux-gnu-gcc -static -O2 -o jit jit.c
$ build/elfuse ./jit
round 0 returned 42
round 1 returned 43
$ build/elfuse -v ./jit 2>&1 | grep -c 'HVC #9'
0
```

On this machine the flip handler never ran. I have not tested other Mac models
or macOS versions, where the documented rule may still hold and the handler
would run. `mmap` placement also follows the rule: it puts mappings that are
executable and not writable in the separate code region at `0x10000000`, rather
than in a 2 MiB block with writable data
([mem.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/mem.c#L3151-L3159)).

## Demand paging

Part 2 noted that untouched parts of the slab use no physical memory. That is
demand paging, and the comment where elfuse creates the slab warns against
breaking it
([guest.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.c#L508-L510)):
"Do NOT memset because that would touch every page and defeat demand paging."

A related technique is copy-on-write. Two mappings can share the same physical
pages until one of them writes, at which point the kernel gives the writer its
own copy of that page. Part 11 applies the same idea to a file to copy a whole
process without copying its memory.

## Two page sizes

elfuse tells the program its pages are 4 KiB. The Mac's pages are 16 KiB:

```sh
$ pagesize
16384
$ build/elfuse --sysroot externals/test-fixtures/rootfs /usr/bin/getconf PAGESIZE 2>/dev/null
4096
```

Within the slab the difference does not matter, because elfuse's page tables
use 4 KiB pages. It matters when elfuse needs the Mac's kernel to map
something into the slab, and the main case is a file mapping.

A program that calls `mmap` on a file with `MAP_SHARED` expects memory and
file to stay in step: bytes it writes to the memory appear in the file, and
changes to the file appear in memory. The Mac's kernel can do that directly if
elfuse maps the file over the matching part of the slab with the host's own
`mmap`. But the host `mmap` maps whole 16 KiB host pages, so an overlay lines up
with the guest range only when the guest address and the file offset both fall
on host page boundaries
([mem.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/mem.c#L40-L44)):

```c
/* Host kernel page size (16 KiB on Apple Silicon, typically 4 KiB on Intel
 * macOS). MAP_FIXED requires addr/length/offset multiples of this, so an
 * overlay onto a guest 4 KiB-aligned IPA is only applicable when the IPA
 * happens to land on a host page boundary; otherwise sys_mmap falls back to the
 * pread snapshot path.
```

So `mmap` without `MAP_FIXED` or `MAP_FIXED_NOREPLACE` has two strategies
([mem.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/mem.c#L3373-L3376)):

1. Overlay. For a shared mapping of a file descriptor open for writing, whose
   guest address and file offset both fall on 16 KiB boundaries, elfuse maps
   the file over the slab.
   The Mac's kernel keeps memory and file in step.
2. Copy. In every other case elfuse reads the file into the slab with `pread`.
   For a private mapping (`MAP_PRIVATE`), where the program's changes must not
   reach the file, a copy behaves as Linux specifies. For a shared mapping
   that could not be overlaid, elfuse writes the changed bytes back when the
   program calls `msync`.

When elfuse picks the address of a new mapping itself, it starts the search on a
16 KiB boundary and skips past each earlier region to the next one, so a new
mapping never starts inside a host page that an earlier overlay covers. The
address half of the overlay condition then always holds
([mem.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/mem.c#L675-L685)).

## Running out of page tables

Every split takes one 4 KiB page from the page-table pool, and elfuse never
returns it. The pool's comment records the failure that set its size
([guest.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/guest.h#L67-L71)):

> Each split 2MiB block draws one 4KiB L3 page from the pool and the bump
> allocator never reclaims it, so a ~13.9MiB pool (3558 pages, ~7 GiB of split
> address space) hosts the many V8 isolates a Node worker_threads pool / cluster
> spins up; a 960KiB pool exhausted after only ~3 isolates and hard-aborted the
> guest.

V8 is Node.js's JavaScript engine, and each isolate is a separate instance of
it with its own heap and generated code, which means many mixed-permission
blocks. The pool now gets every byte of the 16 MiB reserve outside the unmapped
guard, the 40 KiB slot for the shim's code, and the shim's data block, and
because of demand paging, unused pool pages use no physical memory.

## What we learned

- The guest address space is laid out as Linux programs expect: program at
  4 MiB, heap at 16 MiB, stack below 128 MiB, mappings at `0x10000000` and
  8 GiB, and elfuse's own code and data near the top.
- elfuse maps memory in 2 MiB blocks and splits a block into 4 KiB pages when
  permissions inside it differ. elfuse issues no TLB flush for a split; changing
  permissions does, and elfuse asks the shim for the smallest flush that covers
  the change.
- The docs say Hypervisor.framework forbids writable and executable pages, and
  elfuse has a handler for it. On an M4 with macOS 26.6.2, the repository's own
  probe shows such pages work.
- The program sees 4 KiB pages on a 16 KiB host. Shared mappings of files
  opened for writing get a real host mapping when they land on 16 KiB
  boundaries; other non-fixed mappings get a copy.

[Part 7](../07-dispatching-a-system-call/) follows one system call from the
shim to its handler and back, including the calls the shim answers without an
exit to elfuse.
