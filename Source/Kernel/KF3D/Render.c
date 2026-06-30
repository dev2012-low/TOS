#include <KF3D/KF3D.h>
#include <FBDevice.h>
#include <Rgb.h>
#include <Memory/Allocator.h>
#include <Lib/Math.h>
#include <Lib/String.h>
#include <Kernel/Return.h>

typedef struct {
    FLOAT X;
    FLOAT Y;
    FLOAT Z;
    FBDeviceColor Color;
} KF3DClipVert;

static struct {
    BOOL Ready;
    KF3DViewport Viewport;
    KF3DMat4 Model;
    KF3DMat4 View;
    KF3DMat4 Proj;
    FLOAT *DepthBuf;
    UINT32 DepthPixels;
    BOOL Wireframe;
} GR;

static void TransformVec4(KF3DVec4 *Out, const KF3DMat4 *M, FLOAT X, FLOAT Y, FLOAT Z) {
    Out->X = M->M[0] * X + M->M[4] * Y + M->M[8] * Z + M->M[12];
    Out->Y = M->M[1] * X + M->M[5] * Y + M->M[9] * Z + M->M[13];
    Out->Z = M->M[2] * X + M->M[6] * Y + M->M[10] * Z + M->M[14];
    Out->W = M->M[3] * X + M->M[7] * Y + M->M[11] * Z + M->M[15];
}

static INT AllocDepthBuffer(void) {
    UINT32 Pixels = GR.Viewport.Width * GR.Viewport.Height;
    FLOAT *NewBuf;

    if (Pixels == 0) {
        return INCORRECT_VALUE;
    }
    if (Pixels == GR.DepthPixels && GR.DepthBuf) {
        return SUCCESS;
    }

    if (GR.DepthBuf) {
        MemoryFree(GR.DepthBuf);
        GR.DepthBuf = NULLPTR;
        GR.DepthPixels = 0;
    }

    NewBuf = (FLOAT *)MemoryAllocate((USIZE)Pixels * sizeof(FLOAT));
    if (!NewBuf) {
        return NO_MEMORY;
    }

    GR.DepthBuf = NewBuf;
    GR.DepthPixels = Pixels;
    return SUCCESS;
}

INT KF3DInit(NOPTR) {
    UINT32 W;
    UINT32 H;

    if (GR.Ready) {
        return SUCCESS;
    }
    if (!FBDeviceIsInitialized()) {
        return DEVICE_ERROR;
    }

    MemSet(&GR, 0, sizeof(GR));
    W = FBDeviceGetWidth();
    H = FBDeviceGetHeight();
    GR.Viewport.X = 0;
    GR.Viewport.Y = 0;
    GR.Viewport.Width = W;
    GR.Viewport.Height = H;

    KF3DMat4Identity(&GR.Model);
    KF3DMat4Identity(&GR.View);
    KF3DMat4Identity(&GR.Proj);

    if (AllocDepthBuffer() != SUCCESS) {
        return NO_MEMORY;
    }

    GR.Ready = TRUE;
    return SUCCESS;
}

NOPTR KF3DShutdown(NOPTR) {
    if (GR.DepthBuf) {
        MemoryFree(GR.DepthBuf);
    }
    MemSet(&GR, 0, sizeof(GR));
}

BOOL KF3DIsReady(NOPTR) {
    return GR.Ready;
}

NOPTR KF3DSetViewport(const KF3DViewport *Vp) {
    if (!GR.Ready || !Vp || Vp->Width == 0 || Vp->Height == 0) {
        return;
    }
    GR.Viewport = *Vp;
    AllocDepthBuffer();
}

NOPTR KF3DGetViewport(KF3DViewport *Out) {
    if (!Out) {
        return;
    }
    *Out = GR.Viewport;
}

NOPTR KF3DSetModelMatrix(const KF3DMat4 *Model) {
    if (Model) {
        GR.Model = *Model;
    }
}

NOPTR KF3DSetViewMatrix(const KF3DMat4 *View) {
    if (View) {
        GR.View = *View;
    }
}

NOPTR KF3DSetProjMatrix(const KF3DMat4 *Proj) {
    if (Proj) {
        GR.Proj = *Proj;
    }
}

NOPTR KF3DSetWireframe(BOOL Enable) {
    GR.Wireframe = Enable;
}

static void ClearDepth(void) {
    UINT32 I;
    if (!GR.DepthBuf) {
        return;
    }
    for (I = 0; I < GR.DepthPixels; I++) {
        GR.DepthBuf[I] = 1.0f;
    }
}

NOPTR KF3DBeginFrame(FBDeviceColor ClearColor) {
    INT32 X2;
    INT32 Y2;

    if (!GR.Ready) {
        return;
    }

    ClearDepth();
    X2 = GR.Viewport.X + (INT32)GR.Viewport.Width - 1;
    Y2 = GR.Viewport.Y + (INT32)GR.Viewport.Height - 1;
    FBDeviceSetClip(GR.Viewport.X, GR.Viewport.Y, X2, Y2);
    FBDeviceFillRect(GR.Viewport.X, GR.Viewport.Y, GR.Viewport.Width, GR.Viewport.Height, ClearColor);
}

NOPTR KF3DEndFrame(NOPTR) {
    FBDeviceResetClip();
}

static BOOL ProjectVertex(KF3DClipVert *Out, KF3DVec3 Pos, FBDeviceColor Color) {
    KF3DMat4 Mv;
    KF3DMat4 Mvp;
    KF3DVec4 Clip;
    FLOAT InvW;
    FLOAT NdcX;
    FLOAT NdcY;
    FLOAT NdcZ;

    KF3DMat4Multiply(&Mv, &GR.View, &GR.Model);
    KF3DMat4Multiply(&Mvp, &GR.Proj, &Mv);
    TransformVec4(&Clip, &Mvp, Pos.X, Pos.Y, Pos.Z);

    if (Clip.W <= 0.0001f) {
        return FALSE;
    }

    InvW = 1.0f / Clip.W;
    NdcX = Clip.X * InvW;
    NdcY = Clip.Y * InvW;
    NdcZ = Clip.Z * InvW;

    if (NdcZ < -1.0f || NdcZ > 1.0f) {
        return FALSE;
    }

    Out->X = GR.Viewport.X + (NdcX * 0.5f + 0.5f) * (FLOAT)GR.Viewport.Width;
    Out->Y = GR.Viewport.Y + (1.0f - (NdcY * 0.5f + 0.5f)) * (FLOAT)GR.Viewport.Height;
    Out->Z = NdcZ;
    Out->Color = Color;
    return TRUE;
}

static FBDeviceColor LerpColor(FBDeviceColor A, FBDeviceColor B, FLOAT T) {
    FBDeviceColor R;
    R.R = (UINT8)((FLOAT)A.R + ((FLOAT)B.R - (FLOAT)A.R) * T);
    R.G = (UINT8)((FLOAT)A.G + ((FLOAT)B.G - (FLOAT)A.G) * T);
    R.B = (UINT8)((FLOAT)A.B + ((FLOAT)B.B - (FLOAT)A.B) * T);
    R.A = 255;
    return R;
}

static void PutDepthPixel(INT32 X, INT32 Y, FLOAT Z, FBDeviceColor Color) {
    INT32 Lx;
    INT32 Ly;
    UINT32 Idx;

    Lx = X - GR.Viewport.X;
    Ly = Y - GR.Viewport.Y;
    if (Lx < 0 || Ly < 0) {
        return;
    }
    if ((UINT32)Lx >= GR.Viewport.Width || (UINT32)Ly >= GR.Viewport.Height) {
        return;
    }

    Idx = (UINT32)Ly * GR.Viewport.Width + (UINT32)Lx;
    if (Z >= GR.DepthBuf[Idx]) {
        return;
    }

    GR.DepthBuf[Idx] = Z;
    FBDevicePutPixel(X, Y, Color);
}

static void DrawLine3D(KF3DClipVert A, KF3DClipVert B) {
    INT32 X0 = (INT32)A.X;
    INT32 Y0 = (INT32)A.Y;
    INT32 X1 = (INT32)B.X;
    INT32 Y1 = (INT32)B.Y;
    INT32 Dx = Abs(X1 - X0);
    INT32 Dy = Abs(Y1 - Y0);
    INT32 Sx = (X0 < X1) ? 1 : -1;
    INT32 Sy = (Y0 < Y1) ? 1 : -1;
    INT32 Err = Dx - Dy;
    INT32 Steps = Dx + Dy + 1;
    INT32 I;
    FLOAT T;

    if (Steps <= 0) {
        Steps = 1;
    }

    for (I = 0; I <= Steps; I++) {
        T = (FLOAT)I / (FLOAT)Steps;
        PutDepthPixel(X0, Y0, A.Z + (B.Z - A.Z) * T, LerpColor(A.Color, B.Color, T));
        if (X0 == X1 && Y0 == Y1) {
            break;
        }
        if (Err * 2 > -Dy) {
            Err -= Dy;
            X0 += Sx;
        }
        if (Err * 2 < Dx) {
            Err += Dx;
            Y0 += Sy;
        }
    }
}

static FBDeviceColor BaryColor(FBDeviceColor C0, FBDeviceColor C1, FBDeviceColor C2,
                               FLOAT W0, FLOAT W1, FLOAT W2) {
    FBDeviceColor R;
    R.R = (UINT8)((FLOAT)C0.R * W0 + (FLOAT)C1.R * W1 + (FLOAT)C2.R * W2);
    R.G = (UINT8)((FLOAT)C0.G * W0 + (FLOAT)C1.G * W1 + (FLOAT)C2.G * W2);
    R.B = (UINT8)((FLOAT)C0.B * W0 + (FLOAT)C1.B * W1 + (FLOAT)C2.B * W2);
    R.A = 255;
    return R;
}

static void FillTriangle(KF3DClipVert V0, KF3DClipVert V1, KF3DClipVert V2) {
    FLOAT Area;
    INT32 MinX;
    INT32 MaxX;
    INT32 MinY;
    INT32 MaxY;
    INT32 X;
    INT32 Y;

    Area = (V1.X - V0.X) * (V2.Y - V0.Y) - (V2.X - V0.X) * (V1.Y - V0.Y);
    if (Area <= 0.0f) {
        return;
    }

    MinX = (INT32)V0.X;
    if ((INT32)V1.X < MinX) MinX = (INT32)V1.X;
    if ((INT32)V2.X < MinX) MinX = (INT32)V2.X;
    MaxX = (INT32)V0.X;
    if ((INT32)V1.X > MaxX) MaxX = (INT32)V1.X;
    if ((INT32)V2.X > MaxX) MaxX = (INT32)V2.X;

    MinY = (INT32)V0.Y;
    if ((INT32)V1.Y < MinY) MinY = (INT32)V1.Y;
    if ((INT32)V2.Y < MinY) MinY = (INT32)V2.Y;
    MaxY = (INT32)V0.Y;
    if ((INT32)V1.Y > MaxY) MaxY = (INT32)V1.Y;
    if ((INT32)V2.Y > MaxY) MaxY = (INT32)V2.Y;

    if (MinX < GR.Viewport.X) MinX = GR.Viewport.X;
    if (MinY < GR.Viewport.Y) MinY = GR.Viewport.Y;
    if (MaxX >= GR.Viewport.X + (INT32)GR.Viewport.Width) {
        MaxX = GR.Viewport.X + (INT32)GR.Viewport.Width - 1;
    }
    if (MaxY >= GR.Viewport.Y + (INT32)GR.Viewport.Height) {
        MaxY = GR.Viewport.Y + (INT32)GR.Viewport.Height - 1;
    }

    for (Y = MinY; Y <= MaxY; Y++) {
        FLOAT Py = (FLOAT)Y + 0.5f;
        for (X = MinX; X <= MaxX; X++) {
            FLOAT Px = (FLOAT)X + 0.5f;
            FLOAT W0;
            FLOAT W1;
            FLOAT W2;
            FLOAT Z;
            FBDeviceColor C;

            W0 = ((V1.X - V2.X) * (Py - V2.Y) + (V2.Y - V1.Y) * (Px - V2.X)) / Area;
            W1 = ((V2.X - V0.X) * (Py - V0.Y) + (V0.Y - V2.Y) * (Px - V0.X)) / Area;
            W2 = 1.0f - W0 - W1;

            if (W0 < 0.0f || W1 < 0.0f || W2 < 0.0f) {
                continue;
            }

            Z = V0.Z * W0 + V1.Z * W1 + V2.Z * W2;
            C = BaryColor(V0.Color, V1.Color, V2.Color, W0, W1, W2);
            PutDepthPixel(X, Y, Z, C);
        }
    }
}

static void DrawTriangle(KF3DClipVert V0, KF3DClipVert V1, KF3DClipVert V2) {
    if (GR.Wireframe) {
        DrawLine3D(V0, V1);
        DrawLine3D(V1, V2);
        DrawLine3D(V2, V0);
    } else {
        FillTriangle(V0, V1, V2);
    }
}

static void DrawIndexedTriangle(const KF3DMesh *Mesh, UINT32 I0, UINT32 I1, UINT32 I2,
                                FBDeviceColor FlatColor, BOOL UseVertexColor) {
    KF3DClipVert C0;
    KF3DClipVert C1;
    KF3DClipVert C2;
    FBDeviceColor Col0;
    FBDeviceColor Col1;
    FBDeviceColor Col2;

    if (I0 >= Mesh->VertexCount || I1 >= Mesh->VertexCount || I2 >= Mesh->VertexCount) {
        return;
    }

    Col0 = UseVertexColor ? Mesh->Vertices[I0].Color : FlatColor;
    Col1 = UseVertexColor ? Mesh->Vertices[I1].Color : FlatColor;
    Col2 = UseVertexColor ? Mesh->Vertices[I2].Color : FlatColor;

    if (!ProjectVertex(&C0, Mesh->Vertices[I0].Position, Col0)) {
        return;
    }
    if (!ProjectVertex(&C1, Mesh->Vertices[I1].Position, Col1)) {
        return;
    }
    if (!ProjectVertex(&C2, Mesh->Vertices[I2].Position, Col2)) {
        return;
    }

    DrawTriangle(C0, C1, C2);
}

NOPTR KF3DDrawMesh(const KF3DMesh *Mesh, FBDeviceColor Color) {
    UINT32 I;

    if (!GR.Ready || !Mesh || !Mesh->Vertices) {
        return;
    }

    if (Mesh->Indices && Mesh->IndexCount >= 3) {
        for (I = 0; I + 2 < Mesh->IndexCount; I += 3) {
            DrawIndexedTriangle(Mesh, Mesh->Indices[I], Mesh->Indices[I + 1],
                                Mesh->Indices[I + 2], Color, FALSE);
        }
    } else if (Mesh->VertexCount >= 3) {
        for (I = 0; I + 2 < Mesh->VertexCount; I += 3) {
            DrawIndexedTriangle(Mesh, I, I + 1, I + 2, Color, FALSE);
        }
    }
}

NOPTR KF3DDrawMeshVertexColor(const KF3DMesh *Mesh) {
    UINT32 I;

    if (!GR.Ready || !Mesh || !Mesh->Vertices) {
        return;
    }

    if (Mesh->Indices && Mesh->IndexCount >= 3) {
        for (I = 0; I + 2 < Mesh->IndexCount; I += 3) {
            DrawIndexedTriangle(Mesh, Mesh->Indices[I], Mesh->Indices[I + 1],
                                Mesh->Indices[I + 2], RGB_BLACK, TRUE);
        }
    }
}
