---
title: 'Weekly_dependencies_upgrade'
date: 2024-04-13T20:36:29+02:00
tags: ["TBD"]
categories: ["TBD"]
author: ["Chun-Hung Tseng"]
draft: true
---

The issue: dependabot can't upgrade all dependencies at once, if you have several go.mod files in a project
The solutions: go through each PR raised, grep the dependencies, and if it's not all indirect, we manually go through each of them, go get ...@v... and then run go mod tidy