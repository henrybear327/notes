---
title: 'mini-elfuse 6: Files'
date: 2026-09-15T15:00:00+02:00
series: ["mini-elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "mini-elfuse", "linux", "macos", "files"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 6 of a series that builds mini-elfuse, a small version of
[elfuse](https://github.com/sysprog21/elfuse).
[Part 5](../05-glibc-printf/) ran a C program that prints to standard output.
This part adds the calls a program needs to create, read, and inspect files.

## The problem

`files.c` writes a line to a file, asks for the file's size with `stat`, and
reads the file back:

```c
#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "note.txt";

    FILE *f = fopen(path, "w");
    if (!f) {
        perror("fopen");
        return 1;
    }
    fputs("written by a Linux program\n", f);
    fclose(f);

    struct stat st;
    if (stat(path, &st) != 0) {
        perror("stat");
        return 1;
    }
    printf("%s: %lld bytes\n", path, (long long) st.st_size);

    char line[64];
    f = fopen(path, "r");
    if (!f) {
        perror("fopen");
        return 1;
    }
    while (fgets(line, sizeof line, f))
        fputs(line, stdout);
    fclose(f);
    return 0;
}
```

With part 5's code, the first `fopen` fails:

```sh
$ ../step-5/build/mini-elfuse build/files note.txt
fopen: Function not implemented
```

glibc opens files with `openat` (56). The program then needs `read` (63),
`close` (57), and `stat`, which glibc 2.28 turns into `newfstatat` (79). glibc
also calls `fstat` (80) on a stream before the stream's first read or write.
`openat`, `newfstatat`, and `fstat` hand mini-elfuse Linux values that mean
something else to macOS: a directory number, flag bits, or a structure.

## openat

`openat(dirfd, path, flags, mode)` opens `path` relative to the directory
`dirfd`. The path is a string in program memory. `guest_str()` checks that it
starts in program memory and has a NUL byte before program memory ends:

```c
/* A NUL-terminated string in program memory, or NULL */
static const char *guest_str(uint64_t addr)
{
    const char *s = guest_ptr(addr, 1);
    return s && memchr(s, 0, GUEST_SIZE - addr) ? s : NULL;
}
```

mini-elfuse passes the path to macOS unchanged, so a program that opens
`/etc/hosts` gets the Mac's `/etc/hosts`. elfuse can look up absolute paths in
a directory of Linux files first, and it handles Mac file system differences
such as case-insensitive names
([elfuse part 8](../../elfuse/08-files-and-paths/)).

`dirfd` is usually `AT_FDCWD`, meaning the current directory. Linux gives it
the value -100 and macOS -2:

```c
#define LINUX_AT_FDCWD -100
```

```c
static int host_dirfd(uint64_t fd)
{
    return fd == (uint64_t) LINUX_AT_FDCWD ? AT_FDCWD : (int) fd;
}
```

If mini-elfuse passes -100 through unchanged, macOS rejects the call and the
program prints:

```sh
fopen: Bad file descriptor
```

The flags differ too. `O_RDONLY`, `O_WRONLY`, and `O_RDWR` are 0, 1, and 2 on
both systems, but the flags below use different bits. Linux's `O_TRUNC`,
`01000`, is the bit macOS uses for `O_CREAT`. A table lists the Linux flags
that `host_open_flags()` converts:

```c
/* Linux open flags with a different value on macOS. O_RDONLY, O_WRONLY and
 * O_RDWR match; other flags are dropped.
 */
static const struct {
    uint64_t linux_flag;
    int host_flag;
} open_flags[] = {
    {0100, O_CREAT},       {0200, O_EXCL},        {01000, O_TRUNC},
    {02000, O_APPEND},     {04000, O_NONBLOCK},   {040000, O_DIRECTORY},
    {0100000, O_NOFOLLOW}, {02000000, O_CLOEXEC},
};

static int host_open_flags(uint64_t flags)
{
    int f = flags & O_ACCMODE;
    for (size_t i = 0; i < sizeof open_flags / sizeof open_flags[0]; i++)
        if (flags & open_flags[i].linux_flag)
            f |= open_flags[i].host_flag;
    return f;
}
```

The Linux values are written in octal, as in the aarch64 Linux headers. With
both conversions, `openat` is one call:

```c
    case NR_openat: {
        const char *path = guest_str(a[1]);
        ret = path ? host_ret(openat(host_dirfd(a[0]), path,
                                     host_open_flags(a[2]), (int) a[3]))
                   : -LINUX_EFAULT;
        break;
    }
```

The file descriptor macOS returns goes back to the program as it is, as in
part 4.

## read and close

Neither needs a conversion:

```c
    case NR_close:
        ret = host_ret(close(a[0]));
        break;
    case NR_read: {
        void *buf = guest_ptr(a[1], a[2]);
        ret = buf ? host_ret(read(a[0], buf, a[2])) : -LINUX_EFAULT;
        break;
    }
```

## stat

`stat` fills a `struct stat`, and the Linux structure has a different layout
from the macOS one: other field sizes, other field order, and 128 bytes in all.
mini-elfuse declares it as the sysroot's `asm-generic/stat.h` does:

```c
/* struct stat on Linux aarch64 */
struct linux_stat {
    uint64_t dev, ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev, pad1;
    int64_t size;
    int32_t blksize, pad2;
    int64_t blocks, atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    uint32_t unused[2];
};
```

`stat_ret()` copies a macOS result into it, field by field. The file type bits
in `mode` have the same values on both systems, so they need no conversion:

```c
static int64_t stat_ret(int r, const struct stat *st, uint64_t addr)
{
    struct linux_stat *ls = guest_ptr(addr, sizeof *ls);
    if (r < 0)
        return host_ret(r);
    if (!ls)
        return -LINUX_EFAULT;
    *ls = (struct linux_stat) {
        .dev = st->st_dev,
        .ino = st->st_ino,
        .mode = st->st_mode,
        .nlink = st->st_nlink,
        .uid = st->st_uid,
        .gid = st->st_gid,
        .rdev = st->st_rdev,
        .size = st->st_size,
        .blksize = st->st_blksize,
        .blocks = st->st_blocks,
        .atime = st->st_atimespec.tv_sec,
        .atime_nsec = st->st_atimespec.tv_nsec,
        .mtime = st->st_mtimespec.tv_sec,
        .mtime_nsec = st->st_mtimespec.tv_nsec,
        .ctime = st->st_ctimespec.tv_sec,
        .ctime_nsec = st->st_ctimespec.tv_nsec,
    };
    return 0;
}
```

`newfstatat` and `fstat` share it:

```c
    case NR_newfstatat: {
        /* The only flag supported is AT_SYMLINK_NOFOLLOW, 0x100 on Linux. */
        const char *path = guest_str(a[1]);
        int flags = a[3] == 0x100 ? AT_SYMLINK_NOFOLLOW : 0;
        if (a[3] & ~0x100ULL)
            ret = -LINUX_EINVAL;
        else if (!path)
            ret = -LINUX_EFAULT;
        else
            ret = stat_ret(fstatat(host_dirfd(a[0]), path, &st, flags), &st,
                           a[2]);
        break;
    }
    case NR_fstat:
        ret = stat_ret(fstat(a[0], &st), &st, a[1]);
        break;
```

`lstat` reaches `newfstatat` with `AT_SYMLINK_NOFOLLOW`, which is `0x100` on
Linux and `0x20` on macOS. Other flags return `EINVAL`.

## Error numbers again

A file name longer than 255 bytes makes macOS fail with `ENAMETOOLONG`, error
63 there and 36 on Linux, and part 4's `host_ret()` converts it:

```sh
$ build/mini-elfuse build/files $(printf 'x%.0s' $(seq 1 300))
fopen: File name too long
```

With `host_ret()` reduced to `return r < 0 ? -errno : r;`, the same run
reports Linux's error 63 instead:

```sh
fopen: Out of streams resources
```

## Running it

```sh
$ build/mini-elfuse build/files note.txt
note.txt: 27 bytes
written by a Linux program
```

The trace, with the output sent to a file and shown from the first `openat`:

```sh
$ build/mini-elfuse -v build/files note.txt > trace.txt 2>&1
$ cat trace.txt
...
syscall 56(0xffffffffffffff9c, 0x7ffffe7, 0x241, 0x1b6, 0x7ffffe7, 0x1)
  -> 3 (0x3)
syscall 80(0x3, 0x7fffbd0, 0x7fffbd0, 0x46ada0, 0xffffffff, 0x495500)
  -> 0 (0x0)
syscall 64(0x3, 0x495730, 0x1b, 0x3, 0x5e8, 0x1b)
  -> 27 (0x1b)
syscall 57(0x3, 0x40a000, 0xfbad2c04, 0x1b, 0x5e8, 0x1b)
  -> 0 (0x0)
syscall 214(0x4c0000, 0x4c0000, 0xffffffffffff0000, 0x492000, 0x4153a0, 0x3a8e0)
  -> 4980736 (0x4c0000)
syscall 79(0xffffffffffffff9c, 0x7ffffe7, 0x7fffdb0, 0x0, 0x495500, 0x230)
  -> 0 (0x0)
syscall 80(0x1, 0x7fff520, 0x7fff520, 0x46ada0, 0xffffffff, 0x490270)
  -> 0 (0x0)
syscall 56(0xffffffffffffff9c, 0x7ffffe7, 0x0, 0x0, 0x7ffffe7, 0x1)
  -> 3 (0x3)
syscall 80(0x3, 0x7fffb60, 0x7fffb60, 0x46ada0, 0x1, 0x0)
  -> 0 (0x0)
syscall 63(0x3, 0x496740, 0x1000, 0x3, 0xfbad2488, 0x495500)
  -> 27 (0x1b)
syscall 63(0x3, 0x496740, 0x1000, 0x3, 0x1, 0x0)
  -> 0 (0x0)
syscall 57(0x3, 0x40a000, 0x808, 0x494700, 0x1, 0x0)
  -> 0 (0x0)
syscall 64(0x1, 0x495730, 0x2e, 0x1, 0x5e8, 0x2e)
note.txt: 27 bytes
written by a Linux program
  -> 46 (0x2e)
syscall 94(0x0, 0x0, 0x30, 0x494700, 0x5e8, 0x2e)
```

- `fopen(path, "w")` becomes `openat` with flags `0x241`, Linux's
  `O_WRONLY | O_CREAT | O_TRUNC`, and mode `0x1b6`, octal 666. macOS returns
  descriptor 3.
- glibc calls `fstat` on the new descriptor, writes the 27 bytes when `fclose`
  flushes, and closes it.
- When `fclose` frees the stream, glibc gives 64 KiB of heap back: `brk`
  lowers the end of the heap to `0x4c0000`.
- `stat` is `newfstatat` on `AT_FDCWD` with no flags.
- The second `fopen` opens with flags 0, `O_RDONLY`. `fgets` reads 27 bytes,
  and a second `read` returns 0 at the end of the file.
- `fstat` on standard output, a file here, now succeeds, and the two lines
  leave in one `write` at exit. At a terminal, glibc also calls `ioctl` (29)
  on it, which fails.

`make check` runs the program in the build directory and compares both lines:

```make
	cd $(BUILD) && test "$$(./mini-elfuse ./files note.txt)" = \
	    "$$(printf 'note.txt: 27 bytes\nwritten by a Linux program')"
```

[Part 7](../07-mmap/) adds `mmap`.

## The code

The files are served with this site under `mini-elfuse/step-6/`:

```sh
mkdir step-6 && cd step-6
for f in Makefile mini-elfuse.c vectors.S entitlements.plist hello.S null.S printf.c files.c; do
    curl -fsSO https://henrybear327.github.io/notes/mini-elfuse/step-6/$f
done
make check
```

`Makefile`:

{{< include-file "static/mini-elfuse/step-6/Makefile" "make" >}}

`mini-elfuse.c`:

{{< include-file "static/mini-elfuse/step-6/mini-elfuse.c" "c" >}}

`vectors.S`:

{{< include-file "static/mini-elfuse/step-6/vectors.S" "asm" >}}

`entitlements.plist`:

{{< include-file "static/mini-elfuse/step-6/entitlements.plist" "xml" >}}

`hello.S`:

{{< include-file "static/mini-elfuse/step-6/hello.S" "asm" >}}

`null.S`:

{{< include-file "static/mini-elfuse/step-6/null.S" "asm" >}}

`printf.c`:

{{< include-file "static/mini-elfuse/step-6/printf.c" "c" >}}

`files.c`:

{{< include-file "static/mini-elfuse/step-6/files.c" "c" >}}

## What we learned

- File calls carry Linux values that macOS reads differently: `AT_FDCWD` is
  -100 against -2, open flags such as `O_CREAT` have different bits, and
  `struct stat` has a different layout.
- Each conversion is small: one comparison, a table of flags, and a
  field-by-field copy.
- Paths and file descriptors pass through unchanged, which elfuse does not do.
- Error numbers above 34 need part 4's table, or the program reports the wrong
  error.
