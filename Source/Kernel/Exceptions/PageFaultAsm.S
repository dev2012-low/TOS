.code64
.section .text

.extern PageFaultHandler

.global PageFaultStub
PageFaultStub:
    /* Сначала сохраняем все регистры (как в других ISR) */
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

    /* CR2 содержит адрес, вызвавший page fault */
    mov %cr2, %rdi

    /* Error code лежит на стеке выше сохранённых регистров */
    /* Сейчас стек: [R15..RAX][Error Code][RIP][CS][RFLAGS][RSP][SS] */
    /* Error code находится по смещению 8*16 = 128 байт от текущего RSP */
    mov 128(%rsp), %rsi

    /* Указатель на стек (нужен для отладки) */
    mov %rsp, %rdx

    /* Вызываем C-обработчик */
    call PageFaultHandler

    /* Восстанавливаем регистры */
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

    /* Убираем error code со стека (4 байта, но выравнивание до 8) */
    add $8, %rsp

    iretq

.section .note.GNU-stack