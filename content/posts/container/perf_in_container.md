---
title: 'Installing, executing, and analyzing program performance in a container'
date: 2024-03-04T22:02:58+01:00
tags: ["Container", "Perf", "Flamegraph"]
categories: ["Container"]
author: ["Chun-Hung Tseng"]
draft: true
---

# Install Perf in the container

## The issue with perf in a container

Well written document at https://www.swift.org/documentation/server/guides/linux-perf.html

## Solution - [LinuxKit](https://www.docker.com/blog/introducing-linuxkit-container-os-toolkit/)

Dockerfile here

```
```

# Executing Perf in the container

- [Brendan Gregg's perf guide](https://www.brendangregg.com/perf.html)

On M1 - missing PMC counter -> can't use flamegraph (perf script -v you can see the conversion failed)
- perf stat -- sleep 0.1 (screenshot)

On Intel, 2 things required
--previ... https://stackoverflow.com/questions/44745987/use-perf-inside-a-docker-container-without-privileged
--SYS_CAP (no PMC without this)

# Analyzing program performance - with FlameGraph

- [Flamegraph](https://github.com/brendangregg/FlameGraph)

# Some related (but also not-so-related) readings

- [Linux Performance Analysis in 60,000 Milliseconds](https://netflixtechblog.com/linux-performance-analysis-in-60-000-milliseconds-accc10403c55)
- [Perf guide (for Rust)](https://gendignoux.com/blog/2019/11/09/profiling-rust-docker-perf.html)