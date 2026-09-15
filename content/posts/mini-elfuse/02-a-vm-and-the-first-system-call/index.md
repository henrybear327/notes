---
title: 'mini-elfuse 2: A VM and the first system call'
date: 2026-09-15T11:00:00+02:00
series: ["mini-elfuse"]
categories: ["elfuse"]
tags: ["elfuse", "mini-elfuse", "linux", "macos", "hypervisor"]
author: ["Chun-Hung Tseng"]
draft: true
---

Part 2 of a series that builds mini-elfuse, a small version of
[elfuse](https://github.com/sysprog21/elfuse).
[Part 1](../01-loading-a-linux-program/) copied a Linux program into a slab of
memory. This part runs it until its first system call and shows that call
arriving in mini-elfuse.

## The problem

The program's first system call is an `svc` instruction. If the Mac's
processor ran the program as an ordinary process, that `svc` would go to the
macOS kernel. [elfuse part 2](../../elfuse/02-what-a-virtual-machine-is/)
describes the way around it: run the program at EL0 inside a virtual machine,
where `svc` goes to the guest's EL1. Code at EL1 then issues `hvc`, which
Hypervisor.framework turns into an exit: `hv_vcpu_run()` returns in the host
process.

mini-elfuse needs three things for that: a virtual machine that uses the slab
as its memory, a virtual processor (vCPU) that starts at the program's entry
point, and code at EL1 for the `svc` to land in.

## The entitlement

macOS lets a process create a VM only if its code signature carries the
`com.apple.security.hypervisor` entitlement. `entitlements.plist` holds it:

```xml
<dict>
    <key>com.apple.security.hypervisor</key>
    <true/>
</dict>
```

The `Makefile` now links against Hypervisor.framework and signs the result
with an ad-hoc signature (`-s -`):

```make
$(BUILD)/mini-elfuse: mini-elfuse.c vectors.S entitlements.plist | $(BUILD)
	$(CC) -O2 -Wall -o $@ mini-elfuse.c vectors.S -framework Hypervisor
	codesign --entitlements entitlements.plist -f -s - $@
```

Every Hypervisor.framework call in mini-elfuse returns an `hv_return_t`, and
mini-elfuse stops on any failure with the call's text and the error code:

```c
#define HV(call)                                              \
    do {                                                      \
        hv_return_t hv_ret = (call);                          \
        if (hv_ret != HV_SUCCESS)                             \
            die("%s failed: 0x%x", #call, (unsigned) hv_ret); \
    } while (0)
```

Built without the `codesign` step, the first call fails. `0xfae94007` is
`HV_DENIED` in `hv_error.h`:

```sh
$ mkdir -p unsigned
$ clang -O2 -Wall -o unsigned/mini-elfuse mini-elfuse.c vectors.S -framework Hypervisor
$ unsigned/mini-elfuse build/hello
mini-elfuse: hv_vm_create(NULL) failed: 0xfae94007
```

## A VM and a vCPU

After loading the program, `main()` creates the VM, maps the slab into it at
guest physical address 0, copies the exception vectors (next section) into the
guest, and creates and runs a vCPU:

```c
    HV(hv_vm_create(NULL));
    HV(hv_vm_map(slab, 0, GUEST_SIZE,
                 HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    memcpy(slab + VECTORS_BASE, vectors, vectors_end - vectors);

    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *vexit;
    HV(hv_vcpu_create(&vcpu, &vexit, NULL));
    boot_vcpu(vcpu, entry);
    run_vcpu(vcpu, vexit);
```

Right after `hv_vcpu_create()`, `PC`, `CPSR`, the 31 general-purpose registers,
and system registers such as `SCTLR_EL1` and `VBAR_EL1` all read 0; only the
read-only ID registers, such as `MIDR_EL1`, do not. `CPSR` holds the
processor's current state, and 0 means EL0. So the vCPU already starts at the
program's privilege level, and `boot_vcpu()` sets only two registers:

```c
/* A new vCPU has CPSR 0, which is EL0, and x0 to x30 zeroed. */
static void boot_vcpu(hv_vcpu_t vcpu, uint64_t entry)
{
    HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, VECTORS_BASE));
    HV(hv_vcpu_set_reg(vcpu, HV_REG_PC, entry));
}
```

`PC` is the program's entry point. `VBAR_EL1` is the address of EL1's
exception vector table.

This differs from elfuse, whose vCPU starts at EL1 in the shim and drops to
EL0 with `eret` ([elfuse part 5](../../elfuse/05-booting-a-vm-with-no-kernel/)).
Address translation is also still off here: the processor uses each address
as a guest physical address. [Part 3](../03-page-tables/) turns it on.

## The exception vectors

When the program executes `svc`, the processor switches to EL1 and jumps into
the table at `VBAR_EL1`. The table has 16 entries of `0x80` bytes. Which entry
runs depends on the kind of exception and where it came from; a synchronous
exception, such as `svc` or a memory fault, from EL0 running 64-bit code goes
to the entry at `0x400`.

`vectors.S` fills all 16 entries:

```asm
/* EL1 exception vectors, copied to guest address VECTORS_BASE: 16 entries
 * of 0x80 bytes. Entry 0x400 takes svc and faults from EL0; it changes no
 * general-purpose register before mini-elfuse reads them.
 */
    .text
    .globl _vectors, _vectors_end
_vectors:
    .rept 8
    .p2align 7
    hvc #2              /* unexpected exception */
    .endr
    .p2align 7
    hvc #5
    eret
    .rept 7
    .p2align 7
    hvc #2
    .endr
_vectors_end:
```

`.p2align 7` pads to the next multiple of `0x80`, so each `hvc` starts its
own entry. Entry `0x400` is `hvc #5` followed by `eret`; the other 15 are
`hvc #2`, which should never run.

On Linux, `svc` keeps every general-purpose register except `x0`, which carries
the result, and compiled code relies on that
([elfuse part 5](../../elfuse/05-booting-a-vm-with-no-kernel/), "The
vector-entry rule").
elfuse's shim saves all 31 registers on entry because its handler overwrites
some of them. The entry here uses none: `hvc #5` exits to mini-elfuse with the
general-purpose registers as the program left them. When mini-elfuse resumes the
vCPU, `eret` returns to the program at the address in `ELR_EL1`, the instruction
after the `svc`.

The file is assembled by `clang` together with `mini-elfuse.c`, so its bytes
are part of the mini-elfuse binary. `_vectors` and `_vectors_end` are the
Mach-O names of the C symbols `vectors` and `vectors_end`:

```c
/* vectors.S */
extern const uint8_t vectors[], vectors_end[];
```

`main()` copies the bytes between them to `VECTORS_BASE`, `0x200000`, where
`VBAR_EL1` points. elfuse builds its shim into a C array instead
([elfuse part 5](../../elfuse/05-booting-a-vm-with-no-kernel/)).

## The run loop

`hv_vcpu_run()` runs the guest until the next exit and fills in `vexit`. An
exit caused by the guest's `hvc` has reason `HV_EXIT_REASON_EXCEPTION` and a
syndrome whose bits 31 to 26, the exception class, are `0x16`. The low 16 bits
are the number in the `hvc` instruction. Two helpers read a register and stop
on failure:

```c
static uint64_t reg(hv_vcpu_t vcpu, hv_reg_t r)
{
    uint64_t v;
    HV(hv_vcpu_get_reg(vcpu, r, &v));
    return v;
}

static uint64_t sysreg(hv_vcpu_t vcpu, hv_sys_reg_t r)
{
    uint64_t v;
    HV(hv_vcpu_get_sys_reg(vcpu, r, &v));
    return v;
}
```

`hvc #5` means entry `0x400` ran, but that entry also takes memory faults. The
guest's own `ESR_EL1` says which: exception class `0x15` is `svc`. Anything
else ends the run with the fault's registers:

```c
static void run_vcpu(hv_vcpu_t vcpu, hv_vcpu_exit_t *vexit)
{
    for (;;) {
        HV(hv_vcpu_run(vcpu));
        uint64_t syndrome = vexit->exception.syndrome;
        if (vexit->reason != HV_EXIT_REASON_EXCEPTION ||
            syndrome >> 26 != EC_HVC)
            die("unexpected exit: reason %u, syndrome 0x%llx", vexit->reason,
                syndrome);
        uint64_t esr = sysreg(vcpu, HV_SYS_REG_ESR_EL1);
        if ((syndrome & 0xffff) != 5 || esr >> 26 != EC_SVC)
            die("exception: ESR_EL1 0x%llx, FAR_EL1 0x%llx, ELR_EL1 0x%llx",
                esr, sysreg(vcpu, HV_SYS_REG_FAR_EL1),
                sysreg(vcpu, HV_SYS_REG_ELR_EL1));

        die("unhandled syscall %llu at 0x%llx", reg(vcpu, HV_REG_X8),
            sysreg(vcpu, HV_SYS_REG_ELR_EL1) - 4);
    }
}
```

`ESR_EL1` is the exception syndrome register, `FAR_EL1` the faulting address,
and `ELR_EL1` the return address. mini-elfuse does not handle any system call
yet, so it prints the number from `x8` and the address of the `svc`, 4 bytes
before `ELR_EL1`.

## Running it

```sh
$ build/mini-elfuse -v build/hello
load 0x400000-0x4000fa
entry 0x4000d4
mini-elfuse: unhandled syscall 64 at 0x4000e4
```

The program ran its first four instructions at EL0 and reached the `svc` at
`0x4000e4` with `x8` = 64, `write`:

```sh
$ aarch64-linux-gnu-objdump -d build/hello
...
  4000e0:	d2800808 	mov	x8, #0x40                  	// #64
  4000e4:	d4000001 	svc	#0x0
...
```

[Part 3](../03-page-tables/) adds page tables before handling the call.

## The code

The files are served with this site under `mini-elfuse/step-2/`:

```sh
mkdir step-2 && cd step-2
for f in Makefile mini-elfuse.c vectors.S entitlements.plist hello.S; do
    curl -fsSO https://henrybear327.github.io/notes/mini-elfuse/step-2/$f
done
make check
```

`Makefile`:

{{< include-file "static/mini-elfuse/step-2/Makefile" "make" >}}

`mini-elfuse.c`:

{{< include-file "static/mini-elfuse/step-2/mini-elfuse.c" "c" >}}

`vectors.S`:

{{< include-file "static/mini-elfuse/step-2/vectors.S" "asm" >}}

`entitlements.plist`:

{{< include-file "static/mini-elfuse/step-2/entitlements.plist" "xml" >}}

`hello.S`:

{{< include-file "static/mini-elfuse/step-2/hello.S" "asm" >}}

## What we learned

- A process needs the `com.apple.security.hypervisor` entitlement to create a
  VM; without it `hv_vm_create()` returns `HV_DENIED`.
- A new vCPU has `CPSR` 0, which is EL0, so it can start at the program's
  entry point directly.
- `svc` at EL0 enters the vector table at `VBAR_EL1 + 0x400`. An entry that is
  only `hvc #5` and `eret` changes no general-purpose register, so mini-elfuse
  sees them as the program left them.
- The `hvc` number arrives in the low 16 bits of the exit's syndrome, and the
  guest's `ESR_EL1` tells an `svc` from a fault.
