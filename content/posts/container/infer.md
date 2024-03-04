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

This is the `dockerfile` behind each of the daily build.

Assuming that the following content is copied into a file named `Dockerfile-infer`.

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

To build the Docker image using the latest infer commit and push to DockerHub, execute `docker buildx build --progress=plain --push --platform linux/amd64 --tag sysprog21/infer:latest -f Dockerfile-infer .` 

Notice that each build will take several hours to complete.

# Execution

To check the codebase, execute `infer -- make ENABLE_SDL=0 ENABLE_LTO=0 ENABLE_EXT_F=0` in the container.

Notice that
- `ENABLE_SDL` and `ENABLE_EXT_F` are both set to 0 to exclude the analysis being run on external dependencies
- `ENABLE_LTO` is set to 0 due to some unresolved bug with link-time optimization turned on

# Reference
- [Upstream dockefile code](https://github.com/facebook/infer/commit/e4c65eb2a1851fb8aa02c1f632dc1a8274189b28)
