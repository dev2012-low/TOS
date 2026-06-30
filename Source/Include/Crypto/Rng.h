#pragma once

#include <Kernel/Types.h>

NOPTR RngInit(NOPTR);
NOPTR RngMemZero(NOPTR *Ptr, UINT32 Len);
INT RngGetRandomBytes(UINT8 *Buf, UINT32 Len);
NOPTR RngReseed(NOPTR);
