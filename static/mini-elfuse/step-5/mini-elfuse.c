#include <Hypervisor/Hypervisor.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Guest address X is slab[X]. mini-elfuse keeps the memory below USER_BASE
 * for itself; the program gets the rest.
 */
#define GUEST_SIZE (1ULL << 30)
#define VECTORS_BASE 0x200000ULL
#define USER_BASE 0x400000ULL
#define STACK_TOP 0x08000000ULL
#define BRK_LIMIT (STACK_TOP - (8ULL << 20)) /* 8 MiB for the stack */

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
    NR_write = 64,
    NR_writev = 66,
    NR_exit = 93,
    NR_exit_group = 94,
    NR_uname = 160,
    NR_brk = 214,
};
enum { LINUX_EFAULT = 14, LINUX_EINVAL = 22, LINUX_ENOSYS = 38 };

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
    case NR_exit:
    case NR_exit_group:
        exit(a[0]);
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
        if (a[0] >= brk_base && a[0] < BRK_LIMIT) {
            /* Linux unmaps pages the break gives back; they return zeroed */
            if (a[0] < brk_cur)
                memset(slab + a[0], 0, brk_cur - a[0]);
            brk_cur = a[0];
        }
        ret = brk_cur;
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
