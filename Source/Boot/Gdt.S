# Gdt.S - Global Descriptor Table
.section .data
.align 8

.globl Gdt
Gdt:
    .quad 0x0000000000000000     # Null
    .quad 0x00AF9A000000FFFF     # 0x08: Kernel Code
    .quad 0x00AFF2000000FFFF     # 0x10: User Data (SYSRET SS = STAR[63:48]+8)
    .quad 0x00AFFA000000FFFF     # 0x18: User Code (SYSRET CS = STAR[63:48]+16)
    .quad 0x00AF92000000FFFF     # 0x20: Kernel Data
    # 64-bit TSS (104 bytes)
    # Low 64 bits: base[15:0] | limit[15:0] | type | ...
    .quad 0x0000006800008968     # 0x28: TSS low (type=0x9, 64-bit TSS available)
    .quad 0x0000000000000000     # 0x30: TSS high (base[63:32])
GdtEnd:

.section .rodata
.align 8

.globl GdtDesc
GdtDesc:
    .word (GdtEnd - Gdt - 1)
    .quad Gdt

.section .note.GNU-stack
