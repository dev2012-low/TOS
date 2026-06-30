#include <Kernel/Types.h>
#include <Asm/Cpu.h>
#include <Asm/Io.h>
#include <Kernel/KDriver.h>
#include <Multiboot2Parser.h>
#include <Kernel/Paging.h>
#include <Kernel/Return.h>
#include <Memory/Allocator.h>
#include <Memory/PhysAlloc.h>
#include <FBDevice.h>
#include <RgbColor.h>
#include <Acpi.h>
#include <Kernel/SysStop.h>
#include <Console.h>
#include <Kernel/Kpmc.h>
#include <Apic.h>
#include <Ioapic.h>
#include <Kernel/Idt.h>
#include <Time/Timer.h>
#include <Time/Clock.h>
#include <Greetings.h>
#include <Ps2Keyboard.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Task.h>
#include <Pci.h>
#include <KTasks.h>
#include <Drive/Pata.h>
#include <Drive/Drive.h>
#include <Kernel/Syscall.h>
#include <Smbios.h>
#include <KF3D/KF3D.h>
#include <Fs/Vfs.h>
#include <Crypto/Rng.h>
#include <Crypto/SelfTest.h>
#include <Watchdog.h>
#include <Gdl/Gdl.h>
#include <Kernel/UserAccount.h>
#include <Decon.h>
#include <Audit.h>
#include <Kernel/Integrity.h>

INT LastCode = -8086; // Intel, if you're reading this, -8086 not for nothing ;)
EXTERN(UINTPTR, __stack_chk_guard);
Multiboot2Info MB;

#define OSVER "0.03"

INT AcpiInitialize(Multiboot2Info *Info) {
    UINT64 CurrentAcpiTable = 0;
    if (Info->Acpi.RsdpV1Addr) {
	CurrentAcpiTable = Info->Acpi.RsdpV1Addr;
    } else if (Info->Acpi.RsdpV2Addr) {
	CurrentAcpiTable = Info->Acpi.RsdpV2Addr;
    } else {
	SysStop("FAILED_TO_GET_ACPI_TABLE");
    }
    INT AcpiRet = AcpiInit(CurrentAcpiTable);
    RETURN(SUCCESS);
}

NOPTR ApicInitialize(NOPTR) {
    Outb(0x20, 0x11);   // ICW1: init, edge, cascade
    Outb(0xA0, 0x11);
    Outb(0x21, 0x20);   // ICW2: master vector offset (не используется)
    Outb(0xA1, 0x28);   // slave vector offset
    Outb(0x21, 0x04);   // ICW3: master has slave on IRQ2
    Outb(0xA1, 0x02);   // ICW3: slave is on IRQ2
    Outb(0x21, 0x01);   // ICW4: 8086 mode
    Outb(0xA1, 0x01);
    Outb(0x21, 0xFF);   // Маскируем всё
    Outb(0xA1, 0xFF);
    UINT32 LocalApicAddr = AcpiGetLocalApicAddr();
    if (!LocalApicAddr) {
		SysStop("NO_LOCAL_APIC");
    }
    if (ApicInit(LocalApicAddr) != SUCCESS) {
		SysStop("APIC_INIT_FAILED");
    }
    ApicEnable();
    if (IoapicInit() != SUCCESS) {
		SysStop("IOAPIC_INIT_FAILED");
    }
    IoapicProcessOverrides();
    Outb(0x21, 0xFF);
    Outb(0xA1, 0xFF);
}

NOPTR MemoryInit(UINT64 Multiboot2Addr) {
    EXTERN(CHAR, HeapStart);
    EXTERN(CHAR, HeapEnd);
    PhysAllocInit(PhysAllocGet(), Multiboot2Addr);
    PagingInit(Multiboot2ParserGetTotalMemory(Multiboot2ParserGet()));
    UINT64 SHeapStart = (UINT64)&HeapStart;
    UINT64 TotalMem = Multiboot2ParserGetTotalMemory(Multiboot2ParserGet());
    UINT64 Reserved = 1024 * 1024;
    UINT64 SHeapEnd = TotalMem - Reserved;
    USIZE SHeapSize = (USIZE)(SHeapEnd - SHeapStart);
    if (SHeapSize > 0 && SHeapSize < 512 * 1024 * 1024) {
	MemoryAllocatorInit((NOPTR*)SHeapStart, SHeapSize);
    } else {
	SHeapSize = (USIZE)((UINT64)&HeapEnd - (UINT64)&HeapStart);
	MemoryAllocatorInit((NOPTR*)&HeapStart, SHeapSize);
    }
}

// "HH:MM:SS" -> "HHMMSS"
NOPTR TimeToBuild(CHAR *Buffer, const CHAR *Time) {
    if (!Buffer || !Time) return;
    
    // Копируем часы
    Buffer[0] = Time[0];
    Buffer[1] = Time[1];
    // Пропускаем ':'
    Buffer[2] = Time[3];
    Buffer[3] = Time[4];
    // Пропускаем ':'
    Buffer[4] = Time[6];
    Buffer[5] = Time[7];
    
    Buffer[6] = '\0';
}

static NOPTR CryptoInit(NOPTR) {
    RngInit();

    CryptoSelfTest();
}

static void PrintHexDump(const UINT8 *Data, UINT32 Length, const CHAR *Title) {
    if (!Title) Title = "Hex dump";
    
    ConsolePrint("\n%s:\n", Title);
    
    for (UINT32 i = 0; i < Length; i++) {
        if (i % 16 == 0) {
            ConsolePrint("%08X  ", i);
        }
        
        ConsolePrint("%02X ", Data[i]);
        
        if ((i + 1) % 16 == 0) {
            ConsolePrint(" |");
            for (UINT32 j = i - 15; j <= i; j++) {
                CHAR c = (Data[j] >= 0x20 && Data[j] <= 0x7E) ? Data[j] : '.';
                ConsolePrint("%c", c);
            }
            ConsolePrint("|\n");
        }
    }
    
    // Print last line if not complete
    UINT32 Remaining = Length % 16;
    if (Remaining > 0) {
        for (UINT32 i = 0; i < (16 - Remaining); i++) {
            ConsolePrint("   ");
        }
        ConsolePrint(" |");
        UINT32 LastLineStart = Length - Remaining;
        for (UINT32 j = 0; j < Remaining; j++) {
            CHAR c = (Data[LastLineStart + j] >= 0x20 && Data[LastLineStart + j] <= 0x7E) ? Data[LastLineStart + j] : '.';
            ConsolePrint("%c", c);
        }
        ConsolePrint("|\n");
    }
}	

NOPTR KMain(UINT64 Multiboot2Addr) {
    __stack_chk_guard = __builtin_ia32_rdtsc() ^ 0xBAAAD00Du;
    KDriverInit();
    Multiboot2ParserInitInfo(&MB);
    Multiboot2ParserParse((NOPTR*)(UINTPTR)Multiboot2Addr, &MB);
    KpmcInit();
    MemoryInit(Multiboot2Addr);
    AcpiInitialize(Multiboot2ParserGet());
    SmbiosInit();
    FBDeviceInfo* Info = FBDeviceGetInfoFromMB2();
    FBDeviceInit(Info->Addr, Info->Width, Info->Height, Info->Pitch, Info->Bpp);
    GdlInit();
    ConsoleInit();
    DeconInit();
    AuditInit();
    CheckKernelIntegrity();
    KF3DInit();
    KpmcCleanAll();
    IdtInit();
    SyscallInit();
    ApicInitialize();
    InitSystemClock();
    TimerInit(1000);
    SchedulerInit();
    WatchdogInit();
    WatchdogStart(5000);
    Ps2KeyboardInit();
    ConsoleInputAttach();
    UserManagerInit();
    LocalInterruptsEnable();
    KTasksCreateAll();
    GreetingsPrint();
    CryptoInit();
    SchedulerStart();
}
