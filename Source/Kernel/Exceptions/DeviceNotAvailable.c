#include <Kernel/Types.h>
#include <Kernel/SysStop.h>
#include <Asm/Cpu.h>

NOPTR DeviceNotAvailableHandler(NOPTR) {
    UINT64 Cr0 = ReadCr0();
    
    /* Включаем FPU, если выключен */
    if (Cr0 & ((1 << 2) | (1 << 5))) {
        Cr0 &= ~(1 << 2);
        Cr0 &= ~(1 << 5);
        WriteCr0(Cr0);
        return;
    }
    
    SysStop("DEVICE_NOT_AVAILABLE");
    for (;;) Halt();
}