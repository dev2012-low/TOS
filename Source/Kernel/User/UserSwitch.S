.code64
.section .text

.global ElfEnterUserMode
ElfEnterUserMode:
    /* Arguments:
     * RDI = EntryPoint
     * RSI = Rsp
     * RDX = ArgvPtr
     * RCX = CR3 (physical)
     */

    mov %rcx, %cr3

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    mov %rsi, %r8

    pushq $0x10
    pushq %r8
    pushfq
    pop %rax
    or $0x200, %rax
    push %rax
    pushq $0x18
    pushq %rdi

    mov %rdx, %rsi
    mov 8(%r8), %rdi

    iretq

    jmp .
