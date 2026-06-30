#include <Kernel/Types.h>
#include <Kernel/Task.h>
#include <Kernel/Scheduler.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

NOPTR InvalidTssHandler(NOPTR *StackFrame) {
    KTask *Current = SchedulerGetCurrent();
    
    /* Определяем, откуда пришло исключение */
    UINT64 Cs = *(UINT64*)((UINT8*)StackFrame + 112);  /* CS из стека */
    BOOL FromUser = (Cs & 3) == 3;  /* Ring 3 = пользователь */
    
    if (FromUser && Current && Current->Pid > 0) {
        SchedulerKillTask(Current->Pid);
        SchedulerYield();
        return;
    }
    
    SysStop("INVALID_TSS");
    
    for (;;) Halt();
}