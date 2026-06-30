#include <Kernel/Types.h>
#include <Kernel/Task.h>
#include <Kernel/Scheduler.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

NOPTR GeneralProtectionHandler(NOPTR *StackFrame) {
    KTask *Current = SchedulerGetCurrent();
    
    /* CS лежит на стеке после 16 сохранённых регистров (16 * 8 = 128 байт) */
    UINT64 Cs = *(UINT64*)((UINT8*)StackFrame + 128 + 8);  /* +8 для error code */
    BOOL FromUser = (Cs & 3) == 3;
    
    if (FromUser && Current && Current->Pid > 0) {
        /* Пользовательский режим — убиваем задачу */
        SchedulerKillTask(Current->Pid);
        SchedulerYield();
        return;
    }
    
    /* Ядро — паника */
    SysStop("GENERAL_PROTECTION");
    
    for (;;) Halt();
}