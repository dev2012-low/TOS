#include <Gdl/Gdl.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <EdidParse.h>

GdlConnector *GdlConnectorCreate(GdlDevice *Dev, UINT32 Type, UINT32 Index) {
    if (!Dev) return NULLPTR;
    
    GdlConnector *Conn = (GdlConnector*)MemoryAllocate(sizeof(GdlConnector));
    if (!Conn) return NULLPTR;
    
    MemSet(Conn, 0, sizeof(GdlConnector));
    Conn->Index = Index;
    Conn->Dev = Dev;
    Conn->Type = Type;
    Conn->Connected = FALSE;
    Conn->Polled = TRUE;
    ListInit(&Conn->Modes);
    SnPrintf(Conn->Name, sizeof(Conn->Name), "Conn%d", Index);
    
    ListAddTail(&Dev->Connectors, &Conn->Node);
    
    return Conn;
}

INT GdlConnectorDestroy(GdlConnector *Conn) {
    if (!Conn) return GDL_ERR_INVALID_PARAM;
    
    GdlConnectorClearModes(Conn);
    if (Conn->Edid) MemoryFree(Conn->Edid);
    ListDel(&Conn->Node);
    MemoryFree(Conn);
    return GDL_OK;
}

INT GdlConnectorDetect(GdlConnector *Conn) {
    if (!Conn) return 0;
    
    if (Conn->Detect) {
        Conn->Connected = Conn->Detect(Conn) > 0;
    }
    
    return Conn->Connected ? 1 : 0;
}

INT GdlConnectorAddMode(GdlConnector *Conn, GdlMode *Mode) {
    if (!Conn || !Mode) return GDL_ERR_INVALID_PARAM;
    
    ListAddTail(&Conn->Modes, &Mode->Node);
    return GDL_OK;
}

NOPTR GdlConnectorClearModes(GdlConnector *Conn) {
    if (!Conn) return;
    
    struct ListHead *Pos, *N;
    ListForEachSafe(Pos, N, &Conn->Modes) {
        GdlMode *Mode = ListEntry(Pos, GdlMode, Node);
        ListDel(&Mode->Node);
        GdlModeDestroy(Mode);
    }
}

INT GdlConnectorSetEdid(GdlConnector *Conn, UINT8 *Edid, UINT32 Size) {
    if (!Conn || !Edid || Size == 0) return GDL_ERR_INVALID_PARAM;
    
    if (Conn->Edid) MemoryFree(Conn->Edid);
    
    Conn->Edid = (UINT8*)MemoryAllocate(Size);
    if (!Conn->Edid) return GDL_ERR_NO_MEMORY;
    
    MemCpy(Conn->Edid, Edid, Size);
    Conn->EdidSize = Size;
    
    return GDL_OK;
}

NOPTR GdsConnectorUpdateEdid(GdlConnector *Conn, UINT8 *Raw, UINT32 Size) {
    Edid Edid;
    if (EdidParse(Raw, Size, &Edid) != 0) return;
    
    // Add preferred timing from EDID
    if (Edid.PreferredTiming.HActive > 0) {
        GdlMode *Mode = GdlModeCreate(Edid.PreferredTiming.HActive,
                                           Edid.PreferredTiming.VActive, 60);
        GdlConnectorAddMode(Conn, Mode);
    }
    
    // Add CEA modes
    for (INT I = 0; I < Edid.CeaCount; I++) {
        GdlMode *Mode = GdlModeCreate(Edid.CeaModes[I].Width,
                                           Edid.CeaModes[I].Height,
                                           Edid.CeaModes[I].Refresh);
        GdlConnectorAddMode(Conn, Mode);
    }
}