#include <Crypto/Rng.h>
#include <Crypto/ChaCha20.h>
#include <Crypto/Crypto.h>
#include <Crypto/Sha256.h>
#include <Crypto/Hmac.h>
#include <Console.h>
#include <Lib/String.h>

/*
 * ============================================================================== ChaCha20 test ===================================================================================
 */
static INT TestChaCha20(NOPTR) {
    UINT8 Key[32] = {0};
    UINT8 Iv[12] = {0};
    UINT8 PlainText[64];
    UINT8 CipherText[64];
    UINT8 Decrypted[64];
    
    for (INT I = 0; I < 64; I++)
        PlainText[I] = 0;
    
    ChaCha20Ctx Ctx;
    ChaCha20Init(&Ctx, Key, Iv);
    ChaCha20Encrypt(&Ctx, PlainText, CipherText, 64);
    
    ChaCha20Init(&Ctx, Key, Iv);
    ChaCha20Decrypt(&Ctx, CipherText, Decrypted, 64);
    
    if (MemCmp(Decrypted, PlainText, 64) == 0)
        return CRYPTO_OK;
    return CRYPTO_ERR_HARDWARE_FAIL;
}

/*
 * ============================================================================== RNG test ====================================================================================
 */
static INT TestRng(NOPTR) {
    UINT8 Buf[256];
    INT NonZero = 0;
    
    RngGetRandomBytes(Buf, sizeof(Buf));
    
    for (INT I = 0; I < sizeof(Buf); I++) {
        if (Buf[I] != 0) NonZero++;
    }
    
    if (NonZero > 0)
        return CRYPTO_OK;
    return CRYPTO_ERR_HARDWARE_FAIL;
}

/*
 * =============================================================================== Test table ================================================================================== Test table
 */
static const struct {
    const CHAR *Name;
    INT (*Test)(NOPTR);
} Tests[] = {
    {"ChaCha20", TestChaCha20},
    {"RNG",     TestRng},
    {"SHA-256", Sha256SelfTest},
    {"HMAC-SHA-256", HmacSha256SelfTest},
    {NULLPTR, NULLPTR}
};

/*
 * =============================================================================== Public function self-test =================================================================================
 */
INT CryptoSelfTest(NOPTR) {
    INT Passed = 0;
    INT Failed = 0;
    
    for (INT I = 0; Tests[I].Name; I++) {
        if (Tests[I].Test() == CRYPTO_OK) {
            Passed++;
        } else {
            Failed++;
        }
    }
    
    return (Failed == 0) ? CRYPTO_OK : CRYPTO_ERR_HARDWARE_FAIL;
}
