#include <Kernel/Scheduler.h>
#include <Memory/PhysAlloc.h>
#include <Kernel/Task.h>
#include <Kernel/Syscall.h>
#include <Asm/Cpu.h>
#include <Lib/String.h>

static UINT8 DfStack[4096] ATTRIBUTE(aligned(16));

struct GdtPtr {
    UINT16 Limit;
    UINT64 Base;
};

typedef struct {
    UINT32 Reserved0;
    UINT64 Rsp0;
    UINT64 Rsp1;
    UINT64 Rsp2;
    UINT64 Reserved1;
    UINT64 Ist1;
    UINT64 Ist2;
    UINT64 Ist3;
    UINT64 Ist4;
    UINT64 Ist5;
    UINT64 Ist6;
    UINT64 Ist7;
    UINT64 Reserved2;
    UINT16 Reserved3;
    UINT16 IoMapBase;
} ATTRIBUTE(packed) Tss64;

extern UINT8 TssBuffer[104];

static NOPTR GdtSetTss(UINT64 Base, UINT32 Limit) {
    extern UINT64 Gdt[];
    UINT64 Low;
    UINT64 High;

    Low = ((UINT64)Limit & 0xFFFF) |
          (((Base >> 0) & 0xFFFFFF) << 16) |
          (0x89ULL << 40) |
          (((UINT64)Limit & 0xF0000ULL) << 32);

    High = Base >> 32;

    Gdt[5] = Low;
    Gdt[6] = High;
}

NOPTR TssInit(KTask *InitialTask) {
    Tss64 *Tss = (Tss64*)&TssBuffer;
    extern struct GdtPtr GdtDesc;

    MemSet(Tss, 0, sizeof(Tss64));
    Tss->IoMapBase = sizeof(Tss64);  // I/O map отсутствует

    if (InitialTask) {
        UINT64 Rsp0 = InitialTask->KernelStackBase + InitialTask->KernelStackSize;
        Tss->Rsp0 = Rsp0;
        SyscallSetKernelRsp(Rsp0);
    }

    Tss->Ist1 = (UINT64)(UINTPTR)DfStack + 4096;

    // Устанавливаем TSS в GDT
    UINT64 Base = (UINT64)(UINTPTR)Tss;
    UINT32 Limit = sizeof(Tss64) - 1;
    
    extern UINT64 Gdt[];
    
    // Дескриптор TSS (0x28)
    Gdt[5] = (Limit & 0xFFFF) |
             ((Base & 0xFFFFFF) << 16) |
             (0x89ULL << 40) |        // Тип: 0x9 (64-bit TSS available)
             ((Limit & 0xF0000ULL) << 32);
    Gdt[6] = Base >> 32;
    
    LoadGDT((NOPTR*)&GdtDesc);
    asm volatile("ltr %w0" : : "r"((UINT16)0x28));
}

NOPTR TssSetRsp0(UINT64 Rsp0) {
    Tss64 *Tss = (Tss64*)&TssBuffer;
    Tss->Rsp0 = Rsp0;
    SyscallSetKernelRsp(Rsp0);
}
