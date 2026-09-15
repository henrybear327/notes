# mini-elfuse step 5: glibc's printf

Adds the initial stack, `brk`, `writev`, `uname`, and the processor settings a
static glibc program needs, so `printf` prints.

Explained in
[mini-elfuse 5: glibc's printf](https://henrybear327.github.io/notes/posts/mini-elfuse/05-glibc-printf/).

## Build and run

Needs an Apple Silicon Mac with `clang` and `codesign`, and the
`aarch64-linux-gnu-gcc` cross compiler:

    brew tap messense/macos-cross-toolchains
    brew install aarch64-unknown-linux-gnu

Then:

    make check
    build/mini-elfuse -v build/printf
