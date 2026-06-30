#include <Kernel/Types.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

NOPTR BoundHandler(NOPTR) {
    SysStop("BOUND_RANGE_EXCEEDED");
    for (;;) Halt();
}