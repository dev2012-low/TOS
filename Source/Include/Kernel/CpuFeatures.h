#pragma once

#include <Kernel/Types.h>

INT CpuEnableSmepSmap(NOPTR);
INT CpuEnableUmip(NOPTR);
BOOL CpuHasAvx(NOPTR);
NOPTR CpuEnableXsave(NOPTR);