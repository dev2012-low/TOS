#pragma once

#include <FBDevice.h>
#include <Kernel/Types.h>

EXTERN(FBDevice, GFBDevice);

BOOL ClipCoord(INT32 *X, INT32 *Y);
NOPTR Swap(INT32 *A, INT32 *B);
EXTERN(NOPTR, FBDeviceFlush(NOPTR));