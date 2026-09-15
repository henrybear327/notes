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
