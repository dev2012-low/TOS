#pragma once

#include <Kernel/Types.h>
#include <Asm/Cpu.h>

typedef struct {
    volatile UINT32 Locked;
    UINT64 Flags;
} SpinLockIrq;

static inline NOPTR SpinLockIrqInit(SpinLockIrq *Lock) {
    Lock->Locked = 0;
    Lock->Flags = 0;
}

static inline NOPTR SpinLockIrqAcquire(SpinLockIrq *Lock) {
    Lock->Flags = ReadRflags();
    LocalInterruptsDisable();
    while (__sync_lock_test_and_set(&Lock->Locked, 1)) {
        CpuPause();
    }
}

static inline NOPTR SpinLockIrqRelease(SpinLockIrq *Lock) {
    __sync_lock_release(&Lock->Locked);
    WriteRflags(Lock->Flags);
}

static inline BOOL SpinLockIrqTryAcquire(SpinLockIrq *Lock) {
    if (__sync_lock_test_and_set(&Lock->Locked, 1) == 0) {
        Lock->Flags = ReadRflags();
        LocalInterruptsDisable();
        return TRUE;
    }
    return FALSE;
}