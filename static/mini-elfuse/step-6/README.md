# mini-elfuse step 6: Files

Adds `openat`, `read`, `close`, `newfstatat`, and `fstat`, with the flag,
`AT_FDCWD`, and `struct stat` conversions they need.

Explained in
[mini-elfuse 6: Files](https://henrybear327.github.io/notes/posts/mini-elfuse/06-files/).

## Build and run

Needs an Apple Silicon Mac with `clang` and `codesign`, and the
`aarch64-linux-gnu-gcc` cross compiler:

    brew tap messense/macos-cross-toolchains
    brew install aarch64-unknown-linux-gnu

Then:

    make check
    build/mini-elfuse build/files note.txt
