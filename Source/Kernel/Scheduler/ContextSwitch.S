.code64
.section .text

/*
 * void SchedulerSwitchContext(UINT64 *SaveStackPointer, UINT64 NewStackPointer,
 *                             void *SaveAvxState, void *RestoreAvxState);
 *
 * Сохраняет и восстанавливает:
 *   - обычные регистры (RBP, RBX, R12-R15, RFLAGS)
 *   - AVX/AVX2 регистры (YMM0-YMM15) через XSAVE/XRSTOR
 */

.global SchedulerSwitchContext
SchedulerSwitchContext:
    /* Сохраняем обычные регистры */
    pushfq
    push %r15
    push %r14
    push %r13
    push %r12
    push %rbx
    push %rbp

    /* Сохраняем указатель стека */
    mov %rsp, (%rdi)

    /* Проверяем, нужно ли сохранять AVX состояние */
    cmp $0, %rdx
    je 1f
    
    /* Сохраняем YMM регистры через XSAVE */
    mov %rdx, %r8           /* сохраняем указатель на AvxState */
    mov $0xFFFFFFFF, %rax
    mov $0, %rdx
    xsave (%r8)             /* используем r8 вместо rdi */
1:

    /* Восстанавливаем новый стек */
    mov %rsi, %rsp

    /* Проверяем, нужно ли восстанавливать AVX состояние */
    cmp $0, %rcx
    je 2f
    
    /* Восстанавливаем YMM регистры через XRSTOR */
    mov %rcx, %r8
    mov $0xFFFFFFFF, %rax
    mov $0, %rdx
    xrstor (%r8)
2:

    /* Восстанавливаем обычные регистры */
    pop %rbp
    pop %rbx
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    popfq
    ret

.section .note.GNU-stack