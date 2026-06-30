.section .text
.global _start
_start:
    mov %rsp, %rbp
    and $-16, %rsp
    call main
    mov %rax, %rdi
    mov $0, %rax
    syscall
1:  pause
    jmp 1b
