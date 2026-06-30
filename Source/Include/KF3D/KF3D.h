#pragma once

#include <KF3D/Types.h>
#include <Kernel/Return.h>

/* Kernel Framebuffer 3D — программный рендер в FBDevice */

INT KF3DInit(NOPTR);
NOPTR KF3DShutdown(NOPTR);
BOOL KF3DIsReady(NOPTR);

NOPTR KF3DSetViewport(const KF3DViewport *Vp);
NOPTR KF3DGetViewport(KF3DViewport *Out);

NOPTR KF3DBeginFrame(FBDeviceColor ClearColor);
NOPTR KF3DEndFrame(NOPTR);

NOPTR KF3DSetModelMatrix(const KF3DMat4 *Model);
NOPTR KF3DSetViewMatrix(const KF3DMat4 *View);
NOPTR KF3DSetProjMatrix(const KF3DMat4 *Proj);
NOPTR KF3DSetWireframe(BOOL Enable);

NOPTR KF3DMat4Identity(KF3DMat4 *Out);
NOPTR KF3DMat4Multiply(KF3DMat4 *Out, const KF3DMat4 *A, const KF3DMat4 *B);
NOPTR KF3DMat4Translate(KF3DMat4 *Out, FLOAT X, FLOAT Y, FLOAT Z);
NOPTR KF3DMat4RotateY(KF3DMat4 *Out, FLOAT Rad);
NOPTR KF3DMat4RotateX(KF3DMat4 *Out, FLOAT Rad);
NOPTR KF3DMat4Perspective(KF3DMat4 *Out, FLOAT FovYRad, FLOAT Aspect, FLOAT Near, FLOAT Far);
NOPTR KF3DMat4LookAt(KF3DMat4 *Out, KF3DVec3 Eye, KF3DVec3 Target, KF3DVec3 Up);

NOPTR KF3DCameraPerspective(KF3DCamera *Cam, FLOAT FovYDeg, FLOAT Aspect, FLOAT Near, FLOAT Far);
NOPTR KF3DCameraLookAt(KF3DCamera *Cam, KF3DVec3 Eye, KF3DVec3 Target, KF3DVec3 Up);
NOPTR KF3DCameraApply(const KF3DCamera *Cam);

NOPTR KF3DDrawMesh(const KF3DMesh *Mesh, FBDeviceColor Color);
NOPTR KF3DDrawMeshVertexColor(const KF3DMesh *Mesh);
