#include <Kernel/Types.h>

NOPTR BreakpointHandler(NOPTR *StackFrame) {
    /* Увеличиваем RIP на 1 (пропускаем int3) */
    /* RIP хранится на стеке после сохранённых регистров */
    UINT64 *Rip = (UINT64*)((UINT8*)StackFrame + 8 * 16); /* 16 регистров по 8 байт */
    *Rip += 1;
    
    return;
}