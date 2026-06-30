#pragma once

#include <Kernel/Types.h>
#include <Rgb.h>

typedef enum {
    FBDEVICE_FORMAT_RGB888,
    FBDEVICE_FORMAT_BGR888,
    FBDEVICE_FORMAT_RGB565,
    FBDEVICE_FORMAT_UNKNOWN
} FBDevicePixelFormat;

typedef struct {
    UINT8 R;
    UINT8 G;
    UINT8 B;
    UINT8 A;
} FBDeviceColor;

#define FBDEVICE_RGB(R, G, B) ((FBDeviceColor){(R), (G), (B), 255})
#define FBDEVICE_RGBA(R, G, B, A) ((FBDeviceColor){(R), (G), (B), (A)})

#define FONT_HEIGHT 16
#define FONT_WIDTH 8
#define FONT_LINE_SPACING 0
#define FONT_LINE_HEIGHT (FONT_HEIGHT + FONT_LINE_SPACING)

typedef struct {
    UINT64 PhysAddr;
    NOPTR *VirtAddr;
    UINT32 Width;
    UINT32 Height;
    UINT32 Pitch;
    UINT8 Bpp;
    FBDevicePixelFormat Format;
    UINT32 BytesPerPixel;
    NOPTR *BackBuffer;
    BOOL ClipEnabled;
    INT32 ClipX1, ClipY1;
    INT32 ClipX2, ClipY2;
    BOOL Initialized;
} FBDevice;

typedef struct {
    UINT64 Addr;
    UINT32 Pitch;
    UINT32 Width;
    UINT32 Height;
    UINT8 Bpp;
} FBDeviceInfo;

INT FBDeviceInit(UINT64 PhysAddr, UINT32 Width, UINT32 Height, UINT32 Pitch, UINT8 Bpp);
FBDevice *FBDeviceGet(NOPTR);

NOPTR FBDevicePutPixel(INT32 X, INT32 Y, FBDeviceColor Color);
FBDeviceColor FBDeviceGetPixel(INT32 X, INT32 Y);
NOPTR FBDeviceClear(FBDeviceColor Color);

FBDeviceColor FBDeviceRgb(UINT8 R, UINT8 G, UINT8 B);
FBDeviceColor FBDeviceHex(UINT32 Hex);
UINT32 ColorToPixel(FBDeviceColor C);
NOPTR PutPixelRaw(INT32 X, INT32 Y, UINT32 Pixel);

NOPTR FBDeviceDrawRect(INT32 X, INT32 Y, UINT32 W, UINT32 H, FBDeviceColor Color);
NOPTR FBDeviceFillRect(INT32 X, INT32 Y, UINT32 W, UINT32 H, FBDeviceColor Color);
NOPTR FBDeviceDrawCircle(INT32 Cx, INT32 Cy, UINT32 R, FBDeviceColor Color);
NOPTR FBDeviceFillCircle(INT32 Cx, INT32 Cy, UINT32 R, FBDeviceColor Color);
NOPTR FBDeviceDrawEllipse(INT32 Cx, INT32 Cy, UINT32 Rx, UINT32 Ry, FBDeviceColor Color);
NOPTR FBDeviceFillEllipse(INT32 Cx, INT32 Cy, UINT32 Rx, UINT32 Ry, FBDeviceColor Color);
NOPTR FBDeviceDrawTriangle(INT32 X, INT32 Y1, INT32 X2, INT32 Y2, INT32 X3, INT32 Y3, FBDeviceColor Color);
NOPTR FBDeviceFillTriangle(INT32 X, INT32 Y1, INT32 X2, INT32 Y2, INT32 X3, INT32 Y3, FBDeviceColor Color);
NOPTR FBDeviceDrawLine(INT32 X1, INT32 Y1, INT32 X2, INT32 Y2, FBDeviceColor Color);
NOPTR FBDeviceDrawLineThick(INT32 X, INT32 Y1, INT32 X2, INT32 Y2, UINT32 Thickness, FBDeviceColor Color);
NOPTR FBDeviceDrawPolygon(INT32 *XPoints, INT32 *YPoints, UINT32 NumPoints, FBDeviceColor Color);
NOPTR FBDeviceFillPolygon(INT32 *XPoints, INT32 *YPoints, UINT32 NumPoints, FBDeviceColor Color);

NOPTR FBDeviceSetClip(INT32 X1, INT32 Y1, INT32 X2, INT32 Y2);
NOPTR FBDeviceResetClip(NOPTR);
NOPTR FBDeviceDisableClip(NOPTR);

UINT32 FBDeviceGetWidth(NOPTR);
UINT32 FBDeviceGetHeight(NOPTR);
BOOL FBDeviceIsInitialized(NOPTR);
UINT8 FBDeviceGetBpp(NOPTR);

NOPTR FBDeviceDrawChar(CHAR C, INT32 X, INT32 Y, FBDeviceColor Fg, FBDeviceColor Bg);
NOPTR FBDeviceDrawString(const CHAR *Str, INT32 X, INT32 Y, FBDeviceColor Fg, FBDeviceColor Bg);
NOPTR FBDeviceDrawCharLarge(CHAR C, INT32 X, INT32 Y, UINT8 Scale, FBDeviceColor Fg, FBDeviceColor Bg);
NOPTR FBDeviceDrawStringLarge(const CHAR *Str, INT32 X, INT32 Y, UINT8 Scale, FBDeviceColor Fg, FBDeviceColor Bg);
UINT32 FBDeviceTextWidth(const CHAR *Str);
UINT32 FBDeviceTextWidthLarge(const CHAR *Str, UINT8 Scale);
UINT32 FBDeviceCharWidth(CHAR C);
UINT32 FBDeviceCharWidthlarge(CHAR C, UINT8 Scale);

NOPTR FBDeviceSwapBuffers(NOPTR);
NOPTR FBDeviceClearBack(NOPTR);

FBDeviceInfo* FBDeviceGetInfoFromMB2(NOPTR);

NOPTR FBDeviceDrawBitmap(INT32 X, INT32 Y, INT Width, INT Height, const UINT8 *Bitmap);