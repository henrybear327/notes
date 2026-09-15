/* write(1, "hello\n", 6), then exit(0) */
    .text
    .globl _start
_start:
    mov x0, #1
    adr x1, msg
    mov x2, #6
    mov x8, #64         /* write */
    svc #0
    mov x0, #0
    mov x8, #93         /* exit */
    svc #0
msg:
    .ascii "hello\n"
