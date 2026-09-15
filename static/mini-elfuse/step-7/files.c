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
