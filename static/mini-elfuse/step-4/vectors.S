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
