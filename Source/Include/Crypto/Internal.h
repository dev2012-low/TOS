#pragma once

#include <Asm/Cpu.h>

/*
 * =============================================================================== Internal macros (safe) ================================================================================
 */

/*
 * ROTate Left/Right
 */
#define CRYPTO_ROTL32(X, N) (((X) << (N)) | ((X) >> (32 - (N))))
#define CRYPTO_ROTR32(X, N) (((X) >> (N)) | ((X) << (32 - (N))))
#define CRYPTO_ROTL64(X, N) (((X) << (N)) | ((X) >> (64 - (N))))
#define CRYPTO_ROTR64(X, N) (((X) >> (N)) | ((X) << (64 - (N))))

/*
 * Little/Big Endian conversion
 */
static inline UINT32 CryptoCpuToBe32(UINT32 X) {
    #ifdef __x86_64__
    __asm__("bswap %0" : "+r"(X));
    #endif
    return X;
}

static inline UINT32 CryptoCpuToLe32(UINT32 X) {
    return X;  /*
 * x86 — little-endian
 */
}

static inline UINT32 CryptoBe32ToCpu(UINT32 X) {
    #ifdef __x86_64__
    __asm__("bswap %0" : "+r"(X));
    #endif
    return X;
}

static inline UINT64 CryptoCpuToBe64(UINT64 X) {
    #ifdef __x86_64__
    __asm__("bswap %0" : "+r"(X));
    #endif
    return X;
}

/*
 * Rotate for SHA
 */
#define SHA_CH(X, Y, Z) (((X) & (Y)) ^ (~(X) & (Z)))
#define SHA_MAJ(X, Y, Z) (((X) & (Y)) ^ ((X) & (Z)) ^ ((Y) & (X)))
#define SHA_PARITY(X, Y, Z) ((X) ^ (Y) ^ (Z))