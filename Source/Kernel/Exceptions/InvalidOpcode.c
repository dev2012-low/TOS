#include <Kernel/Types.h>
#include <Kernel/Task.h>
#include <Kernel/Scheduler.h>
#include <Asm/Cpu.h>
#include <Kernel/SysStop.h>

/* Основной обработчик #UD */
NOPTR InvalidOpcodeHandler(NOPTR) {
    KTask *Current = SchedulerGetCurrent();
    
    /* Если в юзермоде — убиваем задачу */
    if (Current && Current->Pid > 0) {
        SchedulerKillTask(Current->Pid);
        SchedulerYield();
        return;
    }

    SysStop("INVALID_OPCODE");
    
    for (;;) Halt();
}