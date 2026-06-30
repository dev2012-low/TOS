#include <Gdl/Gdl.h>
#include <Memory/Allocator.h>
#include <Memory/PhysAlloc.h>
#include <Lib/String.h>

static UINT32 GdlNextFbHandle = 1;

static UINT32 GdlFormatToBpp(UINT32 Format) {
    switch (Format) {
        case GDL_FORMAT_RGB565:     return 16;
        case GDL_FORMAT_RGB888:     return 24;
        case GDL_FORMAT_XRGB8888:
        case GDL_FORMAT_ARGB8888:   return 32;
        default:                    return 32;
    }
}

GdlFramebuffer *GdlFramebufferCreate(UINT32 Width, UINT32 Height, UINT32 Format) {
    if (Width == 0 || Height == 0) return NULLPTR;
    
    GdlFramebuffer *Fb = (GdlFramebuffer*)MemoryAllocate(sizeof(GdlFramebuffer));
    if (!Fb) return NULLPTR;
    
    MemSet(Fb, 0, sizeof(GdlFramebuffer));
    Fb->Width = Width;
    Fb->Height = Height;
    Fb->Format = Format;
    Fb->BitsPerPixel = GdlFormatToBpp(Format);
    Fb->Pitch = Width * (Fb->BitsPerPixel / 8);
    Fb->Size = Fb->Pitch * Height;
    Fb->IsDirty = TRUE;
    Fb->Handle = GdlNextFbHandle++;
    
    Fb->VAddr = (UINT8*)MemoryAllocate(Fb->Size);
    if (!Fb->VAddr) {
        MemoryFree(Fb);
        return NULLPTR;
    }
    MemSet(Fb->VAddr, 0, Fb->Size);
    Fb->PAddr = (UINT64)(UINTPTR)Fb->VAddr;
    
    return Fb;
}

INT GdlFramebufferDestroy(GdlFramebuffer *Fb) {
    if (!Fb) return GDL_ERR_INVALID_PARAM;
    
    if (Fb->VAddr) {
        MemoryFree(Fb->VAddr);
    }
    MemoryFree(Fb);
    return GDL_OK;
}

INT GdlFramebufferMap(GdlFramebuffer *Fb) {
    if (!Fb) return GDL_ERR_INVALID_PARAM;
    // Already have vaddr from allocation
    return Fb->VAddr ? GDL_OK : GDL_ERR_NOT_FOUND;
}

NOPTR GdlFramebufferUnmap(GdlFramebuffer *Fb) {
    // Nothing to do - memory is always mapped
    (NOPTR)Fb;
}

INT GdlFramebufferFlush(GdlFramebuffer *Fb) {
    if (!Fb) return GDL_ERR_INVALID_PARAM;
    
    GdlDevice *Dev = GdlGetPrimaryDevice();
    if (Dev && Dev->Flush) {
        Dev->Flush(Dev, Fb);
    }
    
    Fb->IsDirty = FALSE;
    return GDL_OK;
}
