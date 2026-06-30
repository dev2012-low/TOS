#pragma once

#include <Kernel/Types.h>
#include <Asm/Cpu.h>

typedef struct {
    volatile UINT32 Locked;
} SpinLock;

static inline NOPTR SpinLockInit(SpinLock *Lock) {
    Lock->Locked = 0;
}

static inline NOPTR SpinLockAcquire(SpinLock *Lock) {
    while (__sync_lock_test_and_set(&Lock->Locked, 1)) {
        CpuPause();
    }
}

static inline NOPTR SpinLockRelease(SpinLock *Lock) {
    __sync_lock_release(&Lock->Locked);
}

static inline NOPTR SpinLockAcquireUINT32(volatile UINT32 *Lock) {
    while (__sync_lock_test_and_set(Lock, 1)) {
        while (*Lock) CpuPause();
    }
    __sync_synchronize();
}

static inline NOPTR SpinLockReleaseUINT32(volatile UINT32 *Lock) {
    __sync_synchronize();
    __sync_lock_release(Lock);
}

static inline BOOL SpinLockTryAcquire(SpinLock *Lock) {
    return __sync_lock_test_and_set(&Lock->Locked, 1) == 0;
}
