#include <Gdl/Gdl.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>

static const struct {
    UINT32 Width;
    UINT32 Height;
    UINT32 Refresh;
} StandardModes[] = {
    {640, 480, 60},
    {800, 600, 60},
    {1024, 768, 60},
    {1152, 864, 60},
    {1280, 720, 60},
    {1280, 768, 60},
    {1280, 800, 60},
    {1280, 960, 60},
    {1280, 1024, 60},
    {1360, 768, 60},
    {1366, 768, 60},
    {1440, 900, 60},
    {1600, 900, 60},
    {1600, 1200, 60},
    {1680, 1050, 60},
    {1920, 1080, 60},
    {1920, 1200, 60},
    {2560, 1440, 60},
    {3840, 2160, 60},
    {0, 0, 0}
};

static NOPTR CalculateCvtTiming(GdlMode *Mode) {
    // Simplified CVT (Coordinated Video Timings)
    UINT32 HPixels = Mode->HDisplay;
    UINT32 VLines = Mode->VDisplay;
    UINT32 Refresh = Mode->Refresh;
    
    Mode->HDisplay = HPixels;
    Mode->HSyncStart = HPixels + 16;
    Mode->HSyncEnd = HPixels + 80;
    Mode->HTotal = HPixels + 160;
    Mode->HSkew = 0;
    
    Mode->VDisplay = VLines;
    Mode->VSyncStart = VLines + 6;
    Mode->VSyncEnd = VLines + 16;
    Mode->VTotal = VLines + 22;
    Mode->VScan = 0;
    
    Mode->ClockKhz = (HPixels * VLines * Refresh) / 1000;
    if (Mode->ClockKhz < 1) Mode->ClockKhz = 1;
}

GdlMode *GdlModeCreate(UINT32 Width, UINT32 Height, UINT32 Refresh) {
    GdlMode *Mode = (GdlMode*)MemoryAllocate(sizeof(GdlMode));
    if (!Mode) return NULLPTR;
    
    MemSet(Mode, 0, sizeof(GdlMode));
    Mode->Width = Width;
    Mode->Height = Height;
    Mode->Refresh = Refresh;
    Mode->Type = GDL_MODE_TYPE_DRIVER;
    SnPrintf(Mode->Name, sizeof(Mode->Name), "%dx%d@%d", Width, Height, Refresh);
    
    Mode->HDisplay = Width;
    Mode->VDisplay = Height;
    CalculateCvtTiming(Mode);
    
    return Mode;
}

GdlMode *GdlModeCreateTiming(UINT32 HDisplay, UINT32 HSyncStart, UINT32 HSyncEnd, UINT32 HTotal,
                                    UINT32 VDisplay, UINT32 VSyncStart, UINT32 VSyncEnd, UINT32 VTotal,
                                    UINT32 ClockKhz) {
    GdlMode *Mode = (GdlMode*)MemoryAllocate(sizeof(GdlMode));
    if (!Mode) return NULLPTR;
    
    MemSet(Mode, 0, sizeof(GdlMode));
    Mode->HDisplay = HDisplay;
    Mode->HSyncStart = HSyncStart;
    Mode->HSyncEnd = HSyncEnd;
    Mode->HTotal = HTotal;
    Mode->VDisplay = VDisplay;
    Mode->VSyncStart = VSyncStart;
    Mode->VSyncEnd = VSyncEnd;
    Mode->VTotal = VTotal;
    Mode->ClockKhz = ClockKhz;
    
    Mode->Width = HDisplay;
    Mode->Height = VDisplay;
    
    if (ClockKhz > 0 && HTotal > 0 && VTotal > 0) {
        Mode->Refresh = (ClockKhz * 1000) / (HTotal * VTotal);
    } else {
        Mode->Refresh = 60;
    }
    
    SnPrintf(Mode->Name, sizeof(Mode->Name), "%dx%d@%d", Mode->Width, Mode->Height, Mode->Refresh);
    Mode->Type = GDL_MODE_TYPE_DRIVER;
    
    return Mode;
}

NOPTR GdlModeDestroy(GdlMode *Mode) {
    if (Mode) MemoryFree(Mode);
}

INT GdlModeValid(GdlCrtc *Crtc, GdlMode *Mode) {
    if (!Crtc || !Mode) return 0;
    
    // Basic validation
    if (Mode->Width == 0 || Mode->Height == 0) return 0;
    if (Mode->Width > 4096 || Mode->Height > 4096) return 0;
    if (Mode->Refresh < 24 || Mode->Refresh > 240) return 0;
    
    return 1;
}

GdlMode *GdlModeFind(GdlConnector *Conn, UINT32 Width, UINT32 Height) {
    if (!Conn) return NULLPTR;
    
    struct ListHead *Pos;
    ListForEach(Pos, &Conn->Modes) {
        GdlMode *Mode = ListEntry(Pos, GdlMode, Node);
        if (Mode->Width == Width && Mode->Height == Height) {
            return Mode;
        }
    }
    return NULLPTR;
}