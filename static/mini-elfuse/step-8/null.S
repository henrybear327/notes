/* Read address 0 into x0, then exit */
    .text
    .globl _start
_start:
    mov x0, #0
    ldr x0, [x0]
    mov x8, #93         /* exit */
    svc #0
