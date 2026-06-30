#include <Kernel/Types.h>
#include <Kernel/Task.h>
#include <Kernel/Scheduler.h>
#include <Asm/Cpu.h>
#include <Kernel/SysStop.h>

NOPTR DivideErrorHandler(NOPTR) {
    KTask *Current = SchedulerGetCurrent();
    
    /* Если ошибка в пользовательской задаче — убиваем её */
    if (Current && Current->Pid > 0) {
        SchedulerKillTask(Current->Pid);
        SchedulerYield();
        return;
    }
    
    /* Если ошибка в ядре — критическая паника */
    SysStop("DIVIDE_ERROR");
    
    for (;;) Halt();
}