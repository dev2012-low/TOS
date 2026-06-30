#pragma once

#include <Kernel/Types.h>
#include <ApicRegs.h>

/*
 * ============================================================================= Local APIC ============================================================================
 */

INT ApicInit(UINT32 BaseAddr);
NOPTR ApicEnable(NOPTR);
NOPTR ApicDisable(NOPTR);
NOPTR ApicEoi(NOPTR);

UINT32 ApicReadReg(UINT32 Reg);
NOPTR ApicWriteReg(UINT32 Reg, UINT32 Val);

UINT32 ApicGetId(NOPTR);
UINT32 ApicGetVersion(NOPTR);

BOOL ApicIsX2ApicMode(NOPTR);
BOOL ApicCpuSupportsX2Apic(NOPTR);
UINT32 ApicFormatIoapicDestination(UINT32 ApicId);

/*
 * IPI
 */
NOPTR ApicSendIpi(UINT32 ApicId, UINT32 Vector);
NOPTR ApicSendBroadcast(UINT32 Vector);
NOPTR ApicSendInit(UINT32 ApicId);
NOPTR ApicSendStartup(UINT32 ApicId, UINT32 Vector);