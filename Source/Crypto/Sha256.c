#include <Crypto/Sha256.h>
#include <Crypto/Crypto.h>
#include <Crypto/Rng.h>
#include <Crypto/Internal.h>
#include <Lib/String.h>
#include <Console.h>

/*
 * =============================================================================
 * SHA256 Constants (первые 32 бита дробных частей кубических корней простых чисел)
 * =============================================================================
 */
static const UINT32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/*
 * =============================================================================
 * Вспомогательные макросы (SHA256 специфичные)
 * =============================================================================
 */
#define CH(X, Y, Z)  (((X) & (Y)) ^ (~(X) & (Z)))
#define MAJ(X, Y, Z) (((X) & (Y)) ^ ((X) & (Z)) ^ ((Y) & (Z)))
#define SIG0(X)      (CRYPTO_ROTR32((X), 2) ^ CRYPTO_ROTR32((X), 13) ^ CRYPTO_ROTR32((X), 22))
#define SIG1(X)      (CRYPTO_ROTR32((X), 6) ^ CRYPTO_ROTR32((X), 11) ^ CRYPTO_ROTR32((X), 25))
#define THETA0(X)    (CRYPTO_ROTR32((X), 7) ^ CRYPTO_ROTR32((X), 18) ^ ((X) >> 3))
#define THETA1(X)    (CRYPTO_ROTR32((X), 17) ^ CRYPTO_ROTR32((X), 19) ^ ((X) >> 10))

/*
 * =============================================================================
 * Преобразование порядка байт (little → big endian для хеша)
 * =============================================================================
 */
static inline UINT32 Swap32(UINT32 X) {
    #ifdef __x86_64__
    __asm__("bswap %0" : "+r"(X));
    #endif
    return X;
}

static inline NOPTR Sha256Transform(Sha256Ctx *Ctx, const UINT8 *Block) {
    UINT32 W[64];
    UINT32 S[8];
    UINT32 T1, T2;
    INT I;

    /* Распаковка блока (64 байта → 16 слов по 32 бита, big-endian) */
    for (I = 0; I < 16; I++) {
        W[I] = (UINT32)Block[I*4]   << 24 |
               (UINT32)Block[I*4+1] << 16 |
               (UINT32)Block[I*4+2] << 8  |
               (UINT32)Block[I*4+3];
    }

    /* Расширение до 64 слов */
    for (I = 16; I < 64; I++) {
        W[I] = THETA1(W[I-2]) + W[I-7] + THETA0(W[I-15]) + W[I-16];
    }

    /* Копируем текущее состояние */
    for (I = 0; I < 8; I++) {
        S[I] = Ctx->State[I];
    }

    /* 64 раунда сжатия */
    for (I = 0; I < 64; I++) {
        T1 = S[7] + SIG1(S[4]) + CH(S[4], S[5], S[6]) + K[I] + W[I];
        T2 = SIG0(S[0]) + MAJ(S[0], S[1], S[2]);
        S[7] = S[6];
        S[6] = S[5];
        S[5] = S[4];
        S[4] = S[3] + T1;
        S[3] = S[2];
        S[2] = S[1];
        S[1] = S[0];
        S[0] = T1 + T2;
    }

    /* Добавляем к глобальному состоянию */
    for (I = 0; I < 8; I++) {
        Ctx->State[I] += S[I];
    }
}

/*
 * =============================================================================
 * Инициализация контекста
 * =============================================================================
 */
NOPTR Sha256Init(Sha256Ctx *Ctx) {
    if (!Ctx) return;

    Ctx->State[0] = 0x6a09e667;
    Ctx->State[1] = 0xbb67ae85;
    Ctx->State[2] = 0x3c6ef372;
    Ctx->State[3] = 0xa54ff53a;
    Ctx->State[4] = 0x510e527f;
    Ctx->State[5] = 0x9b05688c;
    Ctx->State[6] = 0x1f83d9ab;
    Ctx->State[7] = 0x5be0cd19;

    Ctx->Count = 0;
    MemSet(Ctx->Buffer, 0, SHA256_BLOCK_SIZE);
}

/*
 * =============================================================================
 * Добавление данных
 * =============================================================================
 */
NOPTR Sha256Update(Sha256Ctx *Ctx, const UINT8 *Data, UINT32 Len) {
    UINT32 BufferFree;
    UINT32 CopyLen;

    if (!Ctx || !Data || Len == 0) return;

    while (Len > 0) {
        BufferFree = SHA256_BLOCK_SIZE - (Ctx->Count % SHA256_BLOCK_SIZE);
        CopyLen = (Len < BufferFree) ? Len : BufferFree;

        MemCpy(Ctx->Buffer + (Ctx->Count % SHA256_BLOCK_SIZE), Data, CopyLen);

        Data += CopyLen;
        Len -= CopyLen;
        Ctx->Count += CopyLen;

        /* Если буфер заполнен — обрабатываем блок */
        if ((Ctx->Count % SHA256_BLOCK_SIZE) == 0) {
            Sha256Transform(Ctx, Ctx->Buffer);
        }
    }
}

/*
 * =============================================================================
 * Финализация и получение хеша
 * =============================================================================
 */
NOPTR Sha256Final(Sha256Ctx *Ctx, UINT8 *OutHash) {
    UINT64 BitLength;
    UINT32 PadLen;
    UINT32 I;
    UINT32 Offset;

    if (!Ctx || !OutHash) return;

    Offset = Ctx->Count % SHA256_BLOCK_SIZE;
    BitLength = Ctx->Count * 8;

    /* Добавляем бит '1' (0x80) */
    Ctx->Buffer[Offset++] = 0x80;

    /* Если не хватает места для длины (8 байт) — добиваем нулями и обрабатываем */
    PadLen = SHA256_BLOCK_SIZE - Offset;
    if (PadLen < 8) {
        MemSet(Ctx->Buffer + Offset, 0, PadLen);
        Sha256Transform(Ctx, Ctx->Buffer);
        Offset = 0;
        PadLen = SHA256_BLOCK_SIZE;
    }

    /* Заполняем нулями оставшееся место до 8 байт от конца */
    MemSet(Ctx->Buffer + Offset, 0, PadLen - 8);

    /* Записываем длину сообщения (64 бита, big-endian) */
    for (I = 0; I < 8; I++) {
        Ctx->Buffer[SHA256_BLOCK_SIZE - 8 + I] = (UINT8)(BitLength >> (56 - I * 8));
    }

    /* Финальный transform */
    Sha256Transform(Ctx, Ctx->Buffer);

    /* Выгружаем результат (big-endian) */
    for (I = 0; I < 8; I++) {
        UINT32 Word = Ctx->State[I];
        OutHash[I*4]   = (UINT8)(Word >> 24);
        OutHash[I*4+1] = (UINT8)(Word >> 16);
        OutHash[I*4+2] = (UINT8)(Word >> 8);
        OutHash[I*4+3] = (UINT8)Word;
    }

    /* Безопасно обнуляем контекст */
    RngMemZero(Ctx, sizeof(Sha256Ctx));
}

/*
 * =============================================================================
 * Удобная обёртка: хеш от данных одним вызовом
 * =============================================================================
 */
NOPTR Sha256Hash(const UINT8 *Data, UINT32 Len, UINT8 *OutHash) {
    Sha256Ctx Ctx;

    if (!Data || !OutHash) return;

    Sha256Init(&Ctx);
    Sha256Update(&Ctx, Data, Len);
    Sha256Final(&Ctx, OutHash);
}

/*
 * =============================================================================
 * Самопроверка (тестовые векторы NIST)
 * =============================================================================
 */
static const struct {
    const CHAR *Input;
    UINT8 Expected[32];
} TestVectors[] = {
    {"", {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    }},
    {"abc", {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    }},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8,
        0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
        0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1
    }}
};

INT Sha256SelfTest(NOPTR) {
    UINT8 Hash[32];
    INT I;
    INT Failed = 0;

    for (I = 0; I < sizeof(TestVectors) / sizeof(TestVectors[0]); I++) {
        Sha256Hash((const UINT8*)TestVectors[I].Input, 
                   (UINT32)StrLen(TestVectors[I].Input), 
                   Hash);

        if (MemCmp(Hash, TestVectors[I].Expected, SHA256_HASH_SIZE) != 0) {
            Failed++;
        }
    }

    return (Failed == 0) ? CRYPTO_OK : CRYPTO_ERR_HARDWARE_FAIL;
}