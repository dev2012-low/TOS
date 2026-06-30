#include <FBDevice.h>
#include "Private.h"
#include <Lib/Math.h>

//=============================================================================
// Rectangle primitives
//=============================================================================

NOPTR FBDeviceDrawRect(INT32 X, INT32 Y, UINT32 W, UINT32 H, FBDeviceColor Color) {
    FBDeviceDrawLine(X, Y, X + (INT32)W - 1, Y, Color);
    FBDeviceDrawLine(X + (INT32)W - 1, Y, X + (INT32)W - 1, Y + (INT32)H - 1, Color);
    FBDeviceDrawLine(X + (INT32)W - 1, Y + (INT32)H - 1, X, Y + (INT32)H - 1, Color);
    FBDeviceDrawLine(X, Y + (INT32)H - 1, X, Y, Color);
}

// FBDeviceFillRect уже есть в Base.c

//=============================================================================
// Circle primitives
//=============================================================================

NOPTR FBDeviceDrawCircle(INT32 Cx, INT32 Cy, UINT32 R, FBDeviceColor Color) {
    INT32 F = 1 - (INT32)R;
    INT32 DdF_x = 0;
    INT32 DdF_y = -2 * (INT32)R;
    INT32 X = 0;
    INT32 Y = (INT32)R;
    
    FBDevicePutPixel(Cx, Cy + (INT32)R, Color);
    FBDevicePutPixel(Cx, Cy - (INT32)R, Color);
    FBDevicePutPixel(Cx + (INT32)R, Cy, Color);
    FBDevicePutPixel(Cx - (INT32)R, Cy, Color);
    
    while (X < Y) {
        if (F >= 0) {
            Y--;
            DdF_y += 2;
            F += DdF_y;
        }
        X++;
        DdF_x += 2;
        F += DdF_x + 1;
        
        FBDevicePutPixel(Cx + X, Cy + Y, Color);
        FBDevicePutPixel(Cx - X, Cy + Y, Color);
        FBDevicePutPixel(Cx + X, Cy - Y, Color);
        FBDevicePutPixel(Cx - X, Cy - Y, Color);
        FBDevicePutPixel(Cx + Y, Cy + X, Color);
        FBDevicePutPixel(Cx - Y, Cy + X, Color);
        FBDevicePutPixel(Cx + Y, Cy - X, Color);
        FBDevicePutPixel(Cx - Y, Cy - X, Color);
    }
}

NOPTR FBDeviceFillCircle(INT32 Cx, INT32 Cy, UINT32 R, FBDeviceColor Color) {
    for (UINT32 I = 0; I <= R; I++) {
        FBDeviceDrawCircle(Cx, Cy, I, Color);
    }
}

//=============================================================================
// Ellipse primitives
//=============================================================================

NOPTR FBDeviceDrawEllipse(INT32 Cx, INT32 Cy, UINT32 Rx, UINT32 Ry, FBDeviceColor Color) {
    if (Rx == Ry) {
        FBDeviceDrawCircle(Cx, Cy, Rx, Color);
        return;
    }
    
    INT32 X = 0;
    INT32 Y = (INT32)Ry;
    INT32 Rx2 = (INT32)Rx * (INT32)Rx;
    INT32 Ry2 = (INT32)Ry * (INT32)Ry;
    INT32 TwoRx2 = 2 * Rx2;
    INT32 TwoRy2 = 2 * Ry2;
    INT32 P = Ry2 - Rx2 * Ry + (Rx2 / 4);
    
    while (TwoRy2 * X < TwoRx2 * Y) {
        FBDevicePutPixel(Cx + X, Cy + Y, Color);
        FBDevicePutPixel(Cx - X, Cy + Y, Color);
        FBDevicePutPixel(Cx + X, Cy - Y, Color);
        FBDevicePutPixel(Cx - X, Cy - Y, Color);
        
        if (P < 0) {
            X++;
            P += TwoRy2 * X + Ry2;
        } else {
            X++;
            Y--;
            P += TwoRy2 * X - TwoRx2 * Y + Ry2;
        }
    }
    
    P = Ry2 * (X + 1) * (X + 1) + Rx2 * (Y - 1) * (Y - 1) - Rx2 * Ry2;
    
    while (Y >= 0) {
        FBDevicePutPixel(Cx + X, Cy + Y, Color);
        FBDevicePutPixel(Cx - X, Cy + Y, Color);
        FBDevicePutPixel(Cx + X, Cy - Y, Color);
        FBDevicePutPixel(Cx - X, Cy - Y, Color);
        
        if (P > 0) {
            Y--;
            P -= TwoRx2 * Y + Rx2;
        } else {
            X++;
            Y--;
            P += TwoRy2 * X - TwoRx2 * Y + Rx2;
        }
    }
}

NOPTR FBDeviceFillEllipse(INT32 Cx, INT32 Cy, UINT32 Rx, UINT32 Ry, FBDeviceColor Color) {
    if (Rx == 0 || Ry == 0) return;
    
    if (Rx == Ry) {
        FBDeviceFillCircle(Cx, Cy, Rx, Color);
        return;
    }
    
    UINT32 Pixel = ColorToPixel(Color);
    
    for (INT32 Y = -(INT32)Ry; Y <= (INT32)Ry; Y++) {
        INT32 LineWidth = (INT32)(((INT64)Rx * Rx * (1 - ((INT64)Y * Y / ((INT64)Ry * Ry)))));
        if (LineWidth < 0) continue;
        INT32 XMax = (INT32)ISqrt((UINT32)LineWidth);
        
        for (INT32 X = -XMax; X <= XMax; X++) {
            INT32 Px = Cx + X;
            INT32 Py = Cy + Y;
            if (ClipCoord(&Px, &Py)) {
                PutPixelRaw(Px, Py, Pixel);
            }
        }
    }
}

//=============================================================================
// Line primitives
//=============================================================================

NOPTR FBDeviceDrawLine(INT32 X1, INT32 Y1, INT32 X2, INT32 Y2, FBDeviceColor Color) {
    INT32 Dx = Abs(X2 - X1);
    INT32 Dy = Abs(Y2 - Y1);
    INT32 Sx = (X1 < X2) ? 1 : -1;
    INT32 Sy = (Y1 < Y2) ? 1 : -1;
    INT32 Err = Dx - Dy;
    INT32 CurrentX = X1;
    INT32 CurrentY = Y1;
    
    while (TRUE) {
        FBDevicePutPixel(CurrentX, CurrentY, Color);
        
        if (CurrentX == X2 && CurrentY == Y2) break;
        
        INT32 E2 = 2 * Err;
        if (E2 > -Dy) {
            Err -= Dy;
            CurrentX += Sx;
        }
        if (E2 < Dx) {
            Err += Dx;
            CurrentY += Sy;
        }
    }
}

NOPTR FBDeviceDrawLineThick(INT32 X1, INT32 Y1, INT32 X2, INT32 Y2, 
                            UINT32 Thickness, FBDeviceColor Color) {
    if (Thickness <= 1) {
        FBDeviceDrawLine(X1, Y1, X2, Y2, Color);
        return;
    }
    
    INT32 Dx = X2 - X1;
    INT32 Dy = Y2 - Y1;
    
    if (Abs(Dx) > Abs(Dy)) {
        INT32 Offset = (INT32)Thickness / 2;
        for (UINT32 I = 0; I < Thickness; I++) {
            FBDeviceDrawLine(X1, Y1 - Offset + (INT32)I, X2, Y2 - Offset + (INT32)I, Color);
        }
    } else {
        INT32 Offset = (INT32)Thickness / 2;
        for (UINT32 I = 0; I < Thickness; I++) {
            FBDeviceDrawLine(X1 - Offset + (INT32)I, Y1, X2 - Offset + (INT32)I, Y2, Color);
        }
    }
}

//=============================================================================
// Triangle primitives
//=============================================================================

static NOPTR DrawHorizontalLineFill(INT32 Y, INT32 XStart, INT32 XEnd, UINT32 Pixel) {
    INT32 Start = XStart;
    INT32 End = XEnd;
    if (Start > End) Swap(&Start, &End);
    for (INT32 X = Start; X <= End; X++) {
        PutPixelRaw(X, Y, Pixel);
    }
}

NOPTR FBDeviceDrawTriangle(INT32 X1, INT32 Y1, INT32 X2, INT32 Y2, 
                           INT32 X3, INT32 Y3, FBDeviceColor Color) {
    FBDeviceDrawLine(X1, Y1, X2, Y2, Color);
    FBDeviceDrawLine(X2, Y2, X3, Y3, Color);
    FBDeviceDrawLine(X3, Y3, X1, Y1, Color);
}

NOPTR FBDeviceFillTriangle(INT32 X1, INT32 Y1, INT32 X2, INT32 Y2, 
                           INT32 X3, INT32 Y3, FBDeviceColor Color) {
    UINT32 Pixel = ColorToPixel(Color);
    
    INT32 P1X = X1, P1Y = Y1;
    INT32 P2X = X2, P2Y = Y2;
    INT32 P3X = X3, P3Y = Y3;
    
    // Sort by Y
    if (P1Y > P2Y) { Swap(&P1X, &P2X); Swap(&P1Y, &P2Y); }
    if (P1Y > P3Y) { Swap(&P1X, &P3X); Swap(&P1Y, &P3Y); }
    if (P2Y > P3Y) { Swap(&P2X, &P3X); Swap(&P2Y, &P3Y); }
    
    if (P1Y == P2Y && P2Y == P3Y) return;
    
    if (P1Y == P2Y) {
        INT32 XLeft = P1X, XRight = P2X;
        if (XLeft > XRight) Swap(&XLeft, &XRight);
        
        for (INT32 Y = P1Y; Y <= P3Y; Y++) {
            INT32 XStart = XLeft + (P3X - XLeft) * (Y - P1Y) / (P3Y - P1Y);
            INT32 XEnd = XRight + (P3X - XRight) * (Y - P2Y) / (P3Y - P2Y);
            DrawHorizontalLineFill(Y, XStart, XEnd, Pixel);
        }
    }
    else if (P2Y == P3Y) {
        INT32 XLeft = P2X, XRight = P3X;
        if (XLeft > XRight) Swap(&XLeft, &XRight);
        
        for (INT32 Y = P1Y; Y <= P3Y; Y++) {
            INT32 XStart = P1X + (XLeft - P1X) * (Y - P1Y) / (P2Y - P1Y);
            INT32 XEnd = P1X + (XRight - P1X) * (Y - P1Y) / (P3Y - P1Y);
            DrawHorizontalLineFill(Y, XStart, XEnd, Pixel);
        }
    }
    else {
        INT32 X4 = P1X + (INT32)((INT64)(P3X - P1X) * (P2Y - P1Y) / (P3Y - P1Y));
        
        for (INT32 Y = P1Y; Y <= P2Y; Y++) {
            INT32 XStart = P1X + (P2X - P1X) * (Y - P1Y) / (P2Y - P1Y);
            INT32 XEnd = P1X + (X4 - P1X) * (Y - P1Y) / (P2Y - P1Y);
            DrawHorizontalLineFill(Y, XStart, XEnd, Pixel);
        }
        
        for (INT32 Y = P2Y; Y <= P3Y; Y++) {
            INT32 XStart = P2X + (P3X - P2X) * (Y - P2Y) / (P3Y - P2Y);
            INT32 XEnd = X4 + (P3X - X4) * (Y - P2Y) / (P3Y - P2Y);
            DrawHorizontalLineFill(Y, XStart, XEnd, Pixel);
        }
    }
}

//=============================================================================
// Polygon primitives
//=============================================================================

NOPTR FBDeviceDrawPolygon(INT32 *XPoints, INT32 *YPoints, UINT32 NumPoints, FBDeviceColor Color) {
    if (NumPoints < 3) return;
    
    for (UINT32 I = 0; I < NumPoints - 1; I++) {
        FBDeviceDrawLine(XPoints[I], YPoints[I], XPoints[I + 1], YPoints[I + 1], Color);
    }
    FBDeviceDrawLine(XPoints[NumPoints - 1], YPoints[NumPoints - 1], XPoints[0], YPoints[0], Color);
}

NOPTR FBDeviceFillPolygon(INT32 *XPoints, INT32 *YPoints, UINT32 NumPoints, FBDeviceColor Color) {
    if (NumPoints < 3) return;
    
    UINT32 Pixel = ColorToPixel(Color);
    
    INT32 MinY = YPoints[0];
    INT32 MaxY = YPoints[0];
    for (UINT32 I = 1; I < NumPoints; I++) {
        if (YPoints[I] < MinY) MinY = YPoints[I];
        if (YPoints[I] > MaxY) MaxY = YPoints[I];
    }
    
    for (INT32 Y = MinY; Y <= MaxY; Y++) {
        INT32 Intersections[64];
        UINT32 IntCount = 0;
        
        for (UINT32 I = 0; I < NumPoints; I++) {
            UINT32 J = (I + 1) % NumPoints;
            
            if ((YPoints[I] <= Y && YPoints[J] > Y) || 
                (YPoints[J] <= Y && YPoints[I] > Y)) {
                
                INT32 X = XPoints[I] + (Y - YPoints[I]) * 
                          (XPoints[J] - XPoints[I]) / (YPoints[J] - YPoints[I]);
                if (IntCount < 64) Intersections[IntCount++] = X;
            }
        }
        
        // Sort intersections
        for (UINT32 I = 0; I < IntCount; I++) {
            for (UINT32 J = I + 1; J < IntCount; J++) {
                if (Intersections[I] > Intersections[J]) {
                    Swap(&Intersections[I], &Intersections[J]);
                }
            }
        }
        
        for (UINT32 I = 0; I + 1 < IntCount; I += 2) {
            for (INT32 X = Intersections[I]; X <= Intersections[I + 1]; X++) {
                FBDevicePutPixel(X, Y, Color);
            }
        }
    }
}