.code64
.section .text

.extern SyscallDispatch
.extern GSyscallCpu

.global SyscallEntry

/*
 * SYSCALL entry (MSR_LSTAR). User state:
 *   RCX = return RIP, R11 = RFLAGS, RSP = user stack
 *   RAX = number, RDI RSI RDX R10 R8 R9 = args
 *
 * GSyscallCpu (MSR_KERNEL_GS_BASE, visible via swapgs):
 *   +0 UserRsp, +8 KernelRsp
 */
SyscallEntry:
    swapgs
    mov %rsp, %gs:0
    mov %gs:8, %rsp

    push %r11
    push %rcx

    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax

    mov %rsp, %rdi
    call SyscallDispatch
    mov %rax, %r12

    add $8, %rsp
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    mov %r12, %rax

    pop %rcx
    pop %r11

    mov %gs:0, %rsp
    swapgs
    sysretq

.section .note.GNU-stack
