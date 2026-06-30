#include <Kernel/Types.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

NOPTR MachineCheckHandler(NOPTR) {
    SysStop("MACHINE_CHECK");
    
    for (;;) Halt();
}