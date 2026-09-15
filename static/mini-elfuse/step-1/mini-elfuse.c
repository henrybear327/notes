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
#define USER_BASE 0x400000ULL

enum { ET_EXEC = 2, EM_AARCH64 = 183, PT_LOAD = 1, PT_INTERP = 3 };

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

static uint8_t *slab;
static bool verbose;

#define die(...) errx(1, __VA_ARGS__)
#define vlog(fmt, ...) (verbose ? fprintf(stderr, fmt "\n", __VA_ARGS__) : 0)

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
    }
    close(fd);
    return eh.e_entry;
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
    return 0;
}
