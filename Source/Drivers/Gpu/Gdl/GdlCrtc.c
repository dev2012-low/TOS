#include <Gdl/Gdl.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>

GdlCrtc *GdlCrtcCreate(GdlDevice *Dev, UINT32 Index) {
    if (!Dev) return NULLPTR;
    
    GdlCrtc *Crtc = (GdlCrtc*)MemoryAllocate(sizeof(GdlCrtc));
    if (!Crtc) return NULLPTR;
    
    MemSet(Crtc, 0, sizeof(GdlCrtc));
    Crtc->Index = Index;
    Crtc->Dev = Dev;
    Crtc->Enabled = FALSE;
    Crtc->CursorVisible = FALSE;
    SnPrintf(Crtc->Name, sizeof(Crtc->Name), "Crtc%d", Index);
    
    ListInit(&Crtc->Node);
    ListAddTail(&Dev->Crtcs, &Crtc->Node);
    
    if (!Dev->PrimaryCrtc) {
        Dev->PrimaryCrtc = Crtc;
    }
    
    return Crtc;
}

INT GdlCrtcDestroy(GdlCrtc *Crtc) {
    if (!Crtc) return GDL_ERR_INVALID_PARAM;
    
    ListDel(&Crtc->Node);
    MemoryFree(Crtc);
    return GDL_OK;
}

INT GdlCrtcSetMode(GdlCrtc *Crtc, GdlMode *Mode) {
    if (!Crtc) return GDL_ERR_INVALID_PARAM;
    
    if (Crtc->SetMode) {
        INT Ret = Crtc->SetMode(Crtc, Mode);
        if (Ret == 0) {
            Crtc->Mode = Mode;
        }
        return Ret;
    }
    
    Crtc->Mode = Mode;
    return GDL_OK;
}

INT GdlCrtcSetFramebuffer(GdlCrtc *Crtc, GdlFramebuffer *Fb) {
    if (!Crtc) return GDL_ERR_INVALID_PARAM;
    
    if (Crtc->SetFb) {
        INT Ret = Crtc->SetFb(Crtc, Fb);
        if (Ret == 0) {
            Crtc->Fb = Fb;
        }
        return Ret;
    }
    
    Crtc->Fb = Fb;
    return GDL_OK;
}

INT GdlCrtcPageFlip(GdlCrtc *Crtc, GdlFramebuffer *Fb) {
    if (!Crtc || !Fb) return GDL_ERR_INVALID_PARAM;
    
    if (Crtc->PageFlip) {
        INT Ret = Crtc->PageFlip(Crtc, Fb);
        if (Ret == 0) {
            Crtc->Fb = Fb;
        }
        return Ret;
    }
    
    Crtc->Fb = Fb;
    return GDL_OK;
}

INT GdlCrtcSetCursor(GdlCrtc *Crtc, UINT32 X, UINT32 Y, UINT32 Handle, BOOL Show){
    if (!Crtc) return GDL_ERR_INVALID_PARAM;
    
    Crtc->CursorX = X;
    Crtc->CursorY = Y;
    Crtc->CursorHandle = Handle;
    Crtc->CursorVisible = Show;
    
    if (Crtc->SetCursor) {
        return Crtc->SetCursor(Crtc, X, Y, Handle);
    }
    
    return GDL_OK;
}

INT GdlCrtcEnable(GdlCrtc *Crtc) {
    if (!Crtc) return GDL_ERR_INVALID_PARAM;
    
    if (Crtc->Enable) {
        Crtc->Enable(Crtc);
    }
    Crtc->Enabled = TRUE;
    return GDL_OK;
}

INT GdlCrtcDisable(GdlCrtc *Crtc) {
    if (!Crtc) return GDL_ERR_INVALID_PARAM;
    
    if (Crtc->Disable) {
        Crtc->Disable(Crtc);
    }
    Crtc->Enabled = FALSE;
    return GDL_OK;
}

// Вызывать из IRQ обработчика VBlank
NOPTR GdlCrtcHandleVBlank(GdlCrtc *Crtc) {
    if (!Crtc) return;
    
    Crtc->VBlankCount++;
    
    if (Crtc->PendingFlip && Crtc->PendingFlip->Pending) {
        Crtc->PendingFlip->Pending = 0;
        
        if (Crtc->PendingFlip->Callback) {
            Crtc->PendingFlip->Callback(Crtc->PendingFlip->Data);
        }
        
        MemoryFree(Crtc->PendingFlip);
        Crtc->PendingFlip = NULLPTR;
    }
}