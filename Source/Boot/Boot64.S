# Boot64.S - 64-bit entry point
.code64
.section .text

.extern StackEnd
.extern KMain
.globl LongModeEntry

LongModeEntry:
    # Segment registers
    mov $0x20, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    
    # Stack
    lea StackEnd(%rip), %rsp
    and $-16, %rsp
    
    # Kernel call (rbx = multiboot info)
    mov %rbx, %rdi
    call KMain

Hang64:
    hlt
    jmp Hang64

.section .note.GNU-stack