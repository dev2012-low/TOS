bits 64
section .text
global _start
_start:
    mov rdi, 1
    lea rsi, [msg]
    mov rdx, 13
    mov rax, 1
    syscall
    xor rdi, rdi
    mov rax, 0
    syscall
section .data
msg: db "Hello, TOS!", 10
