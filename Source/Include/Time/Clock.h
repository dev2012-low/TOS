#pragma once

#include <Kernel/Types.h>

typedef struct {
    UINT8 Hh;
    UINT8 Mm;
    UINT8 Ss;
    UINT32 Epoch;
} ClockTime;

NOPTR ClockTick(NOPTR);
NOPTR FormatClock(CHAR *Buffer, ClockTime T);
NOPTR InitSystemClock(NOPTR);