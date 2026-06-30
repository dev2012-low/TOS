#include <Kernel/Integrity.h>
#include <Kernel/Paging.h>
#include <Crypto/Sha256.h>
#include <Crypto/Rng.h>
#include <Kernel/SysStop.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Audit.h>

/* ============================================================================
 * Секции ядра (из линкера)
 * ============================================================================ */

extern UINT8 KernelTextStart[];
extern UINT8 KernelTextEnd[];
extern UINT8 KernelRodataStart[];
extern UINT8 KernelRodataEnd[];

/* Секция .checksum (встраивается скриптом KPatch.py) */
extern UINT8 __checksum_start[];
extern UINT8 __checksum_end[];

#define EXPECTED_HASH_SIZE 32

/* ============================================================================
 * Внутренние функции
 * ============================================================================ */

static void HashToHex(const UINT8 *Hash, CHAR *Buf, UINT32 BufSize) {
    if (!Hash || !Buf || BufSize < 65) return;
    for (INT i = 0; i < 32 && i * 2 + 2 < BufSize; i++) {
        CHAR Hex[3];
        SnPrintf(Hex, sizeof(Hex), "%02x", Hash[i]);
        Buf[i * 2] = Hex[0];
        Buf[i * 2 + 1] = Hex[1];
    }
    Buf[64] = '\0';
}

/* ============================================================================
 * Public Functions
 * ============================================================================ */

INT CheckKernelIntegrity(NOPTR) {
    UINT8 Calculated[32];
    UINT8 Expected[32];
    CHAR ExpectedHex[65];
    CHAR CalculatedHex[65];
    
    /* ============================================================
     * 1. Проверяем, что секция .checksum существует
     * ============================================================ */
    USIZE ChecksumSize = (USIZE)(__checksum_end - __checksum_start);
    if (ChecksumSize < EXPECTED_HASH_SIZE) {
        AuditLog(AUDIT_LEVEL_CRITICAL, AUDIT_EVENT_COMMAND,
                 "Kernel integrity: .checksum section too small (%u bytes)",
                 ChecksumSize);
        return -1;
    }
    
    /* ============================================================
     * 2. Копируем ожидаемый хеш из .checksum
     * ============================================================ */
    MemCpy(Expected, __checksum_start, EXPECTED_HASH_SIZE);
    
    /* ============================================================
     * 3. Вычисляем хеш .text + .rodata
     * ============================================================ */
    Sha256Ctx Ctx;
    Sha256Init(&Ctx);
    Sha256Update(&Ctx, KernelTextStart, KernelTextEnd - KernelTextStart);
    Sha256Update(&Ctx, KernelRodataStart, KernelRodataEnd - KernelRodataStart);
    Sha256Final(&Ctx, Calculated);
    
    /* ============================================================
     * 4. Сравниваем
     * ============================================================ */
    if (SecureMemCmp(Calculated, Expected, EXPECTED_HASH_SIZE) == 0) {
        return 0;
    }
    
    /* ============================================================
     * 5. Хеши не совпадают — ядро повреждено!
     * ============================================================ */
    SysStop("KERNEL_INTEGRITY_CHECK_FAILED");
}

/* ============================================================================
 * GetKernelHash
 * ============================================================================ */

NOPTR GetKernelHash(UINT8 *OutHash, UINT32 *OutSize) {
    if (!OutHash || !OutSize) return;
    
    USIZE ChecksumSize = (USIZE)(__checksum_end - __checksum_start);
    if (ChecksumSize < EXPECTED_HASH_SIZE) {
        *OutSize = 0;
        return;
    }
    
    MemCpy(OutHash, __checksum_start, EXPECTED_HASH_SIZE);
    *OutSize = EXPECTED_HASH_SIZE;
}