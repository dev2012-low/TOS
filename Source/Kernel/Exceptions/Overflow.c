#include <Kernel/Types.h>
#include <Kernel/Task.h>
#include <Kernel/Scheduler.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

NOPTR OverflowHandler(NOPTR) {
    KTask *Current = SchedulerGetCurrent();
    
    if (Current && Current->Pid > 0) {
        SchedulerKillTask(Current->Pid);
        SchedulerYield();
        return;
    }
    
    SysStop("OVERFLOW");
    for (;;) Halt();
}