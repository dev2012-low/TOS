#include <Gdl/Gdl.h>
#include <Memory/Allocator.h>
#include <Memory/PhysAlloc.h>
#include <Lib/String.h>
#include <Kernel/Scheduler.h>
#include <Kernel/SpinLock.h>

static ListHead GdlGemObjects;
static UINT32 GdlNextGemHandle = 1;
static UINT32 GdlGemLock = 0;

static GdlGem *GdlGemFind(UINT32 Handle) {
    struct ListHead *Pos;
    ListForEach(Pos, &GdlGemObjects) {
        GdlGem *Gem = ListEntry(Pos, GdlGem, Node);
        if (Gem->Handle == Handle) {
            return Gem;
        }
    }
    return NULLPTR;
}

static INT GdlGemAllocPages(GdlGem *Gem) {
    UINT32 Pages = (Gem->Size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    if (Gem->Contiguous) {
        Gem->CpuAddr = (UINT64)PhysAllocAllocateRange(PhysAllocGet(), Pages);
        if (!Gem->CpuAddr) return GDL_ERR_NO_MEMORY;
        Gem->PhysAddr = Gem->CpuAddr;
    } else {
        // For non-contiguous, allocate pages individually
        Gem->Pages = MemoryAllocate(Pages * sizeof(NOPTR*));
        if (!Gem->Pages) return GDL_ERR_NO_MEMORY;
        
        for (UINT32 I = 0; I < Pages; I++) {
            NOPTR *Page = PhysAllocAllocatePage(PhysAllocGet());
            if (!Page) goto ErrorFree;
            ((NOPTR**)Gem->Pages)[I] = Page;
        }
        Gem->CpuAddr = (UINT64)(UINTPTR)((NOPTR**)Gem->Pages)[0];
        Gem->PhysAddr = Gem->CpuAddr;
    }
    
    Gem->PageCount = Pages;
    return GDL_OK;
    
ErrorFree:
    for (UINT32 J = 0; J < Pages; J++) {
        if (((NOPTR**)Gem->Pages)[J]) {
            PhysAllocFreePage(PhysAllocGet(), ((NOPTR**)Gem->Pages)[J]);
        }
    }
    MemoryFree(Gem->Pages);
    return GDL_ERR_NO_MEMORY;
}

INT GdlGemCreate(UINT64 Size, UINT32 *Handle) {
    return GdlGemCreateContiguous(Size, Handle);
}

INT GdlGemCreateContiguous(UINT64 Size, UINT32 *Handle) {
    if (Size == 0 || Size > 1024 * 1024 * 1024) return GDL_ERR_INVALID_PARAM;
    if (!Handle) return GDL_ERR_INVALID_PARAM;
    
    SpinLockAcquireUINT32(&GdlGemLock);
    
    GdlGem *Gem = (GdlGem*)MemoryAllocate(sizeof(GdlGem));
    if (!Gem) {
        SpinLockReleaseUINT32(&GdlGemLock);
        return GDL_ERR_NO_MEMORY;
    }
    
    MemSet(Gem, 0, sizeof(GdlGem));
    Gem->Size = Size;
    Gem->Handle = GdlNextGemHandle++;
    Gem->Refcount = 1;
    Gem->Contiguous = TRUE;
    
    INT Ret = GdlGemAllocPages(Gem);
    if (Ret != GDL_OK) {
        MemoryFree(Gem);
        SpinLockReleaseUINT32(&GdlGemLock);
        return Ret;
    }
    
    ListAddTail(&GdlGemObjects, &Gem->Node);
    *Handle = Gem->Handle;
    
    SpinLockReleaseUINT32(&GdlGemLock);
    return GDL_OK;
}

INT GdlGemDestroy(UINT32 Handle) {
    SpinLockAcquireUINT32(&GdlGemLock);
    
    GdlGem *Gem = GdlGemFind(Handle);
    if (!Gem) {
        SpinLockReleaseUINT32(&GdlGemLock);
        return GDL_ERR_NOT_FOUND;
    }
    
    Gem->Refcount--;
    if (Gem->Refcount == 0) {
        if (Gem->CpuAddr) {
            UINT32 Pages = Gem->PageCount;
            if (Gem->Contiguous) {
                PhysAllocFreeRange(PhysAllocGet(), (NOPTR*)(UINTPTR)Gem->CpuAddr, Pages);
            } else if (Gem->Pages) {
                for (UINT32 I = 0; I < Pages; I++) {
                    PhysAllocFreePage(PhysAllocGet(), ((NOPTR**)Gem->Pages)[I]);
                }
                MemoryFree(Gem->Pages);
            }
        }
        ListDel(&Gem->Node);
        MemoryFree(Gem);
    }
    
    SpinLockReleaseUINT32(&GdlGemLock);
    return GDL_OK;
}

INT GdlGemMap(UINT32 Handle, UINT64 *CpuAddr, UINT64 *PhysAddr) {
    SpinLockAcquireUINT32(&GdlGemLock);
    
    GdlGem *Gem = GdlGemFind(Handle);
    if (!Gem) {
        SpinLockReleaseUINT32(&GdlGemLock);
        return GDL_ERR_NOT_FOUND;
    }
    
    if (CpuAddr) *CpuAddr = Gem->CpuAddr;
    if (PhysAddr) *PhysAddr = Gem->PhysAddr;
    
    SpinLockReleaseUINT32(&GdlGemLock);
    return GDL_OK;
}

NOPTR GdlGemUnmap(UINT32 Handle) {
    (NOPTR)Handle;
}

INT GdlGemGetSize(UINT32 Handle, UINT64 *Size) {
    if (!Size) return GDL_ERR_INVALID_PARAM;
    
    SpinLockAcquireUINT32(&GdlGemLock);
    
    GdlGem *Gem = GdlGemFind(Handle);
    if (!Gem) {
        SpinLockReleaseUINT32(&GdlGemLock);
        return GDL_ERR_NOT_FOUND;
    }
    
    *Size = Gem->Size;
    SpinLockReleaseUINT32(&GdlGemLock);
    return GDL_OK;
}

NOPTR GdlGemInit(NOPTR) {
    ListInit(&GdlGemObjects);
    GdlNextGemHandle = 1;
    GdlGemLock = 0;
}

INT GdlGemBind(GdlGem *Gem, GdlDevice *Dev) {
    if (!Gem || !Dev) return GDL_ERR_INVALID_PARAM;
    if (Gem->Bound) return GDL_OK;  // уже привязан
    
    if (Dev->BindGem) {
        INT Ret = Dev->BindGem(Dev, Gem);
        if (Ret == GDL_OK) {
            Gem->Bound = 1;
        }
        return Ret;
    }
    
    return GDL_ERR_NOT_SUPPORTED;
}

INT GdlGemUnbind(GdlGem *Gem, GdlDevice *Dev) {
    if (!Gem || !Dev) return GDL_ERR_INVALID_PARAM;
    if (!Gem->Bound) return GDL_OK;
    
    if (Dev->UnbindGem) {
        INT Ret = Dev->UnbindGem(Dev, Gem);
        if (Ret == GDL_OK) {
            Gem->Bound = 0;
        }
        return Ret;
    }
    
    return GDL_ERR_NOT_SUPPORTED;
}

UINT64 GdlGemGetGpuAddr(GdlGem *Gem, GdlDevice *Dev) {
    if (!Gem || !Dev) return 0;
    if (!Gem->Bound) return 0;
    
    return Gem->GpuAddr;  // нужно добавить это поле в GdlGem
}