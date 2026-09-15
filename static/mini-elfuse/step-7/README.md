# mini-elfuse step 7: mmap

Adds `mmap`, `munmap`, and `mprotect`. Every mapping takes fresh
addresses; file mappings are copies made with `pread`.

Explained in
[mini-elfuse 7: mmap](https://henrybear327.github.io/notes/posts/mini-elfuse/07-mmap/).

## Build and run

Needs an Apple Silicon Mac with `clang` and `codesign`, and the
`aarch64-linux-gnu-gcc` cross compiler:

    brew tap messense/macos-cross-toolchains
    brew install aarch64-unknown-linux-gnu

Then:

    make check
    build/mini-elfuse build/mmap
