#pragma once

#include <Kernel/Types.h>

static inline UINT8 Inb(UINT16 Port)
{
    UINT8 Ret;
    asm volatile("inb %1, %0" : "=a"(Ret) : "Nd"(Port));
    return Ret;
}

static inline NOPTR Outb(UINT16 Port, UINT8 Data)
{
    asm volatile("outb %0, %1" : : "a"(Data), "Nd"(Port));
}

static inline UINT16 Inw(UINT16 Port)
{
    UINT16 Ret;
    asm volatile("inw %1, %0" : "=a"(Ret) : "Nd"(Port));
    return Ret;
}

static inline NOPTR Outw(UINT16 Port, UINT16 Data)
{
    asm volatile("outw %0, %1" : : "a"(Data), "Nd"(Port));
}

static inline UINT32 Inl(UINT16 Port)
{
    UINT32 Ret;
    asm volatile("inl %1, %0" : "=a"(Ret) : "Nd"(Port));
    return Ret;
}

static inline NOPTR Outl(UINT16 Port, UINT32 Data)
{
    asm volatile("outl %0, %1" : : "a"(Data), "Nd"(Port));
}

static inline NOPTR IoWait(NOPTR) {
    Outb(0x80, 0);  // Write to unused port 0x80 (POST card)
}