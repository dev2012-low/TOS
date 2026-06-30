#pragma once

#include <Kernel/Types.h>


INT CheckKernelIntegrity(NOPTR);
NOPTR GetKernelHash(UINT8 *OutHash, UINT32 *OutSize);