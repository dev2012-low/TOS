#include <Kernel/Types.h>
#include <Kernel/Task.h>
#include <Kernel/Scheduler.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

static BOOL IsUserMode(NOPTR *StackFrame) {
    UINT64 Cs = *(UINT64*)((UINT8*)StackFrame + 128 + 8);
    return (Cs & 3) == 3;
}

NOPTR SimdErrorHandler(NOPTR *StackFrame) {
    KTask *Current = SchedulerGetCurrent();
    UINT32 Mxcsr;
    BOOL FromUser = IsUserMode(StackFrame);
    
    /* Читаем MXCSR */
    asm volatile("stmxcsr %0" : "=m"(Mxcsr));
    
    if (FromUser && Current && Current->Pid > 0) {
        /* Сбрасываем ошибки в MXCSR (иначе они будут повторяться) */
        asm volatile("ldmxcsr %0" : : "m"(Mxcsr));
        
        /* Убиваем задачу */
        SchedulerKillTask(Current->Pid);
        SchedulerYield();
        return;
    }
    
    /* Ошибка в ядре — паника */
    SysStop("SIMD_ERROR");
    
    for (;;) Halt();
}