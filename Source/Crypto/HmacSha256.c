#include <Crypto/Hmac.h>
#include <Crypto/Sha256.h>
#include <Crypto/Crypto.h>
#include <Crypto/Rng.h>
#include <Lib/String.h>
#include <Console.h>

#define IPAD 0x36
#define OPAD 0x5C

NOPTR HmacSha256Init(HmacSha256Ctx *Ctx, const UINT8 *Key, UINT32 KeyLen) {
    UINT8 K0[SHA256_BLOCK_SIZE];
    UINT32 I;

    if (!Ctx || !Key) return;

    MemSet(K0, 0, SHA256_BLOCK_SIZE);

    /* Если ключ длиннее блока — хешируем его */
    if (KeyLen > SHA256_BLOCK_SIZE) {
        Sha256Hash(Key, KeyLen, K0);
    } else {
        MemCpy(K0, Key, KeyLen);
    }

    /* Сохраняем K0 для финализации */
    MemCpy(Ctx->KeyPad, K0, SHA256_BLOCK_SIZE);

    /* Внутренний контекст: K0 ⊕ ipad */
    Sha256Init(&Ctx->Inner);
    for (I = 0; I < SHA256_BLOCK_SIZE; I++) {
        K0[I] ^= IPAD;
    }
    Sha256Update(&Ctx->Inner, K0, SHA256_BLOCK_SIZE);

    /* Внешний контекст: K0 ⊕ opad */
    Sha256Init(&Ctx->Outer);
    for (I = 0; I < SHA256_BLOCK_SIZE; I++) {
        K0[I] ^= (IPAD ^ OPAD);  /* Восстанавливаем и XOR с opad */
    }
    Sha256Update(&Ctx->Outer, K0, SHA256_BLOCK_SIZE);

    /* Безопасно обнуляем временный буфер */
    RngMemZero(K0, sizeof(K0));
}

NOPTR HmacSha256Update(HmacSha256Ctx *Ctx, const UINT8 *Data, UINT32 Len) {
    if (!Ctx || !Data) return;
    Sha256Update(&Ctx->Inner, Data, Len);
}

NOPTR HmacSha256Final(HmacSha256Ctx *Ctx, UINT8 *OutMac) {
    UINT8 InnerHash[SHA256_HASH_SIZE];

    if (!Ctx || !OutMac) return;

    /* Финализируем внутренний хеш */
    Sha256Final(&Ctx->Inner, InnerHash);

    /* Добавляем внутренний хеш во внешний */
    Sha256Update(&Ctx->Outer, InnerHash, SHA256_HASH_SIZE);

    /* Финализируем внешний хеш */
    Sha256Final(&Ctx->Outer, OutMac);

    /* Безопасно обнуляем */
    RngMemZero(InnerHash, sizeof(InnerHash));
}

/* Удобная обёртка: всё в одном вызове */
NOPTR HmacSha256(const UINT8 *Key, UINT32 KeyLen,
                 const UINT8 *Data, UINT32 DataLen,
                 UINT8 *OutMac) {
    HmacSha256Ctx Ctx;

    if (!Key || !Data || !OutMac) return;

    HmacSha256Init(&Ctx, Key, KeyLen);
    HmacSha256Update(&Ctx, Data, DataLen);
    HmacSha256Final(&Ctx, OutMac);
}

/*
 * =============================================================================
 * Самопроверка (RFC 4231 тестовые векторы)
 * =============================================================================
 */
static const struct {
    const UINT8 *Key;
    UINT32 KeyLen;
    const UINT8 *Data;
    UINT32 DataLen;
    UINT8 Expected[32];
} TestVectors[] = {
    /* RFC 4231 Test Case 1 */
    { (UINT8*)"\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b\x0b", 20,
      (UINT8*)"Hi There", 8,
      {0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
       0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
       0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
       0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7} },
    /* RFC 4231 Test Case 2 */
    { (UINT8*)"Jefe", 4,
      (UINT8*)"what do ya want for nothing?", 28,
      {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
       0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
       0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
       0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43} },
    /* RFC 4231 Test Case 3 */
    { (UINT8*)"\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa\xaa", 20,
      (UINT8*)"\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd\xdd", 50,
      {0x77, 0x3e, 0xa9, 0x1e, 0x36, 0x80, 0x0e, 0x46,
       0x85, 0x4d, 0xb8, 0xeb, 0xd0, 0x91, 0x81, 0xa7,
       0x29, 0x59, 0x09, 0x8b, 0x3e, 0xf8, 0xc1, 0x22,
       0xd9, 0x63, 0x55, 0x14, 0xce, 0xd5, 0x65, 0xfe} }
};

INT HmacSha256SelfTest(NOPTR) {
    UINT8 Mac[32];
    INT I;
    INT Failed = 0;

    for (I = 0; I < sizeof(TestVectors) / sizeof(TestVectors[0]); I++) {
        HmacSha256(TestVectors[I].Key, TestVectors[I].KeyLen,
                   TestVectors[I].Data, TestVectors[I].DataLen,
                   Mac);

        if (MemCmp(Mac, TestVectors[I].Expected, HMAC_SHA256_SIZE) == 0) {
	    //
        } else {
            Failed++;
        }
    }

    return (Failed == 0) ? CRYPTO_OK : CRYPTO_ERR_HARDWARE_FAIL;
}