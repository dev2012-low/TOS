#pragma once

#include <Kernel/Types.h>
#include <Crypto/Sha256.h>

#define HMAC_SHA256_SIZE 32

/* HMAC-SHA256 */
NOPTR HmacSha256(const UINT8 *Key, UINT32 KeyLen,
                 const UINT8 *Data, UINT32 DataLen,
                 UINT8 *OutMac);

/* Для использования в PBKDF2 — внутренняя функция с возможностью продолжения */
typedef struct {
    Sha256Ctx Inner;
    Sha256Ctx Outer;
    UINT8      KeyPad[SHA256_BLOCK_SIZE];
} HmacSha256Ctx;

NOPTR HmacSha256Init(HmacSha256Ctx *Ctx, const UINT8 *Key, UINT32 KeyLen);
NOPTR HmacSha256Update(HmacSha256Ctx *Ctx, const UINT8 *Data, UINT32 Len);
NOPTR HmacSha256Final(HmacSha256Ctx *Ctx, UINT8 *OutMac);

/* Самопроверка (тестовые векторы RFC 4231) */
INT HmacSha256SelfTest(NOPTR);