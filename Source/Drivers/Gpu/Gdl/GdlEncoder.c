#include <Gdl/Gdl.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>

GdlEncoder *GdsEncoderCreate(GdlDevice *Dev, UINT32 Type, UINT32 Index) {
    if (!Dev) return NULLPTR;
    
    GdlEncoder *Enc = (GdlEncoder*)MemoryAllocate(sizeof(GdlEncoder));
    if (!Enc) return NULLPTR;
    
    MemSet(Enc, 0, sizeof(GdlEncoder));
    Enc->Index = Index;
    Enc->Dev = Dev;
    Enc->Type = Type;
    SnPrintf(Enc->Name, sizeof(Enc->Name), "Enc%d", Index);
    
    ListAddTail(&Dev->Encoders, &Enc->Node);
    
    return Enc;
}

INT GdlEncoderDestroy(GdlEncoder *Enc) {
    if (!Enc) return GDL_ERR_INVALID_PARAM;
    
    ListDel(&Enc->Node);
    MemoryFree(Enc);
    return GDL_OK;
}

INT GdlEncoderSetMode(GdlEncoder *Enc, GdlMode *Mode) {
    if (!Enc) return GDL_ERR_INVALID_PARAM;
    
    if (Enc->ModeSet) {
        return Enc->ModeSet(Enc, Mode);
    }
    
    return GDL_OK;
}
