#include <KF3D/KF3D.h>
#include <Lib/Math.h>
#include <Lib/String.h>

NOPTR KF3DMat4Identity(KF3DMat4 *Out) {
    MemSet(Out, 0, sizeof(KF3DMat4));
    Out->M[0] = 1.0f;
    Out->M[5] = 1.0f;
    Out->M[10] = 1.0f;
    Out->M[15] = 1.0f;
}

NOPTR KF3DMat4Multiply(KF3DMat4 *Out, const KF3DMat4 *A, const KF3DMat4 *B) {
    KF3DMat4 R;
    UINT32 Col;
    UINT32 Row;
    UINT32 K;

    for (Col = 0; Col < 4; Col++) {
        for (Row = 0; Row < 4; Row++) {
            FLOAT Sum = 0.0f;
            for (K = 0; K < 4; K++) {
                Sum += A->M[K * 4 + Row] * B->M[Col * 4 + K];
            }
            R.M[Col * 4 + Row] = Sum;
        }
    }
    *Out = R;
}

NOPTR KF3DMat4Translate(KF3DMat4 *Out, FLOAT X, FLOAT Y, FLOAT Z) {
    KF3DMat4Identity(Out);
    Out->M[12] = X;
    Out->M[13] = Y;
    Out->M[14] = Z;
}

NOPTR KF3DMat4RotateY(KF3DMat4 *Out, FLOAT Rad) {
    FLOAT C = CosF(Rad);
    FLOAT S = SinF(Rad);

    KF3DMat4Identity(Out);
    Out->M[0] = C;
    Out->M[2] = S;
    Out->M[8] = -S;
    Out->M[10] = C;
}

NOPTR KF3DMat4RotateX(KF3DMat4 *Out, FLOAT Rad) {
    FLOAT C = CosF(Rad);
    FLOAT S = SinF(Rad);

    KF3DMat4Identity(Out);
    Out->M[5] = C;
    Out->M[6] = -S;
    Out->M[9] = S;
    Out->M[10] = C;
}

NOPTR KF3DMat4Perspective(KF3DMat4 *Out, FLOAT FovYRad, FLOAT Aspect, FLOAT Near, FLOAT Far) {
    FLOAT F = 1.0f / TanF(FovYRad * 0.5f);
    FLOAT Range = Near - Far;

    KF3DMat4Identity(Out);
    Out->M[0] = F / Aspect;
    Out->M[5] = F;
    Out->M[10] = (Far + Near) / Range;
    Out->M[11] = -1.0f;
    Out->M[14] = (2.0f * Far * Near) / Range;
    Out->M[15] = 0.0f;
}

static KF3DVec3 Vec3Sub(KF3DVec3 A, KF3DVec3 B) {
    KF3DVec3 R;
    R.X = A.X - B.X;
    R.Y = A.Y - B.Y;
    R.Z = A.Z - B.Z;
    return R;
}

static KF3DVec3 Vec3Cross(KF3DVec3 A, KF3DVec3 B) {
    KF3DVec3 R;
    R.X = A.Y * B.Z - A.Z * B.Y;
    R.Y = A.Z * B.X - A.X * B.Z;
    R.Z = A.X * B.Y - A.Y * B.X;
    return R;
}

static KF3DVec3 Vec3Normalize(KF3DVec3 V) {
    FLOAT Len = SqrtF(V.X * V.X + V.Y * V.Y + V.Z * V.Z);
    if (Len > 0.0001f) {
        V.X /= Len;
        V.Y /= Len;
        V.Z /= Len;
    }
    return V;
}

NOPTR KF3DMat4LookAt(KF3DMat4 *Out, KF3DVec3 Eye, KF3DVec3 Target, KF3DVec3 Up) {
    KF3DVec3 F = Vec3Normalize(Vec3Sub(Target, Eye));
    KF3DVec3 S = Vec3Normalize(Vec3Cross(F, Up));
    KF3DVec3 U = Vec3Cross(S, F);

    KF3DMat4Identity(Out);
    Out->M[0] = S.X;
    Out->M[4] = S.Y;
    Out->M[8] = S.Z;
    Out->M[1] = U.X;
    Out->M[5] = U.Y;
    Out->M[9] = U.Z;
    Out->M[2] = -F.X;
    Out->M[6] = -F.Y;
    Out->M[10] = -F.Z;
    Out->M[12] = -(S.X * Eye.X + S.Y * Eye.Y + S.Z * Eye.Z);
    Out->M[13] = -(U.X * Eye.X + U.Y * Eye.Y + U.Z * Eye.Z);
    Out->M[14] = F.X * Eye.X + F.Y * Eye.Y + F.Z * Eye.Z;
}

NOPTR KF3DCameraPerspective(KF3DCamera *Cam, FLOAT FovYDeg, FLOAT Aspect, FLOAT Near, FLOAT Far) {
    const FLOAT Pi = 3.14159265f;
    Cam->FovYRad = FovYDeg * Pi / 180.0f;
    Cam->Aspect = Aspect;
    Cam->Near = Near;
    Cam->Far = Far;
}

NOPTR KF3DCameraLookAt(KF3DCamera *Cam, KF3DVec3 Eye, KF3DVec3 Target, KF3DVec3 Up) {
    Cam->Eye = Eye;
    Cam->Target = Target;
    Cam->Up = Up;
}

NOPTR KF3DCameraApply(const KF3DCamera *Cam) {
    KF3DMat4 View;
    KF3DMat4 Proj;

    KF3DMat4LookAt(&View, Cam->Eye, Cam->Target, Cam->Up);
    KF3DMat4Perspective(&Proj, Cam->FovYRad, Cam->Aspect, Cam->Near, Cam->Far);
    KF3DSetViewMatrix(&View);
    KF3DSetProjMatrix(&Proj);
}
