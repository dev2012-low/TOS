#pragma once

#include <Kernel/Types.h>

#define CHACHA20_KEY_SIZE 32
#define CHACHA20_IV_SIZE 12
#define CHACHA20_BLOCK_SIZE 64

typedef struct {
    UINT32 State[16];
} ChaCha20Ctx;

NOPTR ChaCha20Init(ChaCha20Ctx *Ctx, const UINT8 Key[32], const UINT8 Iv[12]);
NOPTR ChaCha20Encrypt(ChaCha20Ctx *Ctx, const UINT8 *Src, UINT8 *Dst, UINT32 Len);
NOPTR ChaCha20Decrypt(ChaCha20Ctx *Ctx, const UINT8 *Src, UINT8 *Dst, UINT32 Len);
NOPTR ChaCha20Xor(ChaCha20Ctx *Ctx, UINT8 *Data, UINT32 Len);