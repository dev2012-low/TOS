#include <Memory/Allocator.h>
#include <Memory/PhysAlloc.h>
#include <Kernel/Types.h>
#include <Lib/String.h>
#include <Kernel/Return.h>
#include <Kernel/Paging.h>
#include <Kernel/SysStop.h>
#include <Audit.h>
#include <Crypto/Rng.h>

/*
 * Configuration
 */
#define ALIGN 8
#define MAGIC 0xB16B00B5U
#define SIZE_MAX 18446744073709551615U
#define HEAP_MAX_SIZE (512 * 1024 * 1024)  //512 MB maximum

static UINT32 GHeapCanary = 0;
static BOOL GCanaryInitialized = FALSE;

/*
 * Block header
 */
typedef struct BlockHeader
{
    UINT32 Magic;
    UINT32 Canary;
    USIZE Size;
    INT Free;
    struct BlockHeader *Prev;
    struct BlockHeader *Next;
} BlockHeader;

#define MIN_SPLIT_SIZE (sizeof(BlockHeader) + ALIGN)

/*
 * Global
 */
static BlockHeader *HeapHead = NULLPTR;
static BlockHeader *HeapTail = NULLPTR;
static NOPTR *HeapStartVirt = NULLPTR;
static NOPTR *HeapEndVirt = NULLPTR;
static USIZE HeapCurrentSize = 0;

EXTERN(CHAR, HeapStart);
EXTERN(CHAR, HeapEnd);

static inline USIZE AlignUp(USIZE N) {
    return (N + (ALIGN - 1)) & ~(ALIGN - 1);
}

static inline NOPTR *HeaderToPayload(BlockHeader *H) {
    return (NOPTR *)((CHAR *)H + sizeof(BlockHeader));
}

static inline BlockHeader *PayloadToHeader(NOPTR *P) {
    return (BlockHeader *)((CHAR *)P - sizeof(BlockHeader));
}

static NOPTR HeapCanaryInit(NOPTR) {
    if (GCanaryInitialized) return;
    
    // Генерируем случайную канарейку
    RngGetRandomBytes((UINT8*)&GHeapCanary, sizeof(GHeapCanary));
    
    // Гарантируем, что канарейка не равна MAGIC
    if (GHeapCanary == MAGIC) {
        GHeapCanary ^= 0xDEADBEEF;
    }

    if (GHeapCanary == 0) {
        GHeapCanary = 0xB16B00B5;
    }
    
    GCanaryInitialized = TRUE;
}

static inline UINT32 HeapGetCanary(NOPTR) {
    if (!GCanaryInitialized) {
        HeapCanaryInit();
    }
    return GHeapCanary;
}

/*
 * Expand heap using PMM
 */
static INT HeapExpand(USIZE Bytes) {
    USIZE Need = AlignUp(Bytes + sizeof(BlockHeader));
    USIZE PagesNeeded = (Need + PAGE_SIZE - 1) / PAGE_SIZE;
    USIZE ExpandSize = PagesNeeded * PAGE_SIZE;
    
    if (HeapCurrentSize + ExpandSize > HEAP_MAX_SIZE) {
        return 0;
    }
    
    // Allocate physical pages
    NOPTR *Phys = PhysAllocAllocateRange(PhysAllocGet(), PagesNeeded);
    if (!Phys) {
        return 0;
    }
    
    // Identity mapping (virtual = physical)
    NOPTR *Virt = (NOPTR*)HeapEndVirt;
    
    // Map pages
    for (USIZE I = 0; I < PagesNeeded; I++) {
        UINT64 PhysAddr = (UINT64)Phys + I * PAGE_SIZE;
        UINT64 VirtAddr = (UINT64)Virt + I * PAGE_SIZE;
        
        // Map page
        PagingMapPage(PagingGetKernelCR3(), VirtAddr, PhysAddr, 
                        PTE_PRESENT | PTE_WRITABLE);
    }
    
    // Create header for new block
    BlockHeader *H = (BlockHeader *)Virt;
    H->Magic = MAGIC;
    H->Canary = HeapGetCanary();
    H->Free = 1;
    H->Size = ExpandSize - sizeof(BlockHeader);
    H->Prev = HeapTail;
    H->Next = NULLPTR;
    
    if (HeapTail) {
        HeapTail->Next = H;
    } else {
        HeapHead = H;
    }
    HeapTail = H;
    
    HeapEndVirt = (NOPTR*)((UINTPTR)HeapEndVirt + ExpandSize);
    HeapCurrentSize += ExpandSize;
    
    return 1;
}

/*
 * Initialize heap
 */
INT MemoryAllocatorInit(NOPTR *HeapStart, USIZE HeapSize) {
    if (!HeapStart)
        RETURN(NO_OBJECT);
    if (HeapSize < sizeof(BlockHeader))
	RETURN(INCORRECT_VALUE);
    
    HeapStartVirt = HeapStart;
    HeapEndVirt = (NOPTR*)((UINTPTR)HeapStart + HeapSize);
    HeapCurrentSize = HeapSize;

    HeapCanaryInit();
    
    HeapHead = (BlockHeader *)HeapStart;
    HeapHead->Magic = MAGIC;
    HeapHead->Canary = HeapGetCanary();
    HeapHead->Size = HeapSize - sizeof(BlockHeader);
    HeapHead->Free = 1;
    HeapHead->Prev = HeapHead->Next = NULLPTR;
    
    HeapTail = HeapHead;
    
    RETURN(SUCCESS);
}

/*
 * Find free block
 */
static BlockHeader *FindFit(USIZE Size) {
    BlockHeader *Cur = HeapHead;
    while (Cur) {
        if (Cur->Free && Cur->Size >= Size)
            return Cur;
        Cur = Cur->Next;
    }
    return NULLPTR;
}

/*
 * Split block
 */
static NOPTR SplitBlock(BlockHeader *H, USIZE ReqSize) {
    if (!H) return;
    if (H->Size < ReqSize + MIN_SPLIT_SIZE) return;
    
    CHAR *NewHdrAddr = (CHAR *)HeaderToPayload(H) + ReqSize;
    BlockHeader *NewH = (BlockHeader *)NewHdrAddr;
    NewH->Magic = MAGIC;
    NewH->Canary = HeapGetCanary();
    NewH->Free = 1;
    NewH->Size = H->Size - ReqSize - sizeof(BlockHeader);
    NewH->Prev = H;
    NewH->Next = H->Next;
    if (NewH->Next) NewH->Next->Prev = NewH;
    H->Next = NewH;
    H->Size = ReqSize;
    if (HeapTail == H) HeapTail = NewH;
}

/*
 * Coalesce adjacent free blocks
 */
static NOPTR Coalesce(BlockHeader *H)
{
    if (!H) return;
    
    if (H->Next && H->Next->Free) {
        BlockHeader *N = H->Next;
        H->Size = H->Size + sizeof(BlockHeader) + N->Size;
        H->Next = N->Next;
        if (N->Next) N->Next->Prev = H;
        if (HeapTail == N) HeapTail = H;
    }
    
    if (H->Prev && H->Prev->Free) {
        BlockHeader *P = H->Prev;
        P->Size = P->Size + sizeof(BlockHeader) + H->Size;
        P->Next = H->Next;
        if (H->Next) H->Next->Prev = P;
        if (HeapTail == H) HeapTail = P;
    }
}

/*
 * malloc
 */
NOPTR *MemoryAllocate(USIZE Size) {
    if (Size == 0) return NULLPTR;
    Size = AlignUp(Size);
    
    BlockHeader *Fit = FindFit(Size);
    
    while (!Fit) {
        if (!HeapExpand(Size)) {
            return NULLPTR;
        }
        Fit = FindFit(Size);
    }
    
    SplitBlock(Fit, Size);
    Fit->Free = 0;
    Fit->Canary = HeapGetCanary();
    return HeaderToPayload(Fit);
}

/*
 * free
 */
NOPTR MemoryFree(NOPTR *Ptr) {
    if (!Ptr) return;
    
    BlockHeader *H = PayloadToHeader(Ptr);
    
    if (H->Magic != MAGIC)
        SysStop("HEAP_CORRUPTION_INVALID_MAGIC");

    if (H->Canary != HeapGetCanary()) {
        AuditLog(AUDIT_LEVEL_CRITICAL, AUDIT_EVENT_COMMAND,
                 "Heap canary corrupted at 0x%p (expected 0x%08X, got 0x%08X)",
                 Ptr, HeapGetCanary(), H->Canary);
        SysStop("HEAP_CANARY_CORRUPTED");
    }
    
    if (H->Free) {
        AuditLog(AUDIT_LEVEL_WARNING, AUDIT_EVENT_COMMAND,
                 "Double free detected: 0x%p (size=%u)", 
                 Ptr, (UINT32)H->Size);
        return;
    }
    
    // <-- ЗАТИРАЕМ ПАМЯТЬ ПЕРЕД ОСВОБОЖДЕНИЕМ!
    USIZE Size = H->Size;
    SecureMemZero(Ptr, Size);
    
    H->Free = 1;
    Coalesce(H);
}

/*
 * realloc
 */
NOPTR *MemoryReallocate(NOPTR *Ptr, USIZE NewSize) {
    if (!Ptr) return MemoryAllocate(NewSize);
    if (NewSize == 0) {
        MemoryFree(Ptr);
        return NULLPTR;
    }
    
    BlockHeader *H = PayloadToHeader(Ptr);
    if (H->Magic != MAGIC) return NULLPTR;
    
    NewSize = AlignUp(NewSize);
    if (NewSize <= H->Size) {
        SplitBlock(H, NewSize);
        return Ptr;
    }
    
    NOPTR *NewP = MemoryAllocate(NewSize);
    if (!NewP) return NULLPTR;
    
    MemCpy(NewP, Ptr, H->Size);
    
    // <-- ЗАТИРАЕМ СТАРУЮ ПАМЯТЬ!
    SecureMemZero(Ptr, H->Size);
    
    MemoryFree(Ptr);
    return NewP;
}

/*
 * calloc
 */
NOPTR *MemoryCallocate(USIZE NMemB, USIZE Size) {
    if (NMemB != 0 && Size > SIZE_MAX / NMemB) {
        return NULLPTR;
    }
    
    USIZE TotalSize = NMemB * Size;
    if (TotalSize == 0) return NULLPTR;
    
    NOPTR *Ptr = MemoryAllocate(TotalSize);
    if (!Ptr) return NULLPTR;
    
    MemSet(Ptr, 0, TotalSize);
    return Ptr;
}

/*
 * Statistics
 */
NOPTR GetKMemoryStats(KMemoryStats *Stats) {
    if (!Stats) return;
    
    Stats->TotalManaged = 0;
    Stats->UsedPayload = 0;
    Stats->FreePayload = 0;
    Stats->LargestFree = 0;
    Stats->NumBlocks = Stats->NumUsed = Stats->NumFree = 0;
    
    BlockHeader *Cur = HeapHead;
    while (Cur) {
        Stats->NumBlocks++;
        Stats->TotalManaged += sizeof(BlockHeader) + Cur->Size;
        if (Cur->Free) {
            Stats->NumFree++;
            Stats->FreePayload += Cur->Size;
            if (Cur->Size > Stats->LargestFree)
                Stats->LargestFree = Cur->Size;
        }
        else {
            Stats->NumUsed++;
            Stats->UsedPayload += Cur->Size;
        }
        Cur = Cur->Next;
    }
}

/*
 * Aligned allocation
 */
NOPTR *MemoryAllocateAligned(USIZE Size, USIZE Alignment) {
    if (Alignment < ALIGN) Alignment = ALIGN;
    if ((Alignment & (Alignment - 1)) != 0) return NULLPTR;
    
    USIZE TotalSize = Size + Alignment - 1 + sizeof(NOPTR*);
    NOPTR* OriginalPtr = MemoryAllocate(TotalSize);
    if (!OriginalPtr) return NULLPTR;
    
    UINTPTR Addr = (UINTPTR)OriginalPtr + sizeof(NOPTR*);
    UINTPTR AlignedAddr = (Addr + Alignment - 1) & ~(Alignment - 1);
    
    NOPTR** PtrStore = (NOPTR**)(AlignedAddr - sizeof(NOPTR*));
    *PtrStore = OriginalPtr;
    
    return (NOPTR*)AlignedAddr;
}

NOPTR MemoryFreeAligned(NOPTR *Ptr) {
    if (!Ptr) return;
    
    NOPTR** PtrStore = (NOPTR**)((UINTPTR)Ptr - sizeof(NOPTR*));
    NOPTR* OriginalPtr = *PtrStore;
    
    // Получаем размер блока
    BlockHeader *H = PayloadToHeader(OriginalPtr);
    if (H->Magic == MAGIC && !H->Free) {
        SecureMemZero(Ptr, H->Size);  // <-- ЗАТИРАЕМ!
    }
    
    MemoryFree(OriginalPtr);
}