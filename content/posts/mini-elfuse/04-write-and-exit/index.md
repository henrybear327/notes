---
title: 'mini-elfuse 4: write and exit'
date: 2026-09-15T13:00:00+02:00
series: ["mini-elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "mini-elfuse", "linux", "macos", "syscalls"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 4 of a series that builds mini-elfuse, a small version of
[elfuse](https://github.com/sysprog21/elfuse).
[Part 3](../03-page-tables/) turned on the MMU; `hello` still stops at its
first system call. This part carries out that call and the next one, so
`hello` runs to the end.

## The problem

When `hvc #5` exits, the program's registers hold a Linux system call. On
aarch64 Linux, `x8` holds the call number and `x0` to `x5` the arguments, and
the result goes back in `x0`. A failing call returns a negative error number,
such as -38 for `ENOSYS` ("function not implemented").

`hello` makes two calls: `write` (64) and `exit` (93). mini-elfuse has to carry
out each with macOS calls, store the result in `x0`, and resume the program
after its `svc`.

## Dispatching

`do_syscall()` reads the number and all six arguments, then switches on the
number:

```c
static void do_syscall(hv_vcpu_t vcpu)
{
    uint64_t nr = reg(vcpu, HV_REG_X8), a[6];
    for (int i = 0; i < 6; i++)
        a[i] = reg(vcpu, HV_REG_X0 + i);
    vlog("syscall %llu(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)", nr,
         a[0], a[1], a[2], a[3], a[4], a[5]);

    int64_t ret;
    switch (nr) {
    case NR_write: {
        void *buf = guest_ptr(a[1], a[2]);
        ret = buf ? host_ret(write(a[0], buf, a[2])) : -LINUX_EFAULT;
        break;
    }
    case NR_exit:
    case NR_exit_group:
        exit(a[0]);
    default:
        ret = -LINUX_ENOSYS;
    }
    vlog("  -> %lld (0x%llx)", ret, ret);
    HV(hv_vcpu_set_reg(vcpu, HV_REG_X0, ret));
}
```

The numbers and error codes are Linux's, so they get their own names:

```c
/* Linux aarch64 syscall numbers and errno values */
enum { NR_write = 64, NR_exit = 93, NR_exit_group = 94 };
enum { LINUX_EFAULT = 14, LINUX_EINVAL = 22, LINUX_ENOSYS = 38 };
```

- `write(fd, buf, count)`: `guest_ptr()` from part 1 checks that the buffer
  lies in program memory and returns its host address. The program's file
  descriptor 1 is mini-elfuse's own descriptor 1, so the macOS `write` can use
  the number unchanged. elfuse looks up the macOS descriptor behind each
  program descriptor ([elfuse part 3](../../elfuse/03-from-launch-to-exit/));
  mini-elfuse passes descriptors through in every part.
- `exit(status)` ends mini-elfuse with the program's status. `exit_group` is the
  version that ends every thread; with one thread they are the same.
- Every other number returns `-ENOSYS`, the answer a Linux kernel gives for a
  call it does not have. Part 5 shows glibc continuing after several of them.

`dispatch.tbl` and its generated table do this job in elfuse
([elfuse part 7](../../elfuse/07-dispatching-a-system-call/)); a `switch` is
enough for the handful of calls here.

## Error numbers

A macOS call reports failure with -1 and an error number in `errno`. The
numbers differ between the two systems, as elfuse part 7 describes: 1 to 34
are the same except 11, which is `EDEADLK` on macOS and `EAGAIN` on Linux.
`host_ret()` converts a macOS result into a Linux one:

```c
/* errno values up to 34 are the same on macOS and Linux, except 11 */
static int64_t host_ret(int64_t r)
{
    static const int errnos[][2] = {
        /* macOS, Linux */
        {EAGAIN, 11},    {EDEADLK, 35}, {ENAMETOOLONG, 36},
        {ENOTEMPTY, 39}, {ELOOP, 40},
    };
    if (r >= 0)
        return r;
    for (size_t i = 0; i < sizeof errnos / sizeof errnos[0]; i++)
        if (errno == errnos[i][0])
            return -errnos[i][1];
    return errno <= 34 ? -errno : -LINUX_EINVAL;
}
```

The table holds five of the 42 differences elfuse lists: the two uses of 11,
and three errors that file calls return. Any other number above 34 becomes
`EINVAL`, as in elfuse.

## Back to the program

The run loop calls `do_syscall()` where part 3 called `die()`, and loops:

```c
        do_syscall(vcpu);
    }
}
```

The next `hv_vcpu_run()` continues after the `hvc #5` in the vector entry, so
the vCPU runs `eret`. That returns to EL0 at `ELR_EL1`, the instruction after
the program's `svc`, with `x0` holding the result and every other
general-purpose register as the program left it.

## Running it

```sh
$ build/mini-elfuse build/hello
hello
```

`-v` shows both calls. The general-purpose registers mini-elfuse did not set
still hold the program's values, so the arguments of `exit` show `write`'s
leftover `x1` and `x2`:

```sh
$ build/mini-elfuse -v build/hello
load 0x400000-0x4000fa
entry 0x4000d4
syscall 64(0x1, 0x4000f4, 0x6, 0x0, 0x0, 0x0)
hello
  -> 6 (0x6)
syscall 93(0x0, 0x4000f4, 0x6, 0x0, 0x0, 0x0)
```

`0x4000f4` is `msg`, and `write` returns 6, the number of bytes written. `exit`
prints no result line because it does not return. `make check` now compares the
output with `hello`:

```make
check: all
	test "$$($(BUILD)/mini-elfuse $(BUILD)/hello)" = hello
	$(BUILD)/mini-elfuse $(BUILD)/null 2>&1 | grep 'FAR_EL1 0x0,'
```

[Part 5](../05-glibc-printf/) runs a C program built with glibc, which needs
more than two system calls before it reaches `main`.

## The code

The files are served with this site under `mini-elfuse/step-4/`:

```sh
mkdir step-4 && cd step-4
for f in Makefile mini-elfuse.c vectors.S entitlements.plist hello.S null.S; do
    curl -fsSO https://henrybear327.github.io/notes/mini-elfuse/step-4/$f
done
make check
```

`Makefile`:

{{< include-file "static/mini-elfuse/step-4/Makefile" "make" >}}

`mini-elfuse.c`:

{{< include-file "static/mini-elfuse/step-4/mini-elfuse.c" "c" >}}

`vectors.S`:

{{< include-file "static/mini-elfuse/step-4/vectors.S" "asm" >}}

`entitlements.plist`:

{{< include-file "static/mini-elfuse/step-4/entitlements.plist" "xml" >}}

`hello.S`:

{{< include-file "static/mini-elfuse/step-4/hello.S" "asm" >}}

`null.S`:

{{< include-file "static/mini-elfuse/step-4/null.S" "asm" >}}

## What we learned

- A system call arrives as `x8` and `x0` to `x5`; its result goes back in
  `x0`, negative on failure.
- `write` needs its buffer checked and converted to a host address; its file
  descriptor passes through unchanged.
- Resuming the vCPU runs the vector entry's `eret`, which returns to the
  instruction after the program's `svc`.
- macOS error numbers are converted to Linux ones before they reach the
  program.
