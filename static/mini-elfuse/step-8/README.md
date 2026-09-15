# mini-elfuse step 8: Threads

Adds `clone`, `futex`, and thread exit: each thread runs on its own vCPU and
host thread.

Explained in
[mini-elfuse 8: Threads](https://henrybear327.github.io/notes/posts/mini-elfuse/08-threads/).

## Build and run

Needs an Apple Silicon Mac with `clang` and `codesign`, and the
`aarch64-linux-gnu-gcc` cross compiler:

    brew tap messense/macos-cross-toolchains
    brew install aarch64-unknown-linux-gnu

Then:

    make check
    build/mini-elfuse build/threads
