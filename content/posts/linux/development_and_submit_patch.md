---
title: 'Making contributions to linux kernel upstream'
date: 2024-06-17T08:02:53+02:00
tags: ["linux", "LKML"]
categories: ["linux"]
author: ["Chun-Hung Tseng"]
draft: false
---

自己上 patch 到 linux kernel mailing list 的筆記。

- kernel 開發筆記
    - vim 設定 `:set tabstop=8 softtabstop=8 shiftwidth=8 noexpandtab cindent cc=80`
    - 編譯 `make defconfig && make -j $(nproc)`
    - 檢查
        - `./scripts/checkpatch.pl --patch`
        - 使用 [sparse](https://www.kernel.org/doc/html/latest/dev-tools/sparse.html) -> `make C=1`
- LKML 上提交 patch
    - [Generate patch](http://nickdesaulniers.github.io/blog/2017/05/16/submitting-your-first-patch-to-the-linux-kernel-and-responding-to-feedback/)
        - Generate patch `git format-patch HEAD~`
            - With versioning `git format-patch -v3 HEAD^`
        - Apply patch `git am [patch filename]`
    - [Setup email](https://git-send-email.io/) using gmail on Ubuntu
        - `sudo apt install git git-email`
        - Get app password from [here](https://security.google.com/settings/security/apppasswords)
        - Edit `~/.gitconfig`
            ```
            [sendemail]
                smtpPass = ...
                smtpserver = smtp.gmail.com
                smtpuser = ...@gmail.com
                smtpencryption = tls
                smtpserverport = 587
            [user]
                email = ...@gmail.com
                name = ...
            ```
        - Try to send a patch using the instructions [here](https://git-send-email.io/#step-3) 
    - Get the maintainer's email to send to 
        - e.g. `./scripts/get_maintainer.pl -f include/linux/compiler.h`
    - [Reply review emails](https://wiki.openstack.org/wiki/MailingListEtiquette#Replies)
        - 針對 gmail 的 email, 改成 plain text mode, 就可以正常使用 reply all 解決
- References
    - [henrybear327 hackmd notes](https://hackmd.io/KYskENooQzCUrHD3MWXIcw?view)