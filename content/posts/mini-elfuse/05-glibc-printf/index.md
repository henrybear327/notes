---
title: "mini-elfuse 5: glibc's printf"
date: 2026-09-15T14:00:00+02:00
series: ["mini-elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "mini-elfuse", "linux", "macos", "glibc"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 5 of a series that builds mini-elfuse, a small version of
[elfuse](https://github.com/sysprog21/elfuse).
[Part 4](../04-write-and-exit/) ran an assembly program that makes two system
calls. This part runs a C program that calls `printf`, built against glibc.

## The problem

`printf.c` is the usual first C program:

```c
#include <stdio.h>

int main(void)
{
    printf("hello, world\n");
    return 0;
}
```

The `Makefile` builds C files with the cross compiler and `-static`, which
copies the parts of glibc the program uses into the file:

```make
$(BUILD)/%: %.c | $(BUILD)
	$(CROSS)gcc -static -O2 -o $@ $<
```

The result is still one `ET_EXEC` file that part 1's loader accepts, now with
two `PT_LOAD` segments, code and data:

```sh
$ aarch64-linux-gnu-readelf -h -l build/printf
...
  Entry point address:               0x400608
...
  LOAD           0x0000000000000000 0x0000000000400000 0x0000000000400000
                 0x000000000007437c 0x000000000007437c  R E    0x10000
  LOAD           0x000000000007f880 0x000000000048f880 0x000000000048f880
                 0x00000000000021c8 0x0000000000003858  RW     0x10000
...
```

The entry point is glibc's `_start`, not `main`. Before `main` runs, glibc
reads its arguments from the stack, sets up thread-local storage and the heap,
and checks the kernel. Each of those expects something from the kernel that
parts 1 to 4 do not provide. This part runs the program, reads where it
stops, and adds what was missing, one failure at a time.

## The failures

In order, with the part 4 code as the starting point:

| Missing | Where the program stops | Added |
|---|---|---|
| the initial stack | `_start`, reading `argc` at `sp` | `build_stack()` |
| `brk` | `__sbrk`, writing `errno` | `brk` |
| `AT_RANDOM` | `_dl_setup_stack_chk_guard`, reading a null pointer | an auxv entry |
| `SCTLR_EL1.C` | `__aarch64_cas4_acq`, at `ldaxr` | a bit in `SCTLR_EL1` |
| `CPACR_EL1` | `__memset_generic`, at a SIMD instruction | `CPACR_EL1` |
| `writev` | `abort`, at `brk #1000`, with its message lost | `writev` |
| `uname` | `abort`, at `brk #1000`, after printing its message | `uname` |

`aarch64-linux-gnu-addr2line -f -e build/printf <address>` turned each
`ELR_EL1` value below into a function name.

## The initial stack

With part 4's code:

```sh
$ ../step-4/build/mini-elfuse build/printf
mini-elfuse: exception: ESR_EL1 0x92000006, FAR_EL1 0x0, ELR_EL1 0x400614
```

`0x400614` is the fourth instruction of `_start`:

```sh
$ aarch64-linux-gnu-objdump -d --start-address=0x400608 --stop-address=0x400620 build/printf
...
  400608:	d280001d 	mov	x29, #0x0                   	// #0
  40060c:	d280001e 	mov	x30, #0x0                   	// #0
  400610:	aa0003e5 	mov	x5, x0
  400614:	f94003e1 	ldr	x1, [sp]
...
```

It reads `argc` at the stack pointer, and `SP_EL0` is still 0.
[elfuse part 4](../../elfuse/04-loading-a-linux-program/) shows the layout
Linux prepares: `argc`, the `argv` pointers and a null pointer, the `envp`
pointers and a null pointer, the auxiliary vector (auxv) of key and value
pairs, and above them the strings the pointers point to. `build_stack()`
writes that layout below `STACK_TOP`, `0x08000000`. The program's `argv` is
mini-elfuse's command line from the program's path on, and the environment is
empty:

```c
/* The stack Linux gives a new program, from SP up: argc, argv[] and NULL,
 * envp[] (empty here) and NULL, auxv pairs up to AT_NULL, then the strings.
 */
static uint64_t build_stack(int argc, char **argv)
{
    uint64_t str = STACK_TOP - 16; /* AT_RANDOM bytes */
    for (int i = 0; i < argc; i++)
        str -= strlen(argv[i]) + 1;
    uint64_t words = 1 + argc + 1 + 1 + 4; /* argc, argv, NULL, NULL, auxv */
    uint64_t sp = (str - 8 * words) & ~15ULL;

    uint64_t *w = (uint64_t *) (slab + sp);
    *w++ = argc;
    for (int i = 0; i < argc; i++) {
        *w++ = str;
        strcpy((char *) slab + str, argv[i]);
        str += strlen(argv[i]) + 1;
    }
    *w++ = 0;
    *w++ = 0;
    /* glibc takes its stack canary from these bytes */
    *w++ = AT_RANDOM;
    *w++ = str;
    arc4random_buf(slab + str, 16);
    *w++ = AT_NULL;
    *w++ = 0;
    return sp;
}
```

The stack pointer is rounded down to a multiple of 16, as the arm64 calling
convention requires. `main()` passes the result to `boot_vcpu()`, which sets
`SP_EL0`:

```c
    boot_vcpu(vcpu, entry, build_stack(argc - 1, argv + 1));
```

The auxv entry for `AT_RANDOM` comes up two sections later. In the first run
with a stack, the auxv held only `AT_NULL`.

## brk

With the stack added and nothing else, `-v` shows (the `load` and `entry`
lines left out):

```sh
syscall 175(0x0, 0x7, 0x400040, 0x400e00, 0x400ec0, 0x0)
  -> -38 (0xffffffffffffffda)
syscall 174(0xffffffffffffffda, 0x7, 0x400040, 0x400e00, 0x400ec0, 0x0)
  -> -38 (0xffffffffffffffda)
syscall 177(0xffffffffffffffda, 0x1, 0x400040, 0x400e00, 0x400ec0, 0x0)
  -> -38 (0xffffffffffffffda)
syscall 176(0xffffffffffffffda, 0x1, 0x400040, 0x400e00, 0x400ec0, 0x0)
  -> -38 (0xffffffffffffffda)
syscall 214(0x0, 0x0, 0xf80, 0x492000, 0x48f8e0, 0x7)
  -> -38 (0xffffffffffffffda)
mini-elfuse: exception: ESR_EL1 0x92000046, FAR_EL1 0x30, ELR_EL1 0x418994
```

The first four calls are `geteuid`, `getuid`, `getegid`, and `getgid`, from
glibc's `__libc_init_secure`. Without `AT_SECURE` or the four id entries in the
auxv, it compares the effective and real ids to decide whether the program runs
with raised privileges. All four calls fail the same way, the comparisons find
them equal, and glibc carries on.

Call 214 is `brk`, which moves the end of the heap, the program's growable
data area. glibc uses it for the memory of its thread-local storage, and on
failure `__sbrk` stores `ENOMEM` in `errno`:

```sh
  41898c:	d53bd041 	mrs	x1, tpidr_el0
  418990:	52800182 	mov	w2, #0xc                   	// #12
  418994:	b8206822 	str	w2, [x1, x0]
```

`errno` is a thread-local variable, found through the thread pointer
`TPIDR_EL0`, and that pointer is still 0 because thread-local storage is what
glibc was setting up. The write goes to address `0x30`, below 2 MiB, and faults
on part 3's empty entry.

`load_elf()` now records where the program ends, and the heap starts at the
next page boundary:

```c
        if (ph.p_vaddr + ph.p_memsz > end)
            end = ph.p_vaddr + ph.p_memsz;
    }
    close(fd);
    if (end > BRK_LIMIT)
        die("%s: too large", path);
    brk_base = brk_cur = (end + 0xfff) & ~0xfffULL;
```

`brk(addr)` asks for the heap to end at `addr` and returns the end it got. A
request outside `[brk_base, BRK_LIMIT)` returns the current end unchanged,
which is how Linux reports failure, and `brk(0)` is how glibc asks where the
heap ends:

```c
    case NR_brk:
        /* A refused request returns the unchanged break. */
        if (a[0] >= brk_base && a[0] < BRK_LIMIT) {
            /* Linux unmaps pages the break gives back; they return zeroed */
            if (a[0] < brk_cur)
                memset(slab + a[0], 0, brk_cur - a[0]);
            brk_cur = a[0];
        }
        ret = brk_cur;
        break;
```

The memory is already mapped read-write, so `brk` moves `brk_cur` and zeroes
the memory a shrink gives back. `BRK_LIMIT` keeps the heap 8 MiB below the top
of the stack:

```c
#define STACK_TOP 0x08000000ULL
#define BRK_LIMIT (STACK_TOP - (8ULL << 20)) /* 8 MiB for the stack */
```

## AT_RANDOM

With `brk` added, the trace ends:

```sh
syscall 214(0x0, 0x0, 0xf80, 0x492000, 0x48f8e0, 0x7)
  -> 4800512 (0x494000)
syscall 214(0x494f80, 0x494f80, 0xf80, 0x492000, 0x48f8e0, 0x7)
  -> 4804480 (0x494f80)
mini-elfuse: exception: ESR_EL1 0x92000006, FAR_EL1 0x0, ELR_EL1 0x400940
```

`brk` hands out `0xf80` bytes at `0x494000`. The next stop is in
`_dl_setup_stack_chk_guard`, which makes the stack canary: the value that
functions place on the stack and check before returning, to detect an
overwritten return address. glibc copies it from the random bytes that the auxv
entry `AT_RANDOM` points to, and without the entry the pointer is 0.
[elfuse part 4](../../elfuse/04-loading-a-linux-program/) lists the entries
elfuse passes; this program needs only this one:

```c
    /* glibc takes its stack canary from these bytes */
    *w++ = AT_RANDOM;
    *w++ = str;
    arc4random_buf(slab + str, 16);
```

## SCTLR_EL1.C

With `AT_RANDOM` added, the trace ends:

```sh
syscall 160(0x7fffcb8, 0x48f000, 0x491448, 0x40, 0x48f8a0, 0x494730)
  -> -38 (0xffffffffffffffda)
syscall 56(0xffffffffffffff9c, 0x452d60, 0x0, 0x0, 0x0, 0x452d60)
  -> -38 (0xffffffffffffffda)
syscall 56(0xffffffffffffff9c, 0x451388, 0x902, 0x0, 0x902, 0x451388)
  -> -38 (0xffffffffffffffda)
syscall 66(0x2, 0x7fffd50, 0x1, 0x27, 0x2, 0x20)
  -> -38 (0xffffffffffffffda)
syscall 222(0x0, 0x10000, 0x3, 0x22, 0xffffffffffffffff, 0x0)
  -> -38 (0xffffffffffffffda)
mini-elfuse: exception: ESR_EL1 0x92000035, FAR_EL1 0x491fb8, ELR_EL1 0x44b008
```

Several calls fail here. The `writev` and `uname` sections below explain all
but `mmap` (222), with which glibc tries to keep a copy of its error message.
The fault is in `__aarch64_cas4_acq`, a compare-and-swap helper from libgcc
that glibc's locks call:

```sh
  44b004:	2a0003f0 	mov	w16, w0
  44b008:	885ffc40 	ldaxr	w0, [x2]
```

`ldaxr` is an exclusive load, the first half of an atomic update. Fault status
`0x35` in the low bits of `ESR_EL1` is Arm's code for an unsupported exclusive
or atomic access. Setting bit 2 of `SCTLR_EL1`, `C`, removes the fault. With
`C` clear, the architecture treats all data memory as non-cacheable, and on this
Mac an exclusive load from it takes this fault. The finished program needs the
bit too: without it, it faults at the same instruction on a different
address.

```c
#define SCTLR_C 0x4ULL          /* data cache; without it ldaxr/stxr fault */
```

## CPACR_EL1

With `C` set, the run stops elsewhere:

```sh
mini-elfuse: exception: ESR_EL1 0x1fe00000, FAR_EL1 0x0, ELR_EL1 0x416a40
```

Exception class `0x07` is a trapped floating-point or SIMD instruction.
`__memset_generic` starts with `dup v0.16b, w1`, a SIMD instruction, and a
new vCPU traps those because `CPACR_EL1` is 0. Its `FPEN` field, bits 21:20,
set to `11` lets EL0 and EL1 use them. `boot_vcpu()` now sets both registers:

```c
#define CPACR_FPEN (3ULL << 20) /* FP and SIMD at EL0 and EL1 */
```

```c
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1,
                           SCTLR_RES1 | SCTLR_M | SCTLR_C));
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, CPACR_FPEN));
```

## writev

With `CPACR_EL1` set, the trace ends:

```sh
syscall 135(0x2, 0x7fffb00, 0x0, 0x8, 0xffffffff, 0x0)
  -> -38 (0xffffffffffffffda)
mini-elfuse: exception: ESR_EL1 0xf20003e8, FAR_EL1 0x0, ELR_EL1 0x400410
```

Exception class `0x3c` is a `brk` instruction, here `brk #1000`, which glibc's
`abort` runs after raising `SIGABRT` fails. Earlier in the trace, glibc tried
to print why: it opened `/dev/tty` (call 56 with flags `0x902`), then called
`writev` (66) on descriptor 2, and both failed. `writev` writes several buffers
in one call; mini-elfuse writes them one after another:

```c
    case NR_writev: {
        /* struct iovec { void *iov_base; size_t iov_len; }, at most 1024 */
        if (a[2] > 1024) {
            ret = -LINUX_EINVAL;
            break;
        }
        uint64_t *iov = guest_ptr(a[1], 16 * a[2]);
        ret = iov ? 0 : -LINUX_EFAULT;
        for (uint64_t i = 0; iov && i < a[2]; i++) {
            uint64_t len = iov[2 * i + 1];
            void *buf = guest_ptr(iov[2 * i], len);
            int64_t n = buf ? host_ret(write(a[0], buf, len)) : -LINUX_EFAULT;
            if (n < 0) {
                ret = ret ? ret : n; /* bytes already written win */
                break;
            }
            ret += n;
            if ((uint64_t) n < len)
                break;
        }
        break;
    }
```

A short write stops the loop, so the program sees how far the output got, as
it would from Linux.

## uname

With `writev`, the message appears:

```sh
syscall 66(0x2, 0x7fffd50, 0x1, 0x27, 0x2, 0x20)
FATAL: cannot determine kernel version
  -> 39 (0x27)
```

glibc 2.28 checks that the kernel is new enough before `main`. It asks with
`uname` (160), which failed in the trace in the `SCTLR_EL1.C` section, then
tries to open `/proc/sys/kernel/osrelease` (the first call 56), and gives up
when both fail. `struct utsname` is six strings of 65 bytes, and only the third,
the kernel release, matters here:

```c
    case NR_uname: {
        /* struct utsname: six 65-byte strings; release is the third. glibc
         * 2.28 refuses to start unless it can read a kernel version.
         */
        char *uts = guest_ptr(a[0], 6 * 65);
        if (uts) {
            memset(uts, 0, 6 * 65);
            strcpy(uts + 2 * 65, "6.1.0");
        }
        ret = uts ? 0 : -LINUX_EFAULT;
        break;
    }
```

## Running it

```sh
$ build/mini-elfuse build/printf
hello, world
```

The whole trace, without the `load` and `entry` lines:

```sh
$ build/mini-elfuse -v build/printf
...
syscall 175(0x0, 0x7, 0x400040, 0x456ef0, 0x7fffff0, 0x0)
  -> -38 (0xffffffffffffffda)
syscall 174(0xffffffffffffffda, 0x7, 0x400040, 0x456ef0, 0x7fffff0, 0x0)
  -> -38 (0xffffffffffffffda)
syscall 177(0xffffffffffffffda, 0x1, 0x400040, 0x456ef0, 0x7fffff0, 0x0)
  -> -38 (0xffffffffffffffda)
syscall 176(0xffffffffffffffda, 0x1, 0x400040, 0x456ef0, 0x7fffff0, 0x0)
  -> -38 (0xffffffffffffffda)
syscall 214(0x0, 0x0, 0xf80, 0x492000, 0x48f8e0, 0x7)
  -> 4800512 (0x494000)
syscall 214(0x494f80, 0x494f80, 0xf80, 0x492000, 0x48f8e0, 0x7)
  -> 4804480 (0x494f80)
syscall 160(0x7fffcb8, 0x48f000, 0x491448, 0x40, 0x48f8a0, 0x494730)
  -> 0 (0x0)
syscall 78(0xffffffffffffff9c, 0x454958, 0x7ffedb0, 0x1000, 0x7fffd3f, 0xd0)
  -> -38 (0xffffffffffffffda)
syscall 214(0x4c4f80, 0x4c4f80, 0x30000, 0x492000, 0x4909e0, 0xa)
  -> 5001088 (0x4c4f80)
syscall 214(0x4d0000, 0x4d0000, 0xb080, 0x492000, 0x0, 0x1)
  -> 5046272 (0x4d0000)
syscall 80(0x1, 0x7fffcd0, 0x7fffcd0, 0x46a5c8, 0xffffffff, 0x490270)
  -> -38 (0xffffffffffffffda)
syscall 64(0x1, 0x495500, 0xd, 0x1, 0x5e8, 0xd)
hello, world
  -> 13 (0xd)
syscall 94(0x0, 0x0, 0x30, 0x494700, 0x5e8, 0xd)
```

Besides the id calls, two calls still fail without harm. `readlinkat` (78) asks
for the path of `/proc/self/exe`. `fstat` (80) on standard output fails, glibc
treats the output as not a terminal, and it writes the buffered text with one
`write` at exit. The heap end moves to `0x4d0000`, a multiple of 64 KiB: with no
`AT_PAGESZ` in the auxv, glibc 2.28 on aarch64 assumes 64 KiB pages. With an
`AT_PAGESZ` of 4096 added, the same run ends the heap at `0x4b6000` instead.
Part 6 implements `fstat`.

## The code

The files are served with this site under `mini-elfuse/step-5/`:

```sh
mkdir step-5 && cd step-5
for f in Makefile mini-elfuse.c vectors.S entitlements.plist hello.S null.S printf.c; do
    curl -fsSO https://henrybear327.github.io/notes/mini-elfuse/step-5/$f
done
make check
```

`Makefile`:

{{< include-file "static/mini-elfuse/step-5/Makefile" "make" >}}

`mini-elfuse.c`:

{{< include-file "static/mini-elfuse/step-5/mini-elfuse.c" "c" >}}

`vectors.S`:

{{< include-file "static/mini-elfuse/step-5/vectors.S" "asm" >}}

`entitlements.plist`:

{{< include-file "static/mini-elfuse/step-5/entitlements.plist" "xml" >}}

`hello.S`:

{{< include-file "static/mini-elfuse/step-5/hello.S" "asm" >}}

`null.S`:

{{< include-file "static/mini-elfuse/step-5/null.S" "asm" >}}

`printf.c`:

{{< include-file "static/mini-elfuse/step-5/printf.c" "c" >}}

## What we learned

- glibc runs startup code before `main` that reads the initial stack, needs
  `brk` for thread-local storage, and takes its stack canary from `AT_RANDOM`.
- It also needs two processor settings: `SCTLR_EL1.C`, without which its
  atomic updates fault, and `CPACR_EL1.FPEN`, without which its SIMD string
  functions trap.
- glibc 2.28 refuses to start without a kernel version from `uname` or
  `/proc`, and it reports fatal errors through `writev`.
- Several calls, such as the id calls, `readlinkat`, and `fstat`, can fail
  with `ENOSYS` without stopping the program.
