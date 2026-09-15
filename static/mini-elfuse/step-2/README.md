# mini-elfuse step 2: A VM and the first system call

Creates a Hypervisor.framework VM and a vCPU that runs the program at EL0 with
the MMU off, until its first `svc` reaches mini-elfuse.

Explained in
[mini-elfuse 2: A VM and the first system call](https://henrybear327.github.io/notes/posts/mini-elfuse/02-a-vm-and-the-first-system-call/).

## Build and run

Needs an Apple Silicon Mac with `clang` and `codesign`, and the
`aarch64-linux-gnu-gcc` cross compiler:

    brew tap messense/macos-cross-toolchains
    brew install aarch64-unknown-linux-gnu

Then:

    make check
    build/mini-elfuse -v build/hello
