---
title: 'Why We Need a Middleman: Understanding SSH Jump Hosts'
date: 2024-06-17T13:15:33+02:00
tags: ["ssh"]
categories: ["ssh"]
author: ["Chun-Hung Tseng"]
draft: false
---

Connecting to remote servers directly over the internet can be a recipe for disaster. Let's explore why SSH jump hosts are a secure and efficient solution, along with how they typically work.

# The Problem: Wide Open Gates

Imagine a network with critical servers tucked away behind a firewall, like a medieval city within a fortified wall. Directly connecting (SSH) to these servers from the open internet would be like leaving the city gates wide open – anyone could potentially exploit a vulnerability and gain unauthorized access.

If you have ever set up a [ssh honey pot](https://github.com/jaksi/sshesame) before, you will be surprised by how much attach traffic are coming in each day!

# The Solution: A Secure Gateway - The Jump Host

Enter the SSH jump host – a strategically placed server outside the firewall, acting as a secure gateway.  Think of it as a heavily guarded checkpoint before entering the city.  Users first connect to the jump host using SSH, then use it as a springboard to access the desired internal servers.

# Benefits of a Jump Host Architecture

There are several advantages to this approach:

- Enhanced Security: The attack surface shrinks dramatically. Hackers need to breach the jump host first, before even reaching the internal servers.
- Centralized Access Control: Permissions for accessing internal servers are managed on the jump host, simplifying administration and reducing the risk of human error. Imagine having just one key (jump host access) instead of keys for every server!
- Improved Audit Logging: All SSH activity is funneled through the jump host, making it easier to track user actions and identify suspicious behavior. Think of it like a logbook at the checkpoint, recording who enters and exits the city.

# Typical Jump Host Setup

Here's a breakdown of a typical jump host architecture:

- Public Network: The jump host resides in a public-facing zone, accessible from the internet. Firewalls heavily restrict inbound traffic, only allowing authorized SSH connections.
- Jump Host: This hardened server runs minimal services, focusing solely on secure SSH access. Strong passwords and two-factor authentication are essential.
- Internal Network: The actual servers you want to access reside behind a firewall, protected from direct internet access.
SSH Tunneling: Once logged into the jump host, users can securely tunnel connections to the internal servers through the secure jump host environment. Imagine a secret passage within the city walls!

For example:
```
Client A ---- Jump Host ---- Target Server
```

# SSH connection usage tips

## With command line

Use `-A` if you want to enables forwarding of connections from an authentication agent such as ssh-agent.

```bash
ssh -A -J user@jump_server:port  user@destination_server:port
```

## With `~/.ssh/config`

```
### First jumphost. Directly reachable
Host jumphost
    HostName jumphost.example.org

### Host to jump to via jumphost.example.org
Host targetServer
    HostName targetServer.example.org
    ProxyJump jumphost
```

# References
- https://www.tecmint.com/access-linux-server-using-a-jump-host/

> Written with help from Gemini :D
