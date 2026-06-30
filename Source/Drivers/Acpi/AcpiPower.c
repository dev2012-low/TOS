#include <Acpi.h>
#include <Asm/Io.h>
#include <Asm/Cpu.h>

EXTERN(Acpi, GAcpi);

NOPTR AcpiReboot(NOPTR) {
    if (!GAcpi.Fadt) {
        Outb(0x64, 0xFE);
        return;
    }
    
    UINT8 *ResetReg = GAcpi.Fadt->ResetReg;
    
    if (ResetReg[0] == 0x01) {
        UINT16 Port = *(UINT16*)(ResetReg + 4);
        UINT8 Value = GAcpi.Fadt->ResetValue;
        Outb(Port, Value);
    } else if (ResetReg[0] == 0x02) {
        UINT64 Addr = *(UINT64*)(ResetReg + 4);
        UINT8 Value = GAcpi.Fadt->ResetValue;
        *(volatile UINT8*)(UINTPTR)Addr = Value;
    } else {
        Outb(0x64, 0xFE);
    }
    
    while(1) {
        Halt();
    }
}

NOPTR AcpiShutdown(NOPTR) {
    if (!GAcpi.Fadt) {
        return;
    }
    
    UINT16 Pm1aPort = GAcpi.Fadt->Pm1aCntBlk;
    if (!Pm1aPort) {
        return;
    }
    
    UINT16 Value = (5 << 10) | (1 << 13);
    Outw(Pm1aPort, Value);
    
    UINT16 Pm1bPort = GAcpi.Fadt->Pm1bCntBlk;
    if (Pm1bPort) {
        Outw(Pm1bPort, Value);
    }
    
    while(1) {
        Halt();
    }
}

NOPTR AcpiSleep(UINT8 SleepType) {
    if (!GAcpi.Fadt) return;
    
    UINT16 Pm1aPort = GAcpi.Fadt->Pm1aCntBlk;
    if (!Pm1aPort) return;
    
    UINT16 Value = (SleepType << 10) | (1 << 13);
    Outw(Pm1aPort, Value);
    
    UINT16 Pm1bPort = GAcpi.Fadt->Pm1bCntBlk;
    if (Pm1bPort) {
        Outw(Pm1bPort, Value);
    }
}