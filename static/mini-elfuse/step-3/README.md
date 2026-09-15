# mini-elfuse step 3: Page tables

Maps guest memory with three page tables, turns the MMU on, and leaves the
first 2 MiB unmapped, so `null` faults.

Explained in
[mini-elfuse 3: Page tables](https://henrybear327.github.io/notes/posts/mini-elfuse/03-page-tables/).

## Build and run

Needs an Apple Silicon Mac with `clang` and `codesign`, and the
`aarch64-linux-gnu-gcc` cross compiler:

    brew tap messense/macos-cross-toolchains
    brew install aarch64-unknown-linux-gnu

Then:

    make check
    build/mini-elfuse build/null
