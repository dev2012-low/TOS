#pragma once

#include <Kernel/Types.h>

static inline NOPTR LocalInterruptsDisable(NOPTR) {
    asm volatile ("cli" : : : "memory");
}

static inline NOPTR LocalInterruptsEnable(NOPTR) {
    asm volatile ("sti" : : : "memory");
}

static inline NOPTR Halt(NOPTR) {
    asm volatile ("hlt");
}

static inline UINT64 ReadTimeStampCounter(NOPTR) {
    UINT32 Lo, Hi;
    asm volatile ("rdtsc" : "=a"(Lo), "=d"(Hi));
    return ((UINT64)Hi << 32) | Lo;
}

static inline UINT64 ReadCr0(NOPTR) {
    UINT64 Val;
    asm volatile("mov %%cr0, %0" : "=r"(Val));
    return Val;
}

static inline NOPTR WriteCr0(UINT64 Val) {
    asm volatile("mov %0, %%cr0" : : "r"(Val) : "memory");
}

static inline UINT64 ReadCr3(NOPTR) {
    UINT64 Val;
    asm volatile("mov %%cr3, %0" : "=r"(Val));
    return Val;
}

static inline NOPTR WriteCr3(UINT64 Val) {
    asm volatile("mov %0, %%cr3" : : "r"(Val) : "memory");
}

static inline UINT64 ReadCr4(NOPTR) {
    UINT64 Val;
    asm volatile("mov %%cr4, %0" : "=r"(Val));
    return Val;
}

static inline NOPTR WriteCr4(UINT64 Val) {
    asm volatile("mov %0, %%cr4" : : "r"(Val) : "memory");
}

static inline UINT64 ReadRflags(NOPTR) {
    UINT64 Flags;
    asm volatile("pushfq; popq %0" : "=g"(Flags));
    return Flags;
}

static inline NOPTR WriteRflags(UINT64 Flags) {
    asm volatile("pushq %0; popfq" : : "g"(Flags) : "memory", "cc");
}

static inline NOPTR InvalidateTLBPage(UINT64 Addr) {
    asm volatile("invlpg (%0)" : : "r"(Addr) : "memory");
}

static inline NOPTR LoadIDT(NOPTR* Ptr) {
    asm volatile("lidt (%0)" : : "r"(Ptr));
}

static inline NOPTR LoadGDT(NOPTR* Ptr) {
    asm volatile("lgdt (%0)" : : "r"(Ptr));
}

static inline UINT64 ReadMSR(UINT32 Msr) {
    UINT32 Lo, Hi;
    asm volatile("rdmsr" : "=a"(Lo), "=d"(Hi) : "c"(Msr));
    return ((UINT64)Hi << 32) | Lo;
}

static inline NOPTR WriteMSR(UINT32 Msr, UINT64 Val) {
    UINT32 Lo = (UINT32)Val;
    UINT32 Hi = (UINT32)(Val >> 32);
    asm volatile("wrmsr" : : "a"(Lo), "d"(Hi), "c"(Msr));
}

static inline NOPTR Cpuid(UINT32 Code, UINT32 *Eax, UINT32 *Ebx, UINT32 *Ecx, UINT32 *Edx) {
    asm volatile("cpuid"
                 : "=a"(*Eax), "=b"(*Ebx), "=c"(*Ecx), "=d"(*Edx)
                 : "a"(Code), "c"(0));
}

static inline NOPTR CpuidLeaf(UINT32 Code, UINT32 Subleaf, UINT32 *Eax, UINT32 *Ebx, UINT32 *Ecx, UINT32 *Edx) {
    asm volatile("cpuid"
                 : "=a"(*Eax), "=b"(*Ebx), "=c"(*Ecx), "=d"(*Edx)
                 : "a"(Code), "c"(Subleaf));
}

static inline NOPTR CpuPause(NOPTR) {
    asm volatile("pause");
}

static inline BOOL CheckInterruptStatus(NOPTR) {
    UINT64 Flags;
    asm volatile("pushfq; popq %0" : "=g"(Flags));
    return (Flags & 0x200) ? TRUE : FALSE;
}

static inline void AsmFlushCacheRange(NOPTR *Addr, UINTN Size) {
    UINT8 *P = (UINT8 *)Addr;
    while (Size > 0) {
        __builtin_ia32_clflush(P);
        P += 64;
        Size -= (Size >= 64) ? 64 : Size;
    }
    __builtin_ia32_sfence();
}

static inline UINTPTR SaveFlags(NOPTR) {
    UINTPTR Flags;
    asm volatile("pushfq; pop %0" : "=r"(Flags)::"memory");
    return Flags;
}

static inline NOPTR restore_flags(UINTPTR Flags) {
    asm volatile("push %0; popfq" ::"r"(Flags) : "memory", "cc");
}