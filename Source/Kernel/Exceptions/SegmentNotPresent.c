#include <Kernel/Types.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

NOPTR SegmentNotPresentHandler(NOPTR) {
    SysStop("SEGMENT_NOT_PRESENT");
    
    for (;;) Halt();
}