#include <Kernel/Microcode.h>
#include <Asm/Cpu.h>
#include <Asm/Io.h>
#include <Console.h>
#include <Kernel/Paging.h>
#include <Lib/String.h>
#include <Memory/PhysAlloc.h>

// MSR для Intel/VIA/Zhaoxin
#define MSR_IA32_UCODE_REV     0x8B
#define MSR_IA32_UCODE_WRITE   0x79

// MSR для AMD
#define MSR_AMD_PATCH_LEVEL    0xC0010022
#define MSR_AMD_PATCH_LOADER   0xC0010020

static UINT32 MicrocodeChecksum(UINT32 *Ptr, UINT32 SizeBytes) {
    UINT32 Sum = 0;
    UINT32 Count = SizeBytes / 4;
    
    for (UINT32 I = 0; I < Count; I++) {
        Sum += Ptr[I];
    }
    
    // Для Intel checksum должен быть 0
    return Sum;
}

// ==================== Vendor detection ====================

CpuVendor CpuDetectVendor(NOPTR) {
    UINT32 Eax, Ebx, Ecx, Edx;
    CHAR Vendor[13] = {0};
    
    Cpuid(0, &Eax, &Ebx, &Ecx, &Edx);
    
    *(UINT32*)Vendor = Ebx;
    *(UINT32*)(Vendor + 4) = Edx;
    *(UINT32*)(Vendor + 8) = Ecx;
    
    if (StrCmp(Vendor, "GenuineIntel") == 0) {
        return CPU_VENDOR_INTEL;
    } else if (StrCmp(Vendor, "AuthenticAMD") == 0) {
        return CPU_VENDOR_AMD;
    } else if (StrCmp(Vendor, "CentaurHauls") == 0) {
        return CPU_VENDOR_VIA;
    } else if (StrCmp(Vendor, "  Shanghai") == 0) {  // Zhaoxin имеет два пробела в начале!
        return CPU_VENDOR_ZHAOXIN;
    }
    
    return CPU_VENDOR_UNKNOWN;
}

CpuVendorInfo *CpuGetVendorInfo(NOPTR) {
    static CpuVendorInfo Info;
    CpuVendor Vendor = CpuDetectVendor();
    
    switch (Vendor) {
        case CPU_VENDOR_INTEL:
            Info.Vendor = Vendor;
            Info.Name = "Intel";
            Info.SupportsMicrocode = TRUE;
            Info.MsrMethod = "MSR 0x79 (Intel style)";
            break;
        case CPU_VENDOR_AMD:
            Info.Vendor = Vendor;
            Info.Name = "AMD";
            Info.SupportsMicrocode = TRUE;
            Info.MsrMethod = "MSR 0xC0010020 (physical address)";
            break;
        case CPU_VENDOR_VIA:
            Info.Vendor = Vendor;
            Info.Name = "VIA/Centaur";
            Info.SupportsMicrocode = TRUE;
            Info.MsrMethod = "MSR 0x79 (Intel compatible)";
            break;
        case CPU_VENDOR_ZHAOXIN:
            Info.Vendor = Vendor;
            Info.Name = "Zhaoxin";
            Info.SupportsMicrocode = TRUE;
            Info.MsrMethod = "MSR 0x79 (Intel compatible)";
            break;
        default:
            Info.Vendor = Vendor;
            Info.Name = "Unknown";
            Info.SupportsMicrocode = FALSE;
            Info.MsrMethod = "None";
            break;
    }
    
    return &Info;
}

INT CpuMicrocodeGetRevision(UINT32 *Revision) {
    if (!Revision) return -1;
    
    CpuVendor Vendor = CpuDetectVendor();
    
    switch (Vendor) {
        case CPU_VENDOR_INTEL:
        case CPU_VENDOR_VIA:
        case CPU_VENDOR_ZHAOXIN:
            // Intel-style: write 0 to MSR_IA32_UCODE_REV, then CPUID
            WriteMSR(MSR_IA32_UCODE_REV, 0);
            asm volatile("cpuid" : : "a"(1), "c"(0) : "ebx", "edx");
            *Revision = (UINT32)(ReadMSR(MSR_IA32_UCODE_REV) >> 32);
            return 0;
            
        case CPU_VENDOR_AMD:
            *Revision = (UINT32)(ReadMSR(MSR_AMD_PATCH_LEVEL) & 0xFFFFFFFF);
            return 0;
            
        default:
            return -1;
    }
}

// ==================== Intel-style loader (Intel/VIA/Zhaoxin) ====================

static INT IntelLoad(const UINT8 *Data, UINT32 Size, const CHAR *VendorName) {
    if (Size < sizeof(IntelMicrocodeHeader)) {
        ConsolePrint("%s: file too small (%u bytes)\n", VendorName, Size);
        return MICROCODE_ERR_SIZE;
    }
    
    IntelMicrocodeHeader *Hdr = (IntelMicrocodeHeader*)Data;
    
    UINT32 OldRev, NewRev;
    CpuMicrocodeGetRevision(&OldRev);
    
    // Get current CPU signature
    UINT32 Eax, Ebx, Ecx, Edx;
    Cpuid(1, &Eax, &Ebx, &Ecx, &Edx);
    UINT32 CpuSig = Eax;
    
    if (CpuSig != Hdr->ProcessorSignature && Hdr->ProcessorSignature != 0) {
        ConsolePrint("signature mismatch: CPU=0x%x, patch=0x%x\n",
                            CpuSig, Hdr->ProcessorSignature);
        // Продолжаем — некоторые патчи могут быть универсальными
    }
    
    // Verify checksum
    UINT32 TotalBytes = (Hdr->TotalSize == 0) ? (Hdr->DataSize + 48) : Hdr->TotalSize;
    if (TotalBytes > Size) TotalBytes = Size;
    
    UINT32 Sum = MicrocodeChecksum((UINT32*)Data, TotalBytes);
    if (Sum != 0) {
        ConsolePrint("%s: checksum error: 0x%x (should be 0)\n", VendorName, Sum);
        return MICROCODE_ERR_CHECKSUM;
    }
    
    // Load microcode
    UINT64 IntelPatchPtr = (UINT64)(UINTPTR)Data; 

    // Пишем этот АДРЕС в MSR (WriteMSR принимает 64-битное значение адреса)
    WriteMSR(MSR_IA32_UCODE_WRITE, (UINT64)IntelPatchPtr);

    // Один раз вызываем CPUID для сериализации
    asm volatile("cpuid" : : "a"(1) : "ebx", "ecx", "edx");
    
    CpuMicrocodeGetRevision(&NewRev);
    
    if (NewRev == Hdr->UpdateRevision || NewRev > OldRev) {
        return MICROCODE_OK;
    }
    
    ConsolePrint("%s: revision unchanged (0x%x)\n", VendorName, NewRev);
    return MICROCODE_ERR_MSR;
}

// ==================== AMD loader ====================

static INT AmdLoad(const UINT8 *Data, UINT32 Size) {
    if (Size < sizeof(AmdMicrocodeHeader)) {
        ConsolePrint("AMD: file too small (%u bytes)\n", Size);
        return MICROCODE_ERR_SIZE;
    }
    
    AmdMicrocodeHeader *Hdr = (AmdMicrocodeHeader*)Data;
    
    UINT32 OldRev, NewRev;
    CpuMicrocodeGetRevision(&OldRev);
    
    // AMD requires physically contiguous memory for the patch!
    UINT32 PagesNeeded = (Size + PAGE_SIZE - 1) / PAGE_SIZE;
    NOPTR *PhysBuf = PhysAllocAllocateRange(PhysAllocGet(), PagesNeeded);
    
    if (!PhysBuf) {
        ConsolePrint("AMD: failed to allocate %u contiguous pages\n", PagesNeeded);
        return MICROCODE_ERR_MSR;
    }
    
    // Copy patch to physically contiguous buffer
    MemCpy(PhysBuf, Data, Size);
    
    UINT64 PhysAddr = (UINT64)(UINTPTR)PhysBuf;
    
    // Verify checksum (AMD uses per-page checksum, but for simplicity check whole)
    UINT32 Sum = MicrocodeChecksum((UINT32*)PhysBuf, Size);
    if (Sum != 0) {
        ConsolePrint("AMD: checksum 0x%x (continuing anyway)\n", Sum);
    }
    
    // Load microcode by writing physical address to MSR
    WriteMSR(MSR_AMD_PATCH_LOADER, PhysAddr);
    
    // AMD recommends two CPUID instructions after load
    asm volatile("cpuid" : : "a"(1) : "ebx", "ecx", "edx");
    asm volatile("cpuid" : : "a"(1) : "ebx", "ecx", "edx");
    
    CpuMicrocodeGetRevision(&NewRev);
    
    // Free physically contiguous buffer
    PhysAllocFreeRange(PhysAllocGet(), PhysBuf, PagesNeeded);
    
    if (NewRev == Hdr->PatchLevel || NewRev > OldRev) {
        return MICROCODE_OK;
    }
    
    ConsolePrint("AMD: revision unchanged (0x%x)\n", NewRev);
    return MICROCODE_ERR_MSR;
}

// ==================== Main load function ====================

INT CpuMicrocodeLoad(const UINT8 *Data, UINT32 Size) {
    if (!Data || Size == 0) return MICROCODE_ERR_SIZE;
    
    CpuVendor Vendor = CpuDetectVendor();
    
    switch (Vendor) {
        case CPU_VENDOR_INTEL:
            return IntelLoad(Data, Size, "Intel");
        case CPU_VENDOR_VIA:
            return IntelLoad(Data, Size, "VIA");
        case CPU_VENDOR_ZHAOXIN:
            return IntelLoad(Data, Size, "Zhaoxin");
        case CPU_VENDOR_AMD:
            return AmdLoad(Data, Size);
        default:
            return MICROCODE_ERR_UNSUPPORTED;
    }
}
