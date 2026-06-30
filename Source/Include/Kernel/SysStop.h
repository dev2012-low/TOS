#pragma once

#include <Kernel/Types.h>

typedef struct {
    UINT64 Rsp, Rip, Rflags;
    UINT16 Cs, Ds, Es, Fs, Gs, Ss;
    UINT64 Cr0, Cr2, Cr3, Cr4;
} SysStopRegisterState;

void SysStopImpl(const char *Message, void *CallerRip);

#define SysStop(Message) SysStopImpl(Message, __builtin_return_address(0))