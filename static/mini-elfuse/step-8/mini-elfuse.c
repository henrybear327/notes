#include <Hypervisor/Hypervisor.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Guest address X is slab[X]. mini-elfuse keeps the memory below USER_BASE
 * for itself; the program gets the rest.
 */
#define GUEST_SIZE (1ULL << 30)
#define VECTORS_BASE 0x200000ULL
#define USER_BASE 0x400000ULL
#define STACK_TOP 0x08000000ULL
#define BRK_LIMIT (STACK_TOP - (8ULL << 20)) /* 8 MiB for the stack */
#define MMAP_BASE 0x10000000ULL

enum { ET_EXEC = 2, EM_AARCH64 = 183, PT_LOAD = 1, PT_INTERP = 3 };
enum { AT_NULL = 0, AT_RANDOM = 25 };

/* Exception classes, ESR bits 31:26 */
enum { EC_SVC = 0x15, EC_HVC = 0x16 };

/* Page table entries */
#define PTE_BLOCK 0x1ULL          /* maps 2 MiB at level 2 */
#define PTE_TABLE 0x3ULL          /* points to the next level */
#define PTE_AP_EL0_RW (1ULL << 6) /* set: EL0 may read and write */
#define PTE_AF (1ULL << 10)       /* access flag; clear means fault on use */

#define TCR_T0SZ_48 16ULL /* 48-bit addresses, 4 KiB granule: walk from L0 */
#define SCTLR_RES1 0x30d00980ULL
#define SCTLR_M 0x1ULL          /* MMU on */
#define SCTLR_C 0x4ULL          /* data cache; without it ldaxr/stxr fault */
#define CPACR_FPEN (3ULL << 20) /* FP and SIMD at EL0 and EL1 */

/* Linux aarch64 syscall numbers and errno values */
enum {
    NR_openat = 56,
    NR_close = 57,
    NR_read = 63,
    NR_write = 64,
    NR_writev = 66,
    NR_newfstatat = 79,
    NR_fstat = 80,
    NR_exit = 93,
    NR_exit_group = 94,
    NR_futex = 98,
    NR_uname = 160,
    NR_brk = 214,
    NR_munmap = 215,
    NR_clone = 220,
    NR_mmap = 222,
    NR_mprotect = 226,
};
enum {
    LINUX_EAGAIN = 11,
    LINUX_ENOMEM = 12,
    LINUX_EFAULT = 14,
    LINUX_EINVAL = 22,
    LINUX_ENOSYS = 38,
};
#define LINUX_AT_FDCWD -100
enum { LINUX_MAP_FIXED = 0x10, LINUX_MAP_ANONYMOUS = 0x20 };
enum {
    CLONE_THREAD = 0x10000,
    CLONE_SETTLS = 0x80000,
    CLONE_PARENT_SETTID = 0x100000,
    CLONE_CHILD_CLEARTID = 0x200000,
};
enum { FUTEX_WAIT = 0, FUTEX_WAKE = 1 };

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

/* vectors.S */
extern const uint8_t vectors[], vectors_end[];

static uint8_t *slab;
static bool verbose;
static uint64_t brk_base, brk_cur;
static uint64_t mmap_next = MMAP_BASE;

/* lock guards brk_cur, mmap_next and next_tid, and every futex wait and
 * wake: one condition variable stands for all futex addresses.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t futex_cond = PTHREAD_COND_INITIALIZER;
static int32_t next_tid = 2;
static __thread uint64_t clear_child_tid;

struct thread_start {
    uint64_t x[31], pc, sp, tls, clear_child_tid;
};

#define die(...) errx(1, __VA_ARGS__)
#define vlog(fmt, ...) (verbose ? fprintf(stderr, fmt "\n", __VA_ARGS__) : 0)

#define HV(call)                                              \
    do {                                                      \
        hv_return_t hv_ret = (call);                          \
        if (hv_ret != HV_SUCCESS)                             \
            die("%s failed: 0x%x", #call, (unsigned) hv_ret); \
    } while (0)

/* Host address of guest range [addr, addr + len), or NULL unless the whole
 * range is program memory.
 */
static void *guest_ptr(uint64_t addr, uint64_t len)
{
    if (addr < USER_BASE || addr > GUEST_SIZE || len > GUEST_SIZE - addr)
        return NULL;
    return slab + addr;
}

/* A NUL-terminated string in program memory, or NULL */
static const char *guest_str(uint64_t addr)
{
    const char *s = guest_ptr(addr, 1);
    return s && memchr(s, 0, GUEST_SIZE - addr) ? s : NULL;
}

static uint64_t load_elf(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die("%s: %s", path, strerror(errno));

    struct elf64_ehdr eh;
    if (pread(fd, &eh, sizeof eh, 0) != sizeof eh ||
        memcmp(eh.e_ident, "\177ELF", 4) != 0)
        die("%s: not an ELF file", path);
    /* e_ident[4] == 2: 64-bit, e_ident[5] == 1: little-endian */
    if (eh.e_ident[4] != 2 || eh.e_ident[5] != 1 || eh.e_type != ET_EXEC ||
        eh.e_machine != EM_AARCH64 ||
        eh.e_phentsize != sizeof(struct elf64_phdr))
        die("%s: not a 64-bit aarch64 executable", path);

    uint64_t end = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        struct elf64_phdr ph;
        if (pread(fd, &ph, sizeof ph, eh.e_phoff + i * sizeof ph) != sizeof ph)
            die("%s: cannot read program header %d", path, i);
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
        vlog("load 0x%llx-0x%llx", ph.p_vaddr, ph.p_vaddr + ph.p_memsz);
        if (ph.p_vaddr + ph.p_memsz > end)
            end = ph.p_vaddr + ph.p_memsz;
    }
    close(fd);
    if (end > BRK_LIMIT)
        die("%s: too large", path);
    brk_base = brk_cur = (end + 0xfff) & ~0xfffULL;
    return eh.e_entry;
}

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

static uint64_t reg(hv_vcpu_t vcpu, hv_reg_t r)
{
    uint64_t v;
    HV(hv_vcpu_get_reg(vcpu, r, &v));
    return v;
}

static uint64_t sysreg(hv_vcpu_t vcpu, hv_sys_reg_t r)
{
    uint64_t v;
    HV(hv_vcpu_get_sys_reg(vcpu, r, &v));
    return v;
}

/* A new vCPU has CPSR 0, which is EL0, and x0 to x30 zeroed. */
static void boot_vcpu(hv_vcpu_t vcpu, uint64_t entry, uint64_t sp)
{
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, sp));
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, VECTORS_BASE));
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, 0x1000));
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1, TCR_T0SZ_48));
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, 0xff)); /* attr 0: RAM */
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1,
                           SCTLR_RES1 | SCTLR_M | SCTLR_C));
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, CPACR_FPEN));
    HV(hv_vcpu_set_reg(vcpu, HV_REG_PC, entry));
}

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

static int host_dirfd(uint64_t fd)
{
    return fd == (uint64_t) LINUX_AT_FDCWD ? AT_FDCWD : (int) fd;
}

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

static void run_vcpu(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit);

/* A thread made by clone gets its own vCPU, which Hypervisor.framework ties
 * to the host thread that creates it. It boots like the first one and resumes
 * after the parent's svc with the parent's general-purpose registers.
 */
static void *thread_main(void *arg)
{
    struct thread_start *t = arg;
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    HV(hv_vcpu_create(&vcpu, &vexit, NULL));
    boot_vcpu(vcpu, t->pc, t->sp);
    for (int i = 0; i < 31; i++)
        HV(hv_vcpu_set_reg(vcpu, HV_REG_X0 + i, t->x[i]));
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, t->tls));
    clear_child_tid = t->clear_child_tid;
    free(t);
    run_vcpu(vcpu, vexit);
    return NULL;
}

static void do_syscall(hv_vcpu_t vcpu)
{
    uint64_t nr = reg(vcpu, HV_REG_X8), a[6];
    for (int i = 0; i < 6; i++)
        a[i] = reg(vcpu, HV_REG_X0 + i);
    vlog("syscall %llu(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)", nr,
         a[0], a[1], a[2], a[3], a[4], a[5]);

    int64_t ret;
    struct stat st;
    switch (nr) {
    case NR_openat: {
        const char *path = guest_str(a[1]);
        ret = path ? host_ret(openat(host_dirfd(a[0]), path,
                                     host_open_flags(a[2]), (int) a[3]))
                   : -LINUX_EFAULT;
        break;
    }
    case NR_close:
        ret = host_ret(close(a[0]));
        break;
    case NR_read: {
        void *buf = guest_ptr(a[1], a[2]);
        ret = buf ? host_ret(read(a[0], buf, a[2])) : -LINUX_EFAULT;
        break;
    }
    case NR_write: {
        void *buf = guest_ptr(a[1], a[2]);
        ret = buf ? host_ret(write(a[0], buf, a[2])) : -LINUX_EFAULT;
        break;
    }
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
    case NR_exit:
        /* exit ends only the calling thread. mini-elfuse does not count
         * threads, so exit from the initial thread ends the process.
         */
        if (!pthread_main_np()) {
            int32_t *tid = guest_ptr(clear_child_tid, 4);
            pthread_mutex_lock(&lock);
            if (tid) {
                *tid = 0;
                pthread_cond_broadcast(&futex_cond);
            }
            pthread_mutex_unlock(&lock);
            HV(hv_vcpu_destroy(vcpu));
            pthread_exit(NULL);
        }
        /* fall through */
    case NR_exit_group:
        exit(a[0]);
    case NR_futex: {
        /* A woken waiter rechecks the value in user space, so waking every
         * waiter is correct, only slower. The timeout in a[3] is ignored.
         */
        int32_t *val = guest_ptr(a[0], 4);
        int op = a[1] & 0x7f; /* without FUTEX_PRIVATE_FLAG */
        pthread_mutex_lock(&lock);
        ret = 0;
        if (!val)
            ret = -LINUX_EFAULT;
        else if (op == FUTEX_WAKE)
            pthread_cond_broadcast(&futex_cond);
        else if (op != FUTEX_WAIT)
            ret = -LINUX_ENOSYS;
        else if (*val != (int32_t) a[2])
            ret = -LINUX_EAGAIN;
        else
            pthread_cond_wait(&futex_cond, &lock);
        pthread_mutex_unlock(&lock);
        break;
    }
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
    case NR_brk:
        /* A refused request returns the unchanged break. */
        pthread_mutex_lock(&lock);
        if (a[0] >= brk_base && a[0] < BRK_LIMIT) {
            /* Linux unmaps pages the break gives back; they return zeroed */
            if (a[0] < brk_cur)
                memset(slab + a[0], 0, brk_cur - a[0]);
            brk_cur = a[0];
        }
        ret = brk_cur;
        pthread_mutex_unlock(&lock);
        break;
    case NR_mmap: {
        /* Every page is already mapped read-write-execute, so mmap only hands
         * out addresses. They are never reused, so they still read as zero.
         */
        if (a[3] & LINUX_MAP_FIXED || a[1] == 0) {
            ret = -LINUX_EINVAL;
            break;
        }
        pthread_mutex_lock(&lock);
        uint64_t addr = mmap_next;
        void *p = guest_ptr(addr, a[1]);
        if (p)
            mmap_next += (a[1] + 0xfff) & ~0xfffULL;
        pthread_mutex_unlock(&lock);
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
    case NR_clone: {
        /* clone(flags, stack, parent_tid, tls, child_tid) */
        if (!(a[0] & CLONE_THREAD)) {
            ret = -LINUX_ENOSYS;
            break;
        }
        struct thread_start *t = malloc(sizeof *t);
        for (int i = 0; i < 31; i++)
            t->x[i] = reg(vcpu, HV_REG_X0 + i);
        t->x[0] = 0; /* the new thread sees clone return 0 */
        t->pc = sysreg(vcpu, HV_SYS_REG_ELR_EL1);
        t->sp = a[1];
        t->tls =
            a[0] & CLONE_SETTLS ? a[3] : sysreg(vcpu, HV_SYS_REG_TPIDR_EL0);
        t->clear_child_tid = a[0] & CLONE_CHILD_CLEARTID ? a[4] : 0;

        pthread_mutex_lock(&lock);
        ret = next_tid++;
        pthread_mutex_unlock(&lock);
        int32_t *parent_tid = guest_ptr(a[2], 4);
        if (a[0] & CLONE_PARENT_SETTID && parent_tid)
            *parent_tid = ret;

        pthread_t thread;
        if (pthread_create(&thread, NULL, thread_main, t) != 0)
            die("pthread_create failed");
        pthread_detach(thread);
        break;
    }
    case NR_munmap:
    case NR_mprotect:
        ret = 0;
        break;
    default:
        ret = -LINUX_ENOSYS;
    }
    vlog("  -> %lld (0x%llx)", ret, ret);
    HV(hv_vcpu_set_reg(vcpu, HV_REG_X0, ret));
}

static void run_vcpu(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit)
{
    for (;;) {
        HV(hv_vcpu_run(vcpu));
        uint64_t syndrome = vexit->exception.syndrome;
        if (vexit->reason != HV_EXIT_REASON_EXCEPTION ||
            syndrome >> 26 != EC_HVC)
            die("unexpected exit: reason %u, syndrome 0x%llx", vexit->reason,
                syndrome);
        uint64_t esr = sysreg(vcpu, HV_SYS_REG_ESR_EL1);
        if ((syndrome & 0xffff) != 5 || esr >> 26 != EC_SVC)
            die("exception: ESR_EL1 0x%llx, FAR_EL1 0x%llx, ELR_EL1 0x%llx",
                esr, sysreg(vcpu, HV_SYS_REG_FAR_EL1),
                sysreg(vcpu, HV_SYS_REG_ELR_EL1));

        do_syscall(vcpu);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        verbose = true;
        argc--;
        argv++;
    }
    if (argc < 2)
        die("usage: mini-elfuse [-v] program [args...]");

    slab = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE,
                MAP_ANON | MAP_PRIVATE, -1, 0);
    if (slab == MAP_FAILED)
        die("mmap: %s", strerror(errno));

    uint64_t entry = load_elf(argv[1]);
    vlog("entry 0x%llx", entry);

    HV(hv_vm_create(NULL));
    HV(hv_vm_map(slab, 0, GUEST_SIZE,
                 HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    memcpy(slab + VECTORS_BASE, vectors, vectors_end - vectors);
    build_page_tables();

    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    HV(hv_vcpu_create(&vcpu, &vexit, NULL));
    boot_vcpu(vcpu, entry, build_stack(argc - 1, argv + 1));
    run_vcpu(vcpu, vexit);
}
