---
title: 'Infer Docker Image'
date: 2024-03-05T00:01:00+01:00
tags: ["rv32emu", "infer"]
categories: ["rv32emu"]
author: ["Chun-Hung Tseng"]
draft: false
---

In order to leverage the latest [Infer](https://github.com/facebook/infer) features to check all C/C++ and Python source code in [rv32emu](https://github.com/sysprog21/rv32emu), we need to compile it from source, since at the time of writing, the official release version is stuck at [v1.1.0](https://github.com/facebook/infer/releases/tag/v1.1.0) from May, 2021. 

The daily build is triggered and pushed to [sysprog21's DockerHub infer repo](https://hub.docker.com/repository/docker/sysprog21/infer/general) at UTC+8 0:00 every day. *Notice that this Docker image only runs on x86*.

# The `dockerfile` 

Here are the `dockerfile`s behind each of the daily build. 

Assuming that the `dockerfile` content is copied into a file named `Dockerfile-infer`.

## Before and including commit `71a1166bce`

The image contains complied and working infer, along with all of the toolchains used to built it.

Since this is not a release build, java analysis capability is not included.

```dockerfile
FROM ubuntu:22.04

RUN apt-get update && \
    mkdir -p /usr/share/man/man1 && \
    apt-get install --yes --no-install-recommends \
    git ca-certificates build-essential autoconf automake cmake python3 curl sqlite3 \
    opam zlib1g-dev libgmp-dev libsqlite3-dev pkg-config && \
    rm -rf /var/lib/apt/lists/*
RUN opam init --disable-sandboxing -y

RUN git clone https://github.com/facebook/infer.git && \
    cd infer && \
    # Compile Infer to support C and Python3
    ./build-infer.sh -y clang python && \
    # Install Infer system-wide
    make install && \
    make clean

```

## After commit `71a1166bce`

The image contains complied and working infer, using the `create_binary_release.sh` script. We use the tarball and prepare the rv32emu compilation environment in the image.

Since this is a release build, java analysis capability is included.

```dockerfile
FROM ubuntu:22.04 as build

RUN apt-get update && \
    mkdir -p /usr/share/man/man1 && \
    apt-get install --yes --no-install-recommends \
    git ca-certificates build-essential autoconf automake cmake curl sqlite3 software-properties-common \
    opam zlib1g-dev libgmp-dev libsqlite3-dev pkg-config openjdk-8-jdk gpg-agent \
    clang libmpfr-dev libsqlite3-dev ninja-build patchelf && \
    rm -rf /var/lib/apt/lists/*

# install python 3.8 since infer only works with this version
RUN add-apt-repository ppa:deadsnakes/ppa -y && \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get -y install tzdata && \
    apt-get install --yes --no-install-recommends python3.8 && \
    rm -rf /var/lib/apt/lists/*
RUN update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.8 2
RUN update-alternatives --install /usr/bin/python3 python3 /usr/bin/python3.10 1

RUN opam init --disable-sandboxing -y

RUN git clone https://github.com/facebook/infer.git && \
    cd infer && \
    ./facebook-clang-plugins/clang/src/prepare_clang_src.sh && \
    CC=clang CXX=clang++ ./facebook-clang-plugins/clang/setup.sh --ninja --sequential-link && \
    ./scripts/create_binary_release.sh v1.2.0

RUN mv infer/infer-linux64-v1.2.0.tar.xz ../ && tar -xvf infer-linux64-v1.2.0.tar.xz

FROM ubuntu:22.04 as final

RUN apt-get update && \
    apt-get install --yes --no-install-recommends build-essential python3 && \
    rm -rf /var/lib/apt/lists/*

COPY --from=BUILD /infer-linux64-v1.2.0 /infer
ENV PATH=/infer/bin:$PATH
```

To build the Docker image using the latest infer commit and push to DockerHub, execute `docker buildx build --progress=plain --push --platform linux/amd64 --tag sysprog21/infer:latest -f Dockerfile-infer .` 

Notice that each build will take about an hour to complete.

# Execution (for rv32emu)

To check the codebase, execute `infer -- make ENABLE_SDL=0 ENABLE_LTO=0 ENABLE_EXT_F=0` in the container.

Notice that
- `ENABLE_SDL` and `ENABLE_EXT_F` are both set to 0 to exclude the analysis being run on external dependencies
- `ENABLE_LTO` is set to 0 due to some unresolved bug with link-time optimization turned on

# References
- [Upstream dockefile code](https://github.com/facebook/infer/commit/e4c65eb2a1851fb8aa02c1f632dc1a8274189b28)
- [Infer docker notes](https://hackmd.io/6zHcdQEpSsaZxA8RfpCOsQ)
