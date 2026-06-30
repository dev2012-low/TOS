#pragma once

#include <Kernel/Types.h>

#define KERNEL_VIRTUAL_BASE 0x0ULL
#define KERNEL_PHYSICAL_BASE 0x100000ULL

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_WRITE_THROUGH (1ULL << 3)
#define PTE_CACHE_DISABLE (1ULL << 4)
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_HUGE       (1ULL << 7)
#define PTE_GLOBAL     (1ULL << 8)
#define PTE_NO_EXEC    (1ULL << 63)

static inline UINT64 PhysToVirt(UINT64 Phys) {
    return Phys;  // Identity mapping
}

static inline UINT64 VirtToPhys(UINT64 Virt) {
    return Virt;  // Identity mapping
}

static inline NOPTR* PhysToVirtPtr(NOPTR *Phys) {
    return Phys;
}

static inline NOPTR* VirtToPhysPtr(NOPTR *Virt) {
    return Virt;
}

NOPTR PagingInit(UINT64 TotalPhysMem);
UINT64 *PagingGetKernelCR3(NOPTR);
NOPTR PagingSwitch(UINT64 *PML4);
UINT64 PagingLookupVirt(UINT64 *PML4, UINT64 VirtAddr);
NOPTR* PagingUserVirtToPtr(UINT64 *PML4, UINT64 VirtAddr);
UINT64 *PagingCreateUserTask(NOPTR *UserMem, USIZE UserSize);
NOPTR PagingDestroyUserTask(UINT64 *PML4);
INT PagingMapPage(UINT64 *PML4, UINT64 VirtAddr, UINT64 PhysAddr, UINT64 Flags);
INT PagingMapRange(UINT64 *PML4, UINT64 VirtStart, UINT64 PhysStart, USIZE Size, UINT64 Flags);
NOPTR PagingUnmapPage(UINT64 *PML4, UINT64 VirtAddr);
NOPTR PagingUnmapRange(UINT64 *PML4, UINT64 VirtStart, USIZE Size);
INT PagingMapUserRegion(UINT64 *PML4, NOPTR *Addr, USIZE Size);
INT PagingMapUserRegion(UINT64 *PML4, NOPTR *Addr, USIZE Size);
