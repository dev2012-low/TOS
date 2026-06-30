#include <Kernel/Kpmc.h>
#include <Memory/PhysAlloc.h>
#include <Multiboot2Parser.h>
#include <FBDevice.h>
#include <Lib/String.h>
#include <Kernel/Types.h>
#include <Console.h>

static UINT64 GFreedPages = 0;
static UINT64 GFreedBytes = 0;

EXTERN(Multiboot2Info, MB);

static NOPTR KpmcAddFreed(UINT32 PageCount) {
    GFreedPages += PageCount;
    GFreedBytes += PageCount * PAGE_SIZE;
}

NOPTR KpmcClean(KpmcType Type) {
    PhysAlloc *PMM = PhysAllocGet();
    if (!PMM) return;
    
    switch (Type) {
        case KPMC_TYPE_BOOTLOADER:
            // Область загрузчика (0x0 - 0x100000), кроме первых 64KB (некоторые эмуляторы)
            // Освобождаем с 0x10000 по 0x100000
            PhysAllocFreeRange(PMM, (NOPTR*)0x10000, (0xF0000 / PAGE_SIZE));
            KpmcAddFreed(0xF0000 / PAGE_SIZE);
            break;
            
        case KPMC_TYPE_VGA:
            // VGA память (текстовый режим) 0xA0000 - 0xBFFFF
            PhysAllocFreeRange(PMM, (NOPTR*)0xA0000, (0x20000 / PAGE_SIZE));
            KpmcAddFreed(0x20000 / PAGE_SIZE);
            break;
            
        case KPMC_TYPE_BIOS_SHADOW:
            // BIOS Shadow 0xE0000 - 0xFFFFF
            PhysAllocFreeRange(PMM, (NOPTR*)0xE0000, (0x20000 / PAGE_SIZE));
            KpmcAddFreed(0x20000 / PAGE_SIZE);
            break;
            
        case KPMC_TYPE_MULTIBOOT2:
            // Multiboot2 структуры
            if (MB.LoadBaseAddr) {
                UINT32 Mb2Size = *(UINT32*)(UINTPTR)MB.LoadBaseAddr;
                UINT32 Pages = (Mb2Size + PAGE_SIZE - 1) / PAGE_SIZE;
                PhysAllocFreeRange(PMM, (NOPTR*)(UINTPTR)MB.LoadBaseAddr, Pages);
                KpmcAddFreed(Pages);
            }
            break;
            
        case KPMC_TYPE_ACPI_RECLAIM:
            // ACPI Reclaimable (тип 3)
            // Нужно пройти по всем регионам памяти из MB2
            if (MB.Mmap.Entries && MB.Mmap.EntryCount > 0) {
                for (UINT32 I = 0; I < MB.Mmap.EntryCount; I++) {
                    if (MB.Mmap.Entries[I].Type == MULTIBOOT2_MMAP_ACPI_RECLAIMABLE) {
                        UINT64 Start = MB.Mmap.Entries[I].Addr;
                        UINT64 End = Start + MB.Mmap.Entries[I].Len;
                        // Выравниваем
                        Start = (Start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                        End = End & ~(PAGE_SIZE - 1);
                        if (End > Start) {
                            UINT32 Pages = (End - Start) / PAGE_SIZE;
                            PhysAllocFreeRange(PMM, (NOPTR*)(UINTPTR)Start, Pages);
                            KpmcAddFreed(Pages);
                        }
                    }
                }
            }
            break;
            
        case KPMC_TYPE_UEFI:
            // UEFI временные структуры
            if (MB.Efi.SystemTable64) {
                // UEFI System Table (обычно 4KB)
                PhysAllocFreeRange(PMM, (NOPTR*)(UINTPTR)MB.Efi.SystemTable64, 1);
                KpmcAddFreed(1);
            }
            if (MB.Efi.Mmap && MB.Efi.MmapSize > 0) {
                UINT32 Pages = (MB.Efi.MmapSize + PAGE_SIZE - 1) / PAGE_SIZE;
                PhysAllocFreeRange(PMM, (NOPTR*)(UINTPTR)MB.Efi.Mmap, Pages);
                KpmcAddFreed(Pages);
            }
            break;
            
        case KPMC_TYPE_FRAMEBUFFER:
            // Если фреймбуфер больше не нужен (не рекомендуется, если ты его используешь)
            // Только если переключился на другой драйвер
            if (FBDeviceIsInitialized()) {
                FBDeviceInfo *Info = FBDeviceGetInfoFromMB2();
                if (Info && Info->Addr) {
                    UINT32 FbSize = Info->Height * Info->Pitch;
                    UINT32 Pages = (FbSize + PAGE_SIZE - 1) / PAGE_SIZE;
                    PhysAllocFreeRange(PMM, (NOPTR*)(UINTPTR)Info->Addr, Pages);
                    KpmcAddFreed(Pages);
                }
            }
            break;
            
        default:
            break;
    }
}

NOPTR KpmcCleanAll(NOPTR) {
    KpmcClean(KPMC_TYPE_BOOTLOADER);
    KpmcClean(KPMC_TYPE_VGA);
    KpmcClean(KPMC_TYPE_BIOS_SHADOW);
    KpmcClean(KPMC_TYPE_MULTIBOOT2);
    KpmcClean(KPMC_TYPE_ACPI_RECLAIM);
    KpmcClean(KPMC_TYPE_UEFI);
    // KpmcClean(KPMC_TYPE_FRAMEBUFFER); // DO NOT UNCOMMENT THIS!!!
}

NOPTR KpmcInit(NOPTR) {
    GFreedPages = 0;
    GFreedBytes = 0;
}

UINT64 KpmcGetFreedPages(NOPTR) {
    return GFreedPages;
}

UINT64 KpmcGetFreedBytes(NOPTR) {
    return GFreedBytes;
}