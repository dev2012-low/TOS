#pragma once

#include <Kernel/Types.h>
#include <Kernel/List.h>

/*
 * ============================================================================= IOAPIC Driver API ============================================================================
 */

/*
 * Flags for interrupt configuration
 */
#define IOAPIC_FLAG_ACTIVE_LOW      (1 << 0)
#define IOAPIC_FLAG_LEVEL_TRIGGERED (1 << 1)
#define IOAPIC_FLAG_ACTIVE_HIGH     (0 << 0)
#define IOAPIC_FLAG_EDGE_TRIGGERED  (0 << 1)
#define IOAPIC_REDTBL_BASE       0x10

typedef struct {
    UINT32 Gsi;
    UINT32 Flags;
    BOOL Valid;
} IoapicOverride;

typedef struct IoapicDevice {
    ListHead Node;
    UINT32 Id;
    UINT32 Address;
    UINT32 GsiBase;
    UINT32 Version;
    UINT32 MaxRedir;
    volatile NOPTR *VirtAddr;
    BOOL Enabled;
} IoapicDevice;

EXTERN(IoapicOverride, IoapicOverrides[256]);

/*
 * ============================================================================= IOAPIC Management Functions ============================================================================
 */

/*
 * Get number of IOAPICs in system
 */
UINT32 IoapicGetCount(NOPTR);

/*
 * Get IOAPIC version by index
 */
UINT32 IoapicGetVersion(UINT32 Index);

NOPTR IoapicGetRedirection(IoapicDevice *Ioapic, UINT32 Index,
                                    UINT32 *Low, UINT32 *High);

/*
 * Get GSI base for IOAPIC by index
 */
UINT32 IoapicGetGsiBase(UINT32 Index);

/*
 * Get number of IRQs supported by IOAPIC
 */
UINT32 IoapicGetIrqCount(UINT32 Index);

/*
 * ============================================================================= IRQ Routing Functions ============================================================================
 */

/*
 * Redirect a GSI to a specific vector on a specific APIC
 */
INT IoapicRedirectIrq(UINT32 Gsi, UINT8 Vector, UINT32 ApicId, UINT32 Flags);

/*
 * Remove redirection for a GSI
 */
INT IoapicUnredirectIrq(UINT32 Gsi);

/*
 * Mask/unmask specific IRQ
 */
NOPTR IoapicMaskIrq(UINT32 Gsi);
NOPTR IoapicUnmaskIrq(UINT32 Gsi);

INT IoapicInit(NOPTR);

/*
 * Mask/unmask all IRQs on all IOAPICs
 */
NOPTR IoapicMaskAll(NOPTR);
NOPTR IoapicUnmaskAll(NOPTR);

/*
 * Send EOI for level-triggered interrupt
 */
NOPTR IoapicEoi(UINT32 Gsi);

/*
 * Process interrupt source overrides from MADT
 */
INT IoapicProcessOverrides(NOPTR);

INT IoapicGetOverride(UINT32 Source, UINT32 *Gsi, UINT32 *Flags);

NOPTR IoapicReadRedirection(NOPTR *Ioapic, UINT32 Index, UINT32 *Low, UINT32 *High);
IoapicDevice* IoapicGetFirst(NOPTR);