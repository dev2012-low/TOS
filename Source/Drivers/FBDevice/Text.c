#include <FBDevice.h>
#include "Private.h"
#include <FontAccess.h>
#include <Lib/String.h>

//=============================================================================
// Text rendering
//=============================================================================

NOPTR FBDeviceDrawChar(CHAR C, INT32 X, INT32 Y, FBDeviceColor Fg, FBDeviceColor Bg) {
    const UINT8 (*Glyph)[16] = FontGetGlyph(C);
    if (!Glyph) return;
    
    UINT32 FgPixel = ColorToPixel(Fg);
    UINT32 BgPixel = ColorToPixel(Bg);
    
    for (UINT8 Row = 0; Row < FONT_HEIGHT; Row++) {
        UINT8 Bits = (*Glyph)[Row];
        INT32 Py = Y + (INT32)Row;
        
        for (UINT8 Col = 0; Col < FONT_WIDTH; Col++) {
            INT32 Px = X + (INT32)Col;
            
            if (Bits & (0x80 >> Col)) {
                PutPixelRaw(Px, Py, FgPixel);
            } else {
                PutPixelRaw(Px, Py, BgPixel);
            }
        }
    }
}

NOPTR FBDeviceDrawString(const CHAR *Str, INT32 X, INT32 Y, FBDeviceColor Fg, FBDeviceColor Bg) {
    if (!Str) return;
    
    INT32 CurX = X;
    INT32 CurY = Y;
    
    while (*Str) {
        if (*Str == '\n') {
            CurX = X;
            CurY += FONT_LINE_HEIGHT;
        } else {
            FBDeviceDrawChar(*Str, CurX, CurY, Fg, Bg);
            CurX += FONT_WIDTH;
        }
        Str++;
    }
}

NOPTR FBDeviceDrawCharLarge(CHAR C, INT32 X, INT32 Y, UINT8 Scale, FBDeviceColor Fg, FBDeviceColor Bg) {
    if (Scale == 0) return;
    if (Scale == 1) {
        FBDeviceDrawChar(C, X, Y, Fg, Bg);
        return;
    }
    
    const UINT8 (*Glyph)[16] = FontGetGlyph(C);
    if (!Glyph) return;
    
    UINT32 FgPixel = ColorToPixel(Fg);
    UINT32 BgPixel = ColorToPixel(Bg);
    
    for (UINT8 Row = 0; Row < FONT_HEIGHT; Row++) {
        UINT8 Bits = (*Glyph)[Row];
        
        for (UINT8 Col = 0; Col < FONT_WIDTH; Col++) {
            UINT32 Color = (Bits & (0x80 >> Col)) ? FgPixel : BgPixel;
            
            for (UINT8 Sy = 0; Sy < Scale; Sy++) {
                INT32 Py = Y + (INT32)(Row * Scale + Sy);
                for (UINT8 Sx = 0; Sx < Scale; Sx++) {
                    INT32 Px = X + (INT32)(Col * Scale + Sx);
                    if (Px >= 0 && Px < (INT32)GFBDevice.Width && 
                        Py >= 0 && Py < (INT32)GFBDevice.Height) {
                        PutPixelRaw(Px, Py, Color);
                    }
                }
            }
        }
    }
}

NOPTR FBDeviceDrawStringLarge(const CHAR *Str, INT32 X, INT32 Y, UINT8 Scale, FBDeviceColor Fg, FBDeviceColor Bg) {
    if (!Str) return;
    
    INT32 CurX = X;
    INT32 CurY = Y;
    
    while (*Str) {
        if (*Str == '\n') {
            CurX = X;
            CurY += FONT_LINE_HEIGHT * Scale;
        } else {
            FBDeviceDrawCharLarge(*Str, CurX, CurY, Scale, Fg, Bg);
            CurX += FONT_WIDTH * (INT32)Scale;
        }
        Str++;
    }
}

UINT32 FBDeviceTextWidth(const CHAR *Str) {
    if (!Str) return 0;
    return (UINT32)StrLen(Str) * FONT_WIDTH;
}

UINT32 FBDeviceTextWidthLarge(const CHAR *Str, UINT8 Scale) {
    if (!Str) return 0;
    return (UINT32)StrLen(Str) * FONT_WIDTH * Scale;
}

UINT32 FBDeviceCharWidth(CHAR C) {
    (NOPTR)C;
    return FONT_WIDTH;
}

UINT32 FBDeviceCharWidthLarge(CHAR C, UINT8 Scale) {
    (NOPTR)C;
    return FONT_WIDTH * Scale;
}