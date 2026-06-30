#include <Crypto/ChaCha20.h>
#include <Crypto/Internal.h>
#include <Lib/String.h>

/*
 * ============================================================================= ChaCha20 Quarter Round ============================================================================
 */

#define CHACHA_QR(A, B, C, D) \
    A += B; D ^= A; D = CRYPTO_ROTL32(D, 16); \
    C += D; B ^= C; B = CRYPTO_ROTL32(B, 12); \
    A += B; D ^= A; D = CRYPTO_ROTL32(D, 8);  \
    C += D; B ^= C; B = CRYPTO_ROTL32(B, 7)

/*
 * ================================================================================================ Initialization of state =======================================================================================
 */

NOPTR ChaCha20Init(ChaCha20Ctx *Ctx, const UINT8 Key[32], const UINT8 Iv[12]) {
    const UINT32 Constants[4] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
    };
    
    /*
 * Constant
 */
    Ctx->State[0] = Constants[0];
    Ctx->State[1] = Constants[1];
    Ctx->State[2] = Constants[2];
    Ctx->State[3] = Constants[3];
    
    /*
 * Key (32 bytes)
 */
    for (INT I = 0; I < 8; I++) {
        Ctx->State[4 + I] = (UINT32)Key[I*4] |
                            (UINT32)Key[I*4+1] << 8 |
                            (UINT32)Key[I*4+2] << 16 |
                            (UINT32)Key[I*4+3] << 24;
    }
    
    /*
 * Block counter (0)
 */
    Ctx->State[12] = 0;
    
    /*
 * Nonce (12 bytes)
 */
    Ctx->State[13] = (UINT32)Iv[0] |
                     (UINT32)Iv[1] << 8 |
                     (UINT32)Iv[2] << 16 |
                     (UINT32)Iv[3] << 24;
    Ctx->State[14] = (UINT32)Iv[4] |
                     (UINT32)Iv[5] << 8 |
                     (UINT32)Iv[6] << 16 |
                     (UINT32)Iv[7] << 24;
    Ctx->State[15] = (UINT32)Iv[8] |
                     (UINT32)Iv[9] << 8 |
                     (UINT32)Iv[10] << 16 |
                     (UINT32)Iv[11] << 24;
}

/*
 * =============================================================================== Generating a key stream block ================================================================================
 */

static NOPTR ChaCha20Block(ChaCha20Ctx *Ctx, UINT8 *Output) {
    UINT32 Working[16];
    MemCpy(Working, Ctx->State, sizeof(Working));
    
    /*
 * 10 rounds (20 half rounds)
 */
    for (INT I = 0; I < 10; I++) {
        CHACHA_QR(Working[0], Working[4], Working[ 8], Working[12]);
        CHACHA_QR(Working[1], Working[5], Working[ 9], Working[13]);
        CHACHA_QR(Working[2], Working[6], Working[10], Working[14]);
        CHACHA_QR(Working[3], Working[7], Working[11], Working[15]);
        CHACHA_QR(Working[0], Working[5], Working[10], Working[15]);
        CHACHA_QR(Working[1], Working[6], Working[11], Working[12]);
        CHACHA_QR(Working[2], Working[7], Working[ 8], Working[13]);
        CHACHA_QR(Working[3], Working[4], Working[ 9], Working[14]);
    }
    
    /*
 * Adding an initial state
 */
    for (INT I = 0; I < 16; I++) {
        Working[I] += Ctx->State[I];
    }
    
    /*
 * Convert to bytes (little-endian)
 */
    for (INT I = 0; I < 16; I++) {
        Output[I*4]   = Working[I] & 0xFF;
        Output[I*4+1] = (Working[I] >> 8) & 0xFF;
        Output[I*4+2] = (Working[I] >> 16) & 0xFF;
        Output[I*4+3] = (Working[I] >> 24) & 0xFF;
    }
    
    /*
 * Block counter increment
 */
    Ctx->State[12]++;
    if (Ctx->State[12] == 0) {
        Ctx->State[13]++;
    }
}

/*
 * ============================================================================== Encryption/Decryption ================================================================================
 */

NOPTR ChaCha20Xor(ChaCha20Ctx *Ctx, UINT8 *Data, UINT32 Len) {
    UINT8 Keystream[64];
    UINT32 Offset = 0;
    
    while (Len > 0) {
        ChaCha20Block(Ctx, Keystream);
        
        UINT32 Chunk = (Len < 64) ? Len : 64;
        for (UINT32 I = 0; I < Chunk; I++) {
            Data[Offset + I] ^= Keystream[I];
        }
        
        Offset += Chunk;
        Len -= Chunk;
    }
}

NOPTR ChaCha20Encrypt(ChaCha20Ctx *Ctx, const UINT8 *Src, UINT8 *Dst, UINT32 Len) {
    MemCpy(Dst, Src, Len);
    ChaCha20Xor(Ctx, Dst, Len);
}

NOPTR ChaCha20Decrypt(ChaCha20Ctx *Ctx, const UINT8 *Src, UINT8 *Dst, UINT32 Len) {
    /*
 * ChaCha20 is symmetrical
 */
    ChaCha20Encrypt(Ctx, Src, Dst, Len);
}
