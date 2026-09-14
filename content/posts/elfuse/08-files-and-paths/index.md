---
title: 'elfuse 8: Files and paths'
date: 2026-09-09T17:00:00+02:00
series: ["elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "linux", "macos", "filesystems"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 8 of a series on [elfuse](https://github.com/sysprog21/elfuse).
[Part 7](../07-dispatching-a-system-call/) followed a system call to its
handler. This part is about the calls that take a path, such as `open`, `stat`,
and `unlink`: where the file they name lives, and how Mac file names differ
from Linux ones.

All elfuse code and output below come from commit
[`73d8246b`](https://github.com/sysprog21/elfuse/tree/73d8246b), built on an
Apple M4 Mac. The sysroot is the Alpine Linux fixture from part 4.

## The problem

A Linux program that opens `/etc/os-release` wants the file describing its own
Linux system. The Mac has no such file, and the Mac's `/` is not the guest's
`/` anyway. A Linux program also treats a file name as a string of bytes, so
`Foo` and `foo` are two files, while a Mac's default file system treats them
as one. And some paths a Linux program reads, such as `/proc/self/status`, do
not exist on any disk: the Linux kernel generates their contents.

Every syscall that takes a path has to handle all three.

## Two roots

With `--sysroot`, a guest path can mean a file in the sysroot or a file on the
Mac. Alpine's `/etc/os-release` comes from the sysroot:

```sh
$ build/elfuse --sysroot externals/test-fixtures/rootfs /bin/busybox head -2 /etc/os-release
NAME="Alpine Linux"
ID=alpine
```

The sysroot has no `/Users`, so that path falls through to the Mac:

```sh
$ build/elfuse --sysroot externals/test-fixtures/rootfs /bin/busybox ls -1 /Users
Shared
henrybear327
```

(The sysroot warning from part 4 is left out of this post's output.)
`docs/internals.md` states the rule
([Path Resolution](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#path-resolution)):

> Does the sysroot claim this path? A path it holds resolves there; one it does
> not falls back to the host filesystem, except for the guest system
> directories and the temp roots, which resolve in the sysroot whether or not
> it holds them.

The guest system directories are `/usr`, `/bin`, `/sbin`, `/lib`, `/lib64`,
`/run`, `/opt`, `/boot`, `/srv`, `/root`, `/home`, and all of `/var` and `/etc`,
with a few exceptions
([proc-state.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/proc-state.c#L759-L795)).
A program that looks for `/usr/lib/libfoo.so` and does not find it in the
sysroot must not get the Mac's `/usr/lib` instead. The source gives the reason
([proc-state.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/proc-state.c#L1203-L1204)):
"Prevent escaping guest system paths to macOS host paths, which leads to host
contamination and permission failures (e.g. SIP/EPERM)." SIP, System Integrity
Protection, is the macOS feature that makes system directories read-only.

Two files in `/etc` are exceptions: `/etc/resolv.conf` and `/etc/hosts`, the
files that configure name lookup, fall back to the Mac when the sysroot lacks
them. The temp roots, `/tmp` and `/var/tmp`, always resolve in the sysroot.

All of this is reached through one function, `path_translate_at()` in
`src/syscall/path.c`
([path.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/path.c#L492-L495)).
Every syscall that takes a path calls it, so `open`, `stat`, and `unlink` on
the same name always resolve to the same file.

## `..` at the root

On Linux, `..` in the root directory is the root itself: no number of `..`
goes above `/`. The sysroot is an ordinary directory on the Mac, so without
care a guest path like `/../../etc` would resolve outside it. elfuse clamps it:

```sh
$ build/elfuse --sysroot externals/test-fixtures/rootfs /bin/busybox cat /../../etc/alpine-release
3.21.7
```

`clamp_dotdot_at_guest_root()` removes only the `..` components that would
leave the guest root. A `..` that does not leave the root is kept, and three
rows of the docs table show the difference
([Clamping `..` At The Guest Root](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#clamping--at-the-guest-root)):

| guest path | host spelling below the sysroot | why |
|---|---|---|
| `/../etc/hosts` | `/etc/hosts` | the escaping `..` is dropped, the rest is untouched |
| `/a/../b` | `/a/../b` | interior, so the host resolves it |
| `/a/../../etc/x` | `/a/../etc/x` | one `..` is interior, the second escapes |

The interior `..` has to stay. On Linux, `/absent/../b` fails with `ENOENT`
because `absent` does not exist, and `/file/../b` fails with `ENOTDIR` because
`file` is not a directory. Removing `absent/..` or `file/..` before the lookup
would make both succeed whenever `/b` exists.

## Case-insensitive names

The default file system on a Mac, APFS, is case-insensitive: it treats `Foo`,
`foo`, and `FOO` as the same name, a behavior called case folding. It also
treats the two Unicode spellings of a letter like `é`, one code point or `e`
plus a combining accent, as the same name. Linux file systems such as ext4
compare bytes by default. `docs/filenames.md` shows the difference
([The problem](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/filenames.md#the-problem)):

```text
Linux ext4          compares bytes         "Foo" != "foo" != "FOO"   three files
macOS APFS          case- and              "Foo" == "foo" == "FOO"   one file
(default volume)    normalization-blind
```

The Linux kernel source tree contains files whose names differ only in case,
such as `xt_CONNMARK.h` and `xt_connmark.h`. On a case-insensitive volume elfuse
refuses to run if the sysroot holds `net/netfilter/xt_connmark.h` in either
spelling
([sysroot.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/core/sysroot.c#L27-L28)).
Under elfuse the guest gets Linux behavior:

```sh
$ build/elfuse --sysroot externals/test-fixtures/rootfs /bin/busybox sh -c \
    'echo upper > /tmp/Foo; echo lower > /tmp/foo; ls -1 /tmp; cat /tmp/Foo /tmp/foo'
Foo
foo
hello2.sh
upper
lower
```

(`hello2.sh` is the script from part 4.) On the Mac, the sysroot's `/tmp`
holds this:

```sh
$ ls -1a externals/test-fixtures/rootfs/tmp
.
..
.ef=466f6f
foo
hello2.sh
```

`Foo` is stored as `.ef=466f6f`: the prefix `.ef=` followed by the name's bytes
in hexadecimal, `46 6f 6f`. The rule
([Which names are stored as themselves](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/filenames.md#which-names-are-stored-as-themselves))
is that a name is stored as itself only when it has no uppercase ASCII letter,
no byte above `0x7F`, and does not already look like an escape. Every other
name elfuse creates is escaped, even when nothing collides with it. The docs
explain why that restriction is enough: "lowercase
ASCII is a **fixed point of every transformation the volume applies**". Two
names stored as themselves can never fold into each other, and escaped names are
spelled in lowercase hex (or CJK Unified Ideographs for long names), which the
volume does not fold either.

Because the spelling elfuse gives a new name depends only on the guest name,
never on what else is in the directory, two processes can create colliding
names at the same time without any locking. Lookups go one path component at a
time and ask the volume for each name as it is actually stored, comparing
bytes, because a plain `stat` on a case-insensitive volume would report success
for a wrong-case name that Linux must reject
([Resolving a path](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/filenames.md#resolving-a-path)).
Directory listings decode escaped names on the way out, which is why `ls` in the
guest printed `Foo`.

elfuse applies these naming rules only inside the sysroot; a path that falls
through to the Mac follows the Mac's rules. That is why the temp roots stay in
the sysroot: a build that creates temporary files differing only in case gets
Linux behavior. A sysroot on a case-sensitive volume, which `--create-sysroot`
makes, needs no escaping for case, and elfuse turns it off there.

## Generated files in `/proc`

`/proc` is procfs, a file system the Linux kernel generates: each file's
contents describe the running system at the moment it is read. elfuse has no
Linux kernel to ask, so it generates the files itself from its own state:

```sh
$ build/elfuse --sysroot externals/test-fixtures/rootfs /bin/busybox head -6 /proc/self/status
Name:	busybox
State:	R (running)
Tgid:	1
Pid:	1
PPid:	0
Uid:	1000	1000	1000	1000
```

The process id is 1 and the user id is 1000, the values elfuse assigns to the
guest, not the Mac's. Before a path open goes to macOS, elfuse calls
`proc_intercept_open()` in `src/runtime/procemu.c`. For a path it generates, it
returns a descriptor for the generated content. For any other path it returns a
marker, and the open continues to the real file
([procemu.h](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/procemu.h#L21-L24)):

```c
/* Sentinel return value: path was not intercepted, caller should fall through
 * to the real syscall.
 */
#define PROC_NOT_INTERCEPTED (-2)
```

`readlink` and `stat` on these paths use the same pattern, and the same
mechanism answers selected `/dev` paths and parts of `/sys`. `/proc/self/maps`
in part 6 came from here.

## Shared memory under `/dev/shm`

Linux programs share memory between processes by creating files in `/dev/shm`,
an in-memory file system. macOS has no `/dev/shm`, so elfuse redirects a name
there to a per-user directory on the Mac, `/tmp/elfuse-shm-<uid>/`
([POSIX Shared Memory](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#posix-shared-memory-devshm)).

That directory is an ordinary host directory, which creates a risk that Linux
does not have. A symlink is a file that holds a path; opening the symlink opens
the file at that path. On Linux a symlink in `/dev/shm` resolves within the
Linux file system; in elfuse's backing directory it would resolve on the Mac's
file system. So every operation on a `/dev/shm` name acts on the name itself and
never follows a symlink
([The Never-Follow Invariant](https://github.com/sysprog21/elfuse/blob/73d8246b/docs/internals.md#the-never-follow-invariant)).
The docs note that `shm_open` does not follow symlinks either: glibc's
`shm_open` adds `O_NOFOLLOW` itself.

## Terminals

A pty (pseudoterminal) is a pair of devices that lets a program such as a
terminal emulator or `sshd` act as the terminal for another program: one side,
the master, is held by the terminal emulator, and the other side, the slave, is
the shell's terminal. Both Linux and
macOS have ptys, with two differences that elfuse handles in
`src/runtime/procemu-pty.c`
([procemu-pty.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/runtime/procemu-pty.c#L44-L61)):

1. On macOS, setting a pty's window size through the master fails until the
   slave has been opened once. Linux programs expect it to work at once. So
   every time the guest opens a master, elfuse opens its slave too and holds it
   open, without giving the guest a descriptor for it.
2. macOS names slaves `/dev/ttysNNN`; Linux names them `/dev/pts/N`. elfuse
   records the macOS name when the master is opened and sends later opens of
   `/dev/pts/N` there.

## FUSE without macFUSE

FUSE (filesystem in user space) lets an ordinary program, the daemon, implement
a file system: the kernel forwards file operations on the mounted directory to
the daemon and returns its answers. Tools like `sshfs` work this way. On a Mac,
FUSE needs a separately installed kernel extension or framework such as
macFUSE.

elfuse implements the kernel's side of FUSE itself, inside its syscall
handlers
([fuse.c](https://github.com/sysprog21/elfuse/blob/73d8246b/src/syscall/fuse.c#L7-L11)):

```c
 * Both ends of the FUSE protocol live inside the guest VM, so a guest libfuse
 * program works with no macFUSE, FUSE-T or FSKit on the host. What a real
 * kernel would put in its /dev/fuse queue this file puts in a per-session one,
 * and the daemon on the other side is an ordinary guest process reading and
 * writing that character device through the usual syscall path.
```

A guest program that opens a file in a FUSE mount gets its request queued. The
daemon, another guest process, reads the request from `/dev/fuse` and writes a
reply, and the waiting program wakes with the answer. If the daemon is gone,
the wait ends with `ENOTCONN`.

## What we learned

- `path_translate_at()` translates every guest path. A path the
  sysroot holds resolves there, a path it lacks falls back to the Mac, and
  Linux system directories and temp directories never fall back, apart from a
  few named exceptions.
- `..` is clamped at the guest root, and only there.
- On a case-insensitive APFS volume, any name elfuse creates with uppercase
  letters or non-ASCII bytes is stored as `.ef=` plus an encoding of its bytes
  (hex up to 125 bytes), so `Foo` and `foo` can coexist. The guest never sees
  the escaped names.
- `/proc`, parts of `/dev` and `/sys`, `/dev/shm`, ptys, and FUSE are all
  provided by elfuse, because there is no Linux kernel to provide them.

[Part 9](../09-signals-and-the-return-path/) is about signals: how elfuse
interrupts a running program to run its handler, and how the program resumes
where it stopped.
