#include <Kernel/Types.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

NOPTR DoubleFaultHandler(NOPTR) {
    
    /* Паника — восстановление невозможно */
    SysStop("DOUBLE_FAULT");
    
    for (;;) Halt();
}