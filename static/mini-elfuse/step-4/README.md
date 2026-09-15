# mini-elfuse step 4: write and exit

Handles the `write` and `exit` system calls, so `hello` prints and exits.

Explained in
[mini-elfuse 4: write and exit](https://henrybear327.github.io/notes/posts/mini-elfuse/04-write-and-exit/).

## Build and run

Needs an Apple Silicon Mac with `clang` and `codesign`, and the
`aarch64-linux-gnu-gcc` cross compiler:

    brew tap messense/macos-cross-toolchains
    brew install aarch64-unknown-linux-gnu

Then:

    make check
    build/mini-elfuse -v build/hello
