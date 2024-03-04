---
title: 'rv32emu Development Image'
date: 2024-03-04T23:36:52+01:00
tags: ["rv32emu", "development", "docker image"]
categories: ["rv32emu"]
author: ["Chun-Hung Tseng"]
draft: false
---

This dockerfile [didn't make it](https://github.com/sysprog21/rv32emu/pull/328) into the upstream repo because of "the specific utilization context of Docker images, which are designed to streamline deployment processes for particular objectives, rather than encompassing a comprehensive solution".

But nevertheless, I am using it for my day-to-day development on my M1 Macbook, and I find it useful since it won't pollute my MacOS and also provides Linux-like environment. 

Hopefully you will find this useful, too!

# The `dockerfile`

Assuming that you copy-paste the content into a file named `Dockerfile-dev`.

To build, run `docker build -t rv32emu_dev -f Dockerfile-dev .` in the `rv32emu` directory.

To obtain a container, run `docker run --rm -it rv32emu_dev`.

If you would like to work on `rv32emu` and have it available to be seen in the container, run `docker run --rm -v ${PWD}:/home/sysprog21/rv32emu -it rv32emu_dev`.

```dockerfile
FROM sysprog21/rv32emu-gcc as base_gcc
FROM sysprog21/rv32emu-sail as base_sail

# for C++20
FROM ubuntu:23.10 as final 

# https://code.visualstudio.com/remote/advancedcontainers/add-nonroot-user#_creating-a-nonroot-user
# add a non-root user
ARG USERNAME=sysprog21
ARG USER_UID=1001 # NOTICE: 1000 already exists for 23.04
ARG USER_GID=$USER_UID

# Create the user
RUN groupadd --gid $USER_GID $USERNAME && \
    useradd --uid $USER_UID --gid $USER_GID -m $USERNAME && \
    # Add sudo support
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y sudo && \
    echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USERNAME && \
    chmod 0440 /etc/sudoers.d/$USERNAME && \
    rm -rf /var/lib/apt/lists/*

# Set the default user
USER $USERNAME

# Install extra packages for the emulator to compile and execute with full capabilities correctly
RUN sudo apt-get update && \
    DEBIAN_FRONTEND=noninteractive sudo apt-get install -y \
    python3-pip git clang strace gdb valgrind \
    vim clang-tidy clang clang-tools libsdl2-dev libsdl2-mixer-dev && \
    sudo rm -rf /var/lib/apt/lists/*

# NOTICE the use of --break-system-packages
RUN sudo python3 -m pip install git+https://github.com/riscv/riscof --break-system-packages

# copy in the source code
WORKDIR /home/$USERNAME/rv32emu
COPY --chown=$USERNAME:$USERNAME . .

# Copy the GNU Toolchain files
ENV RISCV=/opt/riscv
ENV PATH=$RISCV/bin:$PATH
COPY --chown=$USERNAME:$USERNAME --from=base_gcc /opt/riscv/ /opt/riscv/

# replace the emulator (riscv_sim_RV32) with the arch that the container can execute 
RUN rm /home/$USERNAME/rv32emu/tests/arch-test-target/sail_cSim/riscv_sim_RV32
COPY --chown=$USERNAME:$USERNAME --from=base_sail /home/root/riscv_sim_RV32 /home/$USERNAME/rv32emu/tests/arch-test-target/sail_cSim/riscv_sim_RV32

# Set the default directory
WORKDIR /home/$USERNAME/rv32emu
RUN make distclean

```

## DockerHub images for the compiler and Sail
Notice that the [RISC-V compiler toolchain](https://hub.docker.com/repository/docker/sysprog21/rv32emu-gcc/general) and [Sail](https://hub.docker.com/repository/docker/sysprog21/rv32emu-sail/general) are both [pre-compiled images (available on DockerHub)](https://github.com/sysprog21/rv32emu/commit/d212f96ac03a8d1ec82932660a653bd7794cf36e), as the offical images are only available for x86 at the time of writring so we need to compile-from-source for M1.
