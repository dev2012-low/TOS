#pragma once

#include <Kernel/Types.h>
#include <FBDevice.h>

typedef struct {
    FLOAT X;
    FLOAT Y;
    FLOAT Z;
} KF3DVec3;

typedef struct {
    FLOAT X;
    FLOAT Y;
    FLOAT Z;
    FLOAT W;
} KF3DVec4;

/* column-major, v' = M * v */
typedef struct {
    FLOAT M[16];
} KF3DMat4;

typedef struct {
    KF3DVec3 Position;
    FBDeviceColor Color;
} KF3DVertex;

typedef struct {
    const KF3DVertex *Vertices;
    const UINT16 *Indices;
    UINT32 VertexCount;
    UINT32 IndexCount;
} KF3DMesh;

typedef struct {
    KF3DVec3 Eye;
    KF3DVec3 Target;
    KF3DVec3 Up;
    FLOAT FovYRad;
    FLOAT Aspect;
    FLOAT Near;
    FLOAT Far;
} KF3DCamera;

typedef struct {
    INT32 X;
    INT32 Y;
    UINT32 Width;
    UINT32 Height;
} KF3DViewport;
