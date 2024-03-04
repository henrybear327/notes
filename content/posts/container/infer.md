---
title: 'Infer Docker Image'
date: 2024-03-05T00:01:00+01:00
tags: ["rv32emu", "infer"]
categories: ["rv32emu"]
author: ["Chun-Hung Tseng"]
draft: false
---

In order to leverage [Infer](https://github.com/facebook/infer) to check the C/C++ and Python source code in rv32emu, we compile it from source. 

> At the time of writing, the official release version is stuck at [v1.1.0](https://github.com/facebook/infer/releases/tag/v1.1.0) from May, 2021. Thus, we have to build our own docker image to leverage the latest changes!

The daily build is triggered and pushed to [sysprog21's DockerHub infer repo](https://hub.docker.com/repository/docker/sysprog21/infer/general) at UTC+8 0:00 every day. *Notice that this Docker image only runs on x86.*

# The `dockerfile`

Assuming that the filename of the following content is copied into a file named `Dockerfile-infer`.

To build using the latest commit, and push to DockerHub, execute `docker buildx build --progress=plain --push --platform linux/amd64 --tag sysprog21/infer:latest -f Dockerfile-infer .` Each build will take several hours to complete.

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

# Reference
- [Upstream dockefile code](https://github.com/facebook/infer/commit/e4c65eb2a1851fb8aa02c1f632dc1a8274189b28)
