#include <Crypto/Rng.h>
#include <Crypto/ChaCha20.h>
#include <Crypto/Internal.h>
#include <Crypto/Crypto.h>
#include <Asm/Cpu.h>
#include <Asm/Io.h>
#include <Lib/String.h>
#include <Time/Timer.h>

/*
 * ============================================================================== RNG states =====================================================================================
 */

static struct {
    ChaCha20Ctx Ctx;
    UINT8 Pool[256];       /*
 * Entropy pool
 */
    UINT32 PoolIdx;
    UINT64 ReseedCounter;
    BOOL Initialized;
    BOOL RdrandAvailable;
} GRng;

/*
 * ============================================================================== Checking RDRAND ==================================================================================
 */

static BOOL RdrandAvailable(NOPTR) {
    UINT32 Eax, Ebx, Ecx, Edx;
    Cpuid(1, &Eax, &Ebx, &Ecx, &Edx);
    return (Ecx & (1 << 30)) != 0;  /*
 * RDRAND bit
 */
}

static UINT64 Rdrand64(NOPTR) {
    UINT64 Val;
    INT Retry = 10;
    
    while (Retry--) {
        UINT8 Ok;
        asm volatile(
            "rdrand %0; setc %1"
            : "=r"(Val), "=qm"(Ok)
            : : "cc"
        );
        if (Ok) return Val;
    }
    
    return 0;
}

NOPTR RngMemZero(NOPTR *Ptr, UINT32 Len) {
    volatile UINT8 *P = (volatile UINT8 *)Ptr;
    while (Len--) {
        *P++ = 0;
    }
}

/*
 * =============================================================================== Entropy collection =====================================================================================
 */

static NOPTR AddEntropy(const UINT8 *Data, UINT32 Len) {
    for (UINT32 I = 0; I < Len; I++) {
        GRng.Pool[GRng.PoolIdx] ^= Data[I];
        GRng.PoolIdx = (GRng.PoolIdx + 1) % sizeof(GRng.Pool);
    }
}

static NOPTR CollectEntropy(NOPTR) {
    UINT64 Entropy = 0;
    
    /*
 * TSC (high precision timer)
 */
    Entropy ^= ReadTimeStampCounter();
    AddEntropy((UINT8*)&Entropy, sizeof(Entropy));
    
    /*
 * RDRAND if available
 */
    if (GRng.RdrandAvailable) {
        Entropy ^= Rdrand64();
        AddEntropy((UINT8*)&Entropy, sizeof(Entropy));
    }
    
    /*
 * Stack address (ASLR)
 */
    UINT64 StackPtr;
    asm volatile("mov %%rsp, %0" : "=r"(StackPtr));
    Entropy ^= StackPtr;
    AddEntropy((UINT8*)&Entropy, sizeof(Entropy));
    
    /*
 * CPU flags
 */
    UINT64 Flags;
    asm volatile("pushfq; popq %0" : "=r"(Flags));
    Entropy ^= Flags;
    AddEntropy((UINT8*)&Entropy, sizeof(Entropy));
}

/*
 * ============================================================================== RNG Initialization ================================================================================
 */

NOPTR RngInit(NOPTR) {
    MemSet(&GRng, 0, sizeof(GRng));
    
    GRng.RdrandAvailable = RdrandAvailable();
    
    /*
 * Collecting the initial entropy
 */
    for (INT I = 0; I < 32; I++) {
        CollectEntropy();
    }
    
    /*
 * Initializing ChaCha20 with a pool
 */
    ChaCha20Init(&GRng.Ctx, GRng.Pool, (UINT8[]){
        (GRng.Pool[0] ^ GRng.Pool[32]),
        (GRng.Pool[1] ^ GRng.Pool[33]),
        (GRng.Pool[2] ^ GRng.Pool[34]),
        (GRng.Pool[3] ^ GRng.Pool[35]),
        (GRng.Pool[4] ^ GRng.Pool[36]),
        (GRng.Pool[5] ^ GRng.Pool[37]),
        (GRng.Pool[6] ^ GRng.Pool[38]),
        (GRng.Pool[7] ^ GRng.Pool[39]),
        (GRng.Pool[8] ^ GRng.Pool[40]),
        (GRng.Pool[9] ^ GRng.Pool[41]),
        (GRng.Pool[10] ^ GRng.Pool[42]),
        (GRng.Pool[11] ^ GRng.Pool[43])
    });
    
    GRng.Initialized = TRUE;
    
    /*
 * First reseed
 */
    RngReseed();
}

/*
 * =============================================================================== Reseed =================================================================================== Reseed
 */

NOPTR RngReseed(NOPTR) {
    if (!GRng.Initialized) return;
    
    UINT8 NewKey[32];
    
    /*
 * Generating a new key from the current state
 */
    ChaCha20Xor(&GRng.Ctx, NewKey, sizeof(NewKey));
    
    /*
 * Collecting fresh entropy
 */
    CollectEntropy();
    
    /*
 * Mix with pool
 */
    for (INT I = 0; I < 32; I++) {
        NewKey[I] ^= GRng.Pool[I];
    }
    
    /*
 * New initialization
 */
    ChaCha20Init(&GRng.Ctx, NewKey, (UINT8[]){
        GRng.Pool[0], GRng.Pool[1], GRng.Pool[2], GRng.Pool[3],
        GRng.Pool[4], GRng.Pool[5], GRng.Pool[6], GRng.Pool[7],
        GRng.Pool[8], GRng.Pool[9], GRng.Pool[10], GRng.Pool[11]
    });
    
    GRng.ReseedCounter++;
    RngMemZero(NewKey, sizeof(NewKey));
}

/*
 * ============================================================================== Receiving random bytes ===============================================================================
 */

INT RngGetRandomBytes(UINT8 *Buf, UINT32 Len) {
    if (!Buf || Len == 0) return CRYPTO_ERR_INVALID_PARAM;
    if (!GRng.Initialized) return CRYPTO_ERR_HARDWARE_FAIL;
    
    /*
 * Every 1MB – reseed
 */
    if (GRng.ReseedCounter > 1024 * 1024 / 64) {
        RngReseed();
    }
    
    /*
 * Generating random bytes
 */
    ChaCha20Xor(&GRng.Ctx, Buf, Len);
    GRng.ReseedCounter += Len;
    
    return CRYPTO_OK;
}
