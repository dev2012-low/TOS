#include <FBDevice.h>
#include "Private.h"
#include <Kernel/KDriver.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Asm/Cpu.h>
#include <Kernel/Return.h>
#include <Multiboot2Struct.h>
#include <Multiboot2Parser.h>
#include <Lib/Math.h>

FBDevice GFBDevice = {0};

static BOOL SimdAvailable = FALSE;

static NOPTR FastMemSet32(UINT32 *Dst, UINT32 Value, UINT32 Count) {
    if (!SimdAvailable) {
        for (UINT32 I = 0; I < Count; I++) {
            Dst[I] = Value;
        }
        return;
    }
    UINT64 Value64 = ((UINT64)Value << 32) | Value;
    UINT32 Blocks = Count / 2;
    UINT32 Remainder = Count % 2;
    UINT64 *Dst64 = (UINT64*)Dst;
    for (UINT32 I = 0; I < Blocks; I++) {
        Dst64[I] = Value64;
    }
    if (Remainder) {
        Dst[Blocks * 2] = Value;
    }
}

static NOPTR FastMemCpy32(UINT32 *Dst, UINT32 *Src, UINT32 Count) {
    if (!SimdAvailable) {
        MemCpy(Dst, Src, Count * 4);
        return;
    }
    UINT32 Blocks = Count / 16;
    UINT32 Remainder = Count % 16;
    for (UINT32 I = 0; I < Blocks; I++) {
        UINT32 *D = Dst + I * 16;
        UINT32 *S = Src + I * 16;
        asm volatile(
            "movq (%1), %%mm0\n\t"
            "movq 8(%1), %%mm1\n\t"
            "movq 16(%1), %%mm2\n\t"
            "movq 24(%1), %%mm3\n\t"
            "movq 32(%1), %%mm4\n\t"
            "movq 40(%1), %%mm5\n\t"
            "movq 48(%1), %%mm6\n\t"
            "movq 56(%1), %%mm7\n\t"
            "movq %%mm0, (%0)\n\t"
            "movq %%mm1, 8(%0)\n\t"
            "movq %%mm2, 16(%0)\n\t"
            "movq %%mm3, 24(%0)\n\t"
            "movq %%mm4, 32(%0)\n\t"
            "movq %%mm5, 40(%0)\n\t"
            "movq %%mm6, 48(%0)\n\t"
            "movq %%mm7, 56(%0)\n\t"
            : : "r"(D), "r"(S)
            : "memory", "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7"
        );
    }
    for (UINT32 I = Blocks * 16; I < Count; I++) {
        Dst[I] = Src[I];
    }
    asm volatile("emms" ::: "memory");
}

static NOPTR FastFillRect(UINT32 *Dst, UINT32 Width, UINT32 Height, UINT32 PitchPixels, UINT32 Color) {
    UINT32 LinePixels = Width;
    UINT64 StartTicks = SimdAvailable ? 0 : 0;
    for (UINT32 Y = 0; Y < Height; Y++) {
        UINT32 *Row = Dst + Y * PitchPixels;
        FastMemSet32(Row, Color, LinePixels);
    }
}

UINT32 ColorToPixel(FBDeviceColor C) {
    if (GFBDevice.Format == FBDEVICE_FORMAT_RGB888) {
        return (C.R << 16) | (C.G << 8) | C.B;
    } else if (GFBDevice.Format == FBDEVICE_FORMAT_BGR888) {
        return (C.B << 16) | (C.G << 8) | C.R;
    } else if (GFBDevice.Format == FBDEVICE_FORMAT_RGB565) {
        return ((C.R >> 3) << 11) | ((C.G >> 2) << 5) | (C.B >> 3);
    }
    return 0;
}

static inline BOOL ClipCoordFast(INT32 *X, INT32 *Y, UINT32 *W, UINT32 *H) {
    if (*X < 0) { *W += *X; *X = 0; }
    if (*Y < 0) { *H += *Y; *Y = 0; }
    if (*X + *W > GFBDevice.Width) *W = GFBDevice.Width - *X;
    if (*Y + *H > GFBDevice.Height) *H = GFBDevice.Height - *Y;
    return (*W > 0 && *H > 0);
}

BOOL ClipCoord(INT32 *X, INT32 *Y) {  
    if (GFBDevice.ClipEnabled) {
        if (*X < GFBDevice.ClipX1 || *X >= GFBDevice.ClipX2 ||
            *Y < GFBDevice.ClipY1 || *Y >= GFBDevice.ClipY2) {
                return FALSE;
        }
    } else {
        if (*X < 0 || *X >= (INT32)GFBDevice.Width ||
            *Y < 0 || *Y >= (INT32)GFBDevice.Height) {
                return FALSE;
        }
    }
    return TRUE;
}

NOPTR PutPixelRaw(INT32 X, INT32 Y, UINT32 Pixel) {
    if (X < 0 || X >= (INT32)GFBDevice.Width || Y < 0 || Y >= (INT32)GFBDevice.Height) {
        return;
    }
    UINT8 *Ptr;
    if (GFBDevice.BackBuffer) {
        Ptr = (UINT8*)GFBDevice.BackBuffer + Y * GFBDevice.Pitch + X * GFBDevice.BytesPerPixel;
    } else {
        Ptr = (UINT8*)GFBDevice.VirtAddr + Y * GFBDevice.Pitch + X * GFBDevice.BytesPerPixel;
    }
    if (GFBDevice.BytesPerPixel == 4) {
        *(UINT32*)Ptr = Pixel;
    } else if (GFBDevice.BytesPerPixel == 3) {
        Ptr[0] = Pixel & 0xFF;
        Ptr[1] = (Pixel >> 8) & 0xFF;
        Ptr[2] = (Pixel >> 16) & 0xFF;
    } else if (GFBDevice.BytesPerPixel == 2) {
        *(UINT16*)Ptr = (UINT16)Pixel;
    }
}

INT FBDeviceInit(UINT64 PhysAddr, UINT32 Width, UINT32 Height, UINT32 Pitch, UINT8 Bpp) {
    if (!PhysAddr || Width == 0 || Height == 0) RETURN(INCORRECT_VALUE);
    GFBDevice.PhysAddr = PhysAddr;
    GFBDevice.VirtAddr = (NOPTR*)PhysAddr;
    GFBDevice.Width = Width;
    GFBDevice.Height = Height;
    GFBDevice.Pitch = Pitch;
    GFBDevice.Bpp = Bpp;
    GFBDevice.BytesPerPixel = (Bpp + 7) / 8;
    GFBDevice.BackBuffer = MemoryAllocate(GFBDevice.Height * GFBDevice.Pitch);
    if (GFBDevice.BackBuffer) {
        MemSet(GFBDevice.BackBuffer, 0, GFBDevice.Height * GFBDevice.Pitch);
    }
    if (Bpp == 32) GFBDevice.Format = FBDEVICE_FORMAT_RGB888;
    else if (Bpp == 16) GFBDevice.Format = FBDEVICE_FORMAT_RGB565;
    else GFBDevice.Format = FBDEVICE_FORMAT_UNKNOWN;
    GFBDevice.ClipEnabled = FALSE;
    UINT32 Eax, Ebx, Ecx, Edx;
    Cpuid(1, &Eax, &Ebx, &Ecx, &Edx);
    SimdAvailable = (Edx & (1 << 23)) != 0;
    SimdAvailable |= (Edx & (1 << 25)) != 0;
    GFBDevice.Initialized = TRUE;
    KDriverRegister(KDriverGenerateStruct("Framebuffer", 0, TRUE, NULLPTR, NULLPTR));
    FBDeviceClear(RGB_BLACK);
    RETURN(SUCCESS);
}

FBDevice *FBDeviceGet(NOPTR) {
    return GFBDevice.Initialized ? &GFBDevice : NULLPTR;
}

NOPTR FBDevicePutPixel(INT32 X, INT32 Y, FBDeviceColor Color) {
    if (!ClipCoord(&X, &Y)) return;
    PutPixelRaw(X, Y, ColorToPixel(Color));
}

FBDeviceColor FBDeviceGetPixel(INT32 X, INT32 Y) {
    FBDeviceColor Black = RGB_BLACK;
    if (!ClipCoord(&X, &Y)) return Black;
    UINT8 *Ptr;
    if (GFBDevice.BackBuffer) {
        Ptr = (UINT8*)GFBDevice.BackBuffer + Y * GFBDevice.Pitch + X * GFBDevice.BytesPerPixel;
    } else {
        Ptr = (UINT8*)GFBDevice.VirtAddr + Y * GFBDevice.Pitch + X * GFBDevice.BytesPerPixel;
    }
    UINT32 Pixel = 0;
    if (GFBDevice.BytesPerPixel == 4) Pixel = *(UINT32*)Ptr;
    else if (GFBDevice.BytesPerPixel == 3) Pixel = Ptr[0] | (Ptr[1] << 8) | (Ptr[2] << 16);
    else if (GFBDevice.BytesPerPixel == 2) Pixel = *(UINT16*)Ptr;
    if (GFBDevice.Format == FBDEVICE_FORMAT_RGB888) {
        return (FBDeviceColor){ (Pixel >> 16) & 0xFF, (Pixel >> 8) & 0xFF, Pixel & 0xFF, 255 };
    }
    return Black;
}

NOPTR FBDeviceFillRect(INT32 X, INT32 Y, UINT32 W, UINT32 H, FBDeviceColor Color) {
    if (!ClipCoordFast(&X, &Y, &W, &H)) return;
    UINT32 Pixel = ColorToPixel(Color);
    UINT32 PitchPixels = GFBDevice.Pitch / 4;
    UINT32 *Dst;
    if (GFBDevice.BackBuffer) {
        Dst = (UINT32*)GFBDevice.BackBuffer;
    } else {
        Dst = (UINT32*)GFBDevice.VirtAddr;
    }
    Dst += Y * PitchPixels + X;
    FastFillRect(Dst, W, H, PitchPixels, Pixel);
}

NOPTR FBDeviceClear(FBDeviceColor Color) {
    if (GFBDevice.BackBuffer) {
        FBDeviceFillRect(0, 0, GFBDevice.Width, GFBDevice.Height, Color);
    } else {
        FBDeviceFillRect(0, 0, GFBDevice.Width, GFBDevice.Height, Color);
    }
}

NOPTR FBDeviceSwapBuffers(NOPTR) {
    if (!GFBDevice.BackBuffer) return;
    UINT32 TotalPixels = GFBDevice.Width * GFBDevice.Height;
    UINT32 *Dst = (UINT32*)GFBDevice.VirtAddr;
    UINT32 *Src = (UINT32*)GFBDevice.BackBuffer;
    FastMemCpy32(Dst, Src, TotalPixels);
}

NOPTR FBDeviceClearBack(NOPTR) {
    if (!GFBDevice.BackBuffer) return;
    FBDeviceFillRect(0, 0, GFBDevice.Width, GFBDevice.Height, RGB_BLACK);
}

FBDeviceColor FBDeviceRgb(UINT8 R, UINT8 G, UINT8 B) {
    return (FBDeviceColor){R, G, B, 255};
}

FBDeviceColor FBDeviceHex(UINT32 Hex) {
    return (FBDeviceColor){ (Hex >> 16) & 0xFF, (Hex >> 8) & 0xFF, Hex & 0xFF, 255 };
}

UINT32 FBDeviceGetWidth(NOPTR) {
    return GFBDevice.Width;
}

UINT32 FBDeviceGetHeight(NOPTR) {
    return GFBDevice.Height;
}

BOOL FBDeviceIsInitialized(NOPTR) {
    return GFBDevice.Initialized;
}

UINT8 FBDeviceGetBpp(NOPTR) {
    return GFBDevice.Bpp;
}

NOPTR FBDeviceSetClip(INT32 X1, INT32 Y1, INT32 X2, INT32 Y2) {
    GFBDevice.ClipX1 = (X1 < 0) ? 0 : X1;
    GFBDevice.ClipY1 = (Y1 < 0) ? 0 : Y1;
    GFBDevice.ClipX2 = (X2 >= (INT32)GFBDevice.Width) ? (INT32)GFBDevice.Width : X2;
    GFBDevice.ClipY2 = (Y2 >= (INT32)GFBDevice.Height) ? (INT32)GFBDevice.Height : Y2;
    GFBDevice.ClipEnabled = TRUE;
}

NOPTR FBDeviceResetClip(NOPTR) {
    GFBDevice.ClipX1 = 0;
    GFBDevice.ClipY1 = 0;
    GFBDevice.ClipX2 = GFBDevice.Width;
    GFBDevice.ClipY2 =  GFBDevice.Height;
    GFBDevice.ClipEnabled = FALSE;
}

NOPTR FBDeviceDisableClip(NOPTR) {
    GFBDevice.ClipEnabled = FALSE;
}

NOPTR Swap(INT32 *A, INT32 *B) {
    INT32 T = *A;
    *A = *B;
    *B = T;
}

FBDeviceInfo* FBDeviceGetInfoFromMB2(NOPTR) {
    Multiboot2Info* CurrentMBInfo = Multiboot2ParserGet();
    if (!CurrentMBInfo) return NULLPTR;
    
    FBDeviceInfo* FBInfo = (FBDeviceInfo*)MemoryAllocate(sizeof(FBDeviceInfo));
    if (!FBInfo) return NULLPTR;
    
    FBInfo->Addr = CurrentMBInfo->Framebuffer.Addr;
    FBInfo->Pitch = CurrentMBInfo->Framebuffer.Pitch;
    FBInfo->Width = CurrentMBInfo->Framebuffer.Width;
    FBInfo->Height = CurrentMBInfo->Framebuffer.Height;
    FBInfo->Bpp = CurrentMBInfo->Framebuffer.Bpp;
    
    return FBInfo;
}

static UINT32 RGB888ToRGB565(UINT8 R, UINT8 G, UINT8 B) {
    return ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3);
}

static UINT32 RGB565ToRGB888(UINT16 RGB565) {
    UINT8 R = ((RGB565 >> 11) & 0x1F) << 3;
    UINT8 G = ((RGB565 >> 5) & 0x3F) << 2;
    UINT8 B = (RGB565 & 0x1F) << 3;
    return (R << 16) | (G << 8) | B;
}

NOPTR FBDeviceDrawBitmap(INT32 X, INT32 Y, INT Width, INT Height, const UINT8 *Bitmap) {
    const UINT8 *Pixel = Bitmap;

    for (UINT32 Row = 0; Row < Height; Row++) {
        for (UINT32 Col = 0; Col < Width; Col++) {
            UINT8 R = *Pixel++;
            UINT8 G = *Pixel++;
            UINT8 B = *Pixel++;
            UINT8 A = *Pixel++;  // Читаем альфа‑канал

            if (A == 0) continue;  // Пропускаем полностью прозрачные пиксели

            FBDevicePutPixel(X + Col, Y + Row, FBDEVICE_RGB(R, G, B));
        }
    }
}
