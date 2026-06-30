[BITS 64]

global Ps2KeyboardIrq
extern Ps2KeyboardIrqHandler

Ps2KeyboardIrq:
    push rbp
    mov rbp, rsp          ; сохраняем указатель на RIP, CS, RFLAGS
    ; теперь RBP указывает на RIP, который лежит в стеке
    ; но это НЕ исходный RSP, а RSP-8
    
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov r12, rsp          ; сохраняем текущий RSP перед выравниванием
    
    and rsp, -16          ; выравнивание
    
    call Ps2KeyboardIrqHandler

    mov rsp, r12          ; восстанавливаем RSP до всех pop'ей
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    pop rbp
    
    iretq
