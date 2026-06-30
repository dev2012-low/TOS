#include <Kernel/Types.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

NOPTR NmiHandler(NOPTR) {
    SysStop("NMI_HARDWARE_FAILURE");

    for (;;) Halt();
}