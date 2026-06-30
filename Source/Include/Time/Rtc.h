#pragma once

#include <Kernel/Types.h>

#define TIMEZONE_OFFSET 7

NOPTR ReadRtcTime(UINT32 *Hour, UINT32 *Minute, UINT32 *Second);
NOPTR ReadRtcDate(UINT32 *Year, UINT32 *Month, UINT32 *Day);