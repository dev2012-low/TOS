#pragma once

#include <Kernel/Types.h>
#include <Lib/String.h>

#define HEAP_CANARY_MAGIC 0xDEADBEEF

typedef struct {
    UINT32 CanaryValue;
    BOOL Initialized;
} HeapCanary;

/*
 * Memory allocator statistics structure
 */
typedef struct {
    USIZE TotalManaged;   // Total bytes managed (payload + headers)
    USIZE UsedPayload;    // Bytes currently used by allocations
    USIZE FreePayload;    // Bytes free for allocation
    USIZE LargestFree;    // Largest contiguous free block
    USIZE NumBlocks;      // Total number of blocks (used + free)
    USIZE NumUsed;        // Number of used blocks
    USIZE NumFree;        // Number of free blocks
} KMemoryStats;

// Initialization
INT MemoryAllocatorInit(NOPTR *HeapStart, USIZE HeapSize);

// Standard allocation
NOPTR *MemoryAllocate(USIZE Size);
NOPTR MemoryFree(NOPTR *Ptr);
NOPTR *MemoryReallocate(NOPTR *Ptr, USIZE NewSize);
NOPTR *MemoryCallocate(USIZE NMemB, USIZE Size);

// Statistics
NOPTR GetKMemoryStats(KMemoryStats *Stats);

// Aligned allocation
NOPTR *MemoryAllocateAligned(USIZE Size, USIZE Alignment);
NOPTR MemoryFreeAligned(NOPTR *Ptr);