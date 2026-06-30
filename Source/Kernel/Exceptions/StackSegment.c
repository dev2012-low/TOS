#include <Kernel/Types.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

NOPTR StackSegmentHandler(NOPTR) {
    SysStop("STACK_SEGMENT_FAULT");
    
    for (;;) Halt();
}