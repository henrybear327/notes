---
title: 'Etcd_node_networking'
date: 2024-04-13T20:38:04+02:00
tags: ["TBD"]
categories: ["TBD"]
author: ["Chun-Hung Tseng"]
draft: true
---

Raft nodes doesn't communicate with the outside directly, on its own. The traffic goes through etcdserver. 
There are 2 modes, streams (Reader and writer) and pipeline. Uses cases are different (as we can see from the switch function)
If the connection is initiated from the outside, we can use proxy to block incoming / outgoing traffic
If the connection is initized from the inside, then we need to block from L4 or L7