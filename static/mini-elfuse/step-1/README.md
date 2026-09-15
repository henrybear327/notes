# mini-elfuse step 1: Loading a Linux program

Reads a static aarch64 Linux ELF file and copies its `PT_LOAD` segments into
a 1 GiB slab. Nothing runs yet.

Explained in
[mini-elfuse 1: Loading a Linux program](https://henrybear327.github.io/notes/posts/mini-elfuse/01-loading-a-linux-program/).

## Build and run

Needs a Mac with `clang` and the `aarch64-linux-gnu-gcc` cross compiler:

    brew tap messense/macos-cross-toolchains
    brew install aarch64-unknown-linux-gnu

Then:

    make check
