---
title: 'mini-elfuse 3: Page tables'
date: 2026-09-15T12:00:00+02:00
series: ["mini-elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "mini-elfuse", "linux", "macos", "memory"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 3 of a series that builds mini-elfuse, a small version of
[elfuse](https://github.com/sysprog21/elfuse).
[Part 2](../02-a-vm-and-the-first-system-call/) ran a program to its first
system call with address translation off. This part writes page tables and
turns translation on.

## The problem

With translation off, the program can read and write every byte of the slab:
address 0, the exception vectors at `0x200000`, and anything else mini-elfuse
keeps there. A Linux program also expects a read of address 0 to fail, so
that a null pointer bug stops the program where it happens
([elfuse part 6](../../elfuse/06-guest-memory-and-page-tables/)).

A second test program, `null.S`, reads address 0 and then calls `exit`:

```asm
/* Read address 0 into x0, then exit */
    .text
    .globl _start
_start:
    mov x0, #0
    ldr x0, [x0]
    mov x8, #93         /* exit */
    svc #0
```

Under the part 2 code, the read succeeds and the program reaches its `exit`:

```sh
$ ../step-2/build/mini-elfuse build/null
mini-elfuse: unhandled syscall 93 at 0x4000e0
```

The `Makefile` builds each guest program from its `.S` file with one pattern
rule, so `null` only needs adding to `all` and to `check`.

## Four levels, three tables

[elfuse part 2](../../elfuse/02-what-a-virtual-machine-is/) introduced page
tables: a tree the MMU (memory management unit) walks to translate an address
and check its permissions. With 4 KiB pages and 48-bit addresses, the tree has
four levels. An entry at level 0 covers 512 GiB, at level 1 1 GiB, at level 2
2 MiB, and at level 3 4 KiB. An entry at level 1 or 2 can point to a table at
the next level or map its whole range directly as a block.

mini-elfuse maps its 1 GiB with 2 MiB blocks, so it needs one table at each of
levels 0, 1, and 2, each a 4 KiB page of 512 entries. They go at `0x1000`,
`0x2000`, and `0x3000`, below program memory:

```c
/* Level 0, 1 and 2 tables at 0x1000, 0x2000 and 0x3000. L2[0] stays empty,
 * so addresses below 2 MiB fault.
 */
static void build_page_tables(void)
{
    uint64_t *l0 = (uint64_t *) (slab + 0x1000);
    uint64_t *l1 = (uint64_t *) (slab + 0x2000);
    uint64_t *l2 = (uint64_t *) (slab + 0x3000);

    l0[0] = 0x2000 | PTE_TABLE;
    l1[0] = 0x3000 | PTE_TABLE;
    /* EL1 cannot execute memory EL0 can write, so the vectors get their own
     * block, which EL0 cannot read or write.
     */
    l2[1] = VECTORS_BASE | PTE_BLOCK | PTE_AF;
    for (uint64_t i = 2; i < 512; i++)
        l2[i] = i << 21 | PTE_BLOCK | PTE_AF | PTE_AP_EL0_RW;
}
```

`main()` calls it once, after copying the vectors. Each entry is the address
it points to or maps, plus flag bits:

```c
/* Page table entries */
#define PTE_BLOCK 0x1ULL          /* maps 2 MiB at level 2 */
#define PTE_TABLE 0x3ULL          /* points to the next level */
#define PTE_AP_EL0_RW (1ULL << 6) /* set: EL0 may read and write */
#define PTE_AF (1ULL << 10)       /* access flag; clear means fault on use */
```

| Bits | Meaning |
|---|---|
| 1:0 | `01` block, `11` table, `00` no mapping |
| 4:2 | which `MAIR_EL1` attribute applies; 0 here |
| 7:6 | access permissions: `01` EL0 and EL1 read-write, `00` EL1 read-write only |
| 10 | access flag; the MMU faults on an entry without it |

The layout that results:

| Range | Entry | Access |
|---|---|---|
| `0x0` to `0x1fffff` | `L2[0]`, empty | none; page tables live here, out of the program's reach |
| `0x200000` to `0x3fffff` | `L2[1]` | no EL0 read or write; the vectors |
| `0x400000` to `0x3fffffff` | `L2[2]` to `L2[511]` | EL0 read, write, and execute |

Bits 53 and 54 would forbid execution at EL1 and EL0; mini-elfuse leaves them
clear. elfuse instead gives each segment its own permissions and splits blocks
into 4 KiB pages where they differ
([elfuse part 6](../../elfuse/06-guest-memory-and-page-tables/)).

The vectors need a block of their own because of an Arm rule: code at EL1
cannot execute memory that EL0 can write. With `PTE_AP_EL0_RW` on `L2[1]`, the
first `svc` makes the processor fault while fetching the vector entry, and the
fault's own vector entry faults the same way: the vCPU never exits and
mini-elfuse hangs. Without `PTE_AF` on the blocks it hangs too.

## Turning the MMU on

`boot_vcpu()` sets four more registers:

```c
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, 0x1000));
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1, TCR_T0SZ_48));
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, 0xff)); /* attr 0: RAM */
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, SCTLR_RES1 | SCTLR_M));
```

- `TTBR0_EL1` is the address of the level 0 table.
- `TCR_EL1` sets the size of the tree. Its low 6 bits, `T0SZ`, are 64 minus
  the address width, so 16 means 48-bit addresses. `TG0`, bits 15:14, is left
  at 0, which selects 4 KiB pages.
- `MAIR_EL1` holds eight memory attributes, one byte each. Attribute 0,
  the one bits 4:2 select, is `0xff`: ordinary memory. A value of 0 would mean
  device memory.
- `SCTLR_EL1` bit 0, `M`, turns the MMU on. `SCTLR_RES1` holds eight bits that
  the Arm architecture reserves as 1 when the feature each one controls is
  absent. The M4 has four of those features, and 1 selects their older
  behavior. elfuse sets the same bits because Hypervisor.framework starts the
  register at 0.

```c
#define TCR_T0SZ_48 16ULL /* 48-bit addresses, 4 KiB granule: walk from L0 */
#define SCTLR_RES1 0x30d00980ULL
#define SCTLR_M 0x1ULL /* MMU on */
```

elfuse does not set `M` before its first vCPU runs. Its shim asks for it with
`hvc #4` from inside the guest, and the comment there says setting it earlier
causes a fault on the first instruction
([elfuse part 5](../../elfuse/05-booting-a-vm-with-no-kernel/)). On this Mac,
setting it from `boot_vcpu()` works. elfuse's later threads skip `hvc #4` too:
each new vCPU gets the parent's registers and starts at EL0
([elfuse part 10](../../elfuse/10-threads-futexes-and-the-clock/)).

## Running it

`hello` runs as before, and `null` now stops at its read:

```sh
$ build/mini-elfuse build/hello
mini-elfuse: unhandled syscall 64 at 0x4000e4
$ build/mini-elfuse build/null
mini-elfuse: exception: ESR_EL1 0x92000006, FAR_EL1 0x0, ELR_EL1 0x4000d8
```

`ESR_EL1` `0x92000006` decodes as:

| Bits | Value | Meaning |
|---|---|---|
| 31:26 | `0x24` | data abort from EL0 |
| 6 | 0 | a read |
| 5:0 | `0x06` | translation fault at level 2 |

The fault is at level 2 because `L2[0]` is empty. `FAR_EL1` is the address
read, 0, and `ELR_EL1` is the `ldr` instruction:

```sh
$ aarch64-linux-gnu-objdump -d build/null
...
  4000d8:	f9400000 	ldr	x0, [x0]
...
```

[Part 4](../04-write-and-exit/) handles `hello`'s system calls.

## The code

The files are served with this site under `mini-elfuse/step-3/`:

```sh
mkdir step-3 && cd step-3
for f in Makefile mini-elfuse.c vectors.S entitlements.plist hello.S null.S; do
    curl -fsSO https://henrybear327.github.io/notes/mini-elfuse/step-3/$f
done
make check
```

`Makefile`:

{{< include-file "static/mini-elfuse/step-3/Makefile" "make" >}}

`mini-elfuse.c`:

{{< include-file "static/mini-elfuse/step-3/mini-elfuse.c" "c" >}}

`vectors.S`:

{{< include-file "static/mini-elfuse/step-3/vectors.S" "asm" >}}

`entitlements.plist`:

{{< include-file "static/mini-elfuse/step-3/entitlements.plist" "xml" >}}

`hello.S`:

{{< include-file "static/mini-elfuse/step-3/hello.S" "asm" >}}

`null.S`:

{{< include-file "static/mini-elfuse/step-3/null.S" "asm" >}}

## What we learned

- Three 4 KiB tables map the 1 GiB slab in 2 MiB blocks, except the first
  block.
- Leaving the first level 2 entry empty makes addresses below 2 MiB fault,
  which catches null pointers and keeps the page tables out of the program's
  reach.
- EL1 cannot execute memory EL0 can write, so the vectors sit in a block EL0
  cannot read or write.
- The MMU is on once `TTBR0_EL1`, `TCR_EL1`, `MAIR_EL1`, and `SCTLR_EL1` are
  set; on this Mac they can all be set before the vCPU first runs.
