#include <Kernel/Types.h>
#include <Kernel/Task.h>
#include <Kernel/Scheduler.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

/* Проверяем, юзермод ли это */
static BOOL IsUserMode(NOPTR *StackFrame) {
    UINT64 Cs = *(UINT64*)((UINT8*)StackFrame + 128 + 8);
    return (Cs & 3) == 3;
}

NOPTR FpuErrorHandler(NOPTR *StackFrame) {
    KTask *Current = SchedulerGetCurrent();
    UINT16 FpuStatus;
    BOOL FromUser = IsUserMode(StackFrame);
    
    /* Читаем FPU статус */
    asm volatile("fstsw %0" : "=m"(FpuStatus));
    
    /* Если ошибка в пользовательской задаче — убиваем её */
    if (FromUser && Current && Current->Pid > 0) {
        SchedulerKillTask(Current->Pid);
        SchedulerYield();
        return;
    }
    
    /* Ошибка в ядре — паника */
    SysStop("FPU_ERROR");
    
    for (;;) Halt();
}