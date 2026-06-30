.code64
.section .text

.global TimerIsr
.extern TimerHandler

TimerIsr:
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    
    pushq $0           # err_code
    pushq $32          # int_no

    call TimerHandler
    /* TimerHandler may context-switch via iretq and not return */

    add $16, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax

    iretq

.section .note.GNU-stack