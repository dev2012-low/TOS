#pragma once

#include <Kernel/Types.h>

#define SHA256_HASH_SIZE    32
#define SHA256_BLOCK_SIZE   64

typedef struct {
    UINT32 State[8];
    UINT64 Count;
    UINT8 Buffer[SHA256_BLOCK_SIZE];
} Sha256Ctx;

NOPTR Sha256Init(Sha256Ctx *Ctx);
NOPTR Sha256Update(Sha256Ctx *Ctx, const UINT8 *Data, UINT32 Len);
NOPTR Sha256Final(Sha256Ctx *Ctx, UINT8 *OutHash);
NOPTR Sha256Hash(const UINT8 *Data, UINT32 Len, UINT8 *OutHash);
INT Sha256SelfTest(NOPTR);