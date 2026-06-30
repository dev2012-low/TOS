#pragma once

#include <Kernel/Types.h>
#include <Pci.h>

/*
 * ============================================================================
 * PCI IDs for NVIDIA GPUs (Tesla to Pascal)
 * ============================================================================
 */
#define NVIDIA_VENDOR_ID         0x10DE

/* Tesla (G80-G200) */
#define NVIDIA_G80               0x0600
#define NVIDIA_G200              0x05E0

/* Fermi (GF100-GF119) */
#define NVIDIA_GF100             0x06C0
#define NVIDIA_GF104             0x06E0
#define NVIDIA_GF106             0x0CA0
#define NVIDIA_GF108             0x0DE0

/* Kepler (GK104-GK208) */
#define NVIDIA_GK104             0x1180
#define NVIDIA_GK106             0x11C0
#define NVIDIA_GK107             0x1185
#define NVIDIA_GK110             0x1020

/* Maxwell (GM107-GM206) */
#define NVIDIA_GM107             0x1380
#define NVIDIA_GM204             0x13C0
#define NVIDIA_GM206             0x1400
#define NVIDIA_GM200             0x15F0

/* Pascal (GP102-GP108) */
#define NVIDIA_GP102             0x1B00
#define NVIDIA_GP104             0x1B80
#define NVIDIA_GP106             0x1C00
#define NVIDIA_GP107             0x1C80
#define NVIDIA_GP108             0x1D00

/*
 * ============================================================================
 * PCI BARs
 * ============================================================================
 */
#define NVIDIA_BAR_MMIO          0
#define NVIDIA_BAR_FB            2
#define NVIDIA_BAR_ROM           3
#define NVIDIA_BAR_GART          4

/*
 * ============================================================================
 * NVIDIA Display Registers
 * ============================================================================
 */
#define NV_PMC_BASE              0x00010000
#define NV_PMC_INTR              (NV_PMC_BASE + 0x20)
#define NV_PMC_INTR_ENABLE       (NV_PMC_BASE + 0x24)
#define NV_PMC_INTR_STATUS       (NV_PMC_BASE + 0x28)

#define NV_PDISPLAY_BASE         0x00610000
#define NV_PDISPLAY_SURFACE      0x0061080C
#define NV_PDISPLAY_SURFACE_PITCH 0x00610810
#define NV_PDISPLAY_SURFACE_FORMAT 0x00610814

#define NV_PCRTC0_BASE           0x00610800
#define NV_PCRTC0_CONTROL        (NV_PCRTC0_BASE + 0x00)
#define NV_PCRTC0_TIMING         (NV_PCRTC0_BASE + 0x04)
#define NV_PCRTC0_SIZE           (NV_PCRTC0_BASE + 0x08)

#define NV_PBUS_BASE             0x00001000
#define NV_PBUS_PCI_NV_19        (NV_PBUS_BASE + 0x0190)

#define NV_PRAMDAC_BASE          0x00680000
#define NV_PRAMDAC_PLL           (NV_PRAMDAC_BASE + 0x500)
#define NV_PRAMDAC_PLL_COEFF     (NV_PRAMDAC_BASE + 0x504)
#define NV_PRAMDAC_PLL_STATUS    (NV_PRAMDAC_BASE + 0x508)

/*
 * ============================================================================
 * Bit fields
 * ============================================================================
 */
#define NV_PCRTC_CONTROL_ENABLE  (1 << 0)
#define NV_PCRTC_CONTROL_VSYNC   (1 << 8)

#define SURFACE_FORMAT_RGB565    (0x00 << 0)
#define SURFACE_FORMAT_XRGB8888  (0x04 << 0)
#define SURFACE_FORMAT_ARGB8888  (0x05 << 0)

#define NV_PRAMDAC_PLL_ENABLE    (1 << 0)
#define NV_PRAMDAC_PLL_LOCK      (1 << 16)

#define NV_PMC_INTR_VBLANK0      (1 << 0)
#define NV_PMC_INTR_VBLANK1      (1 << 1)

#define GART_PAGE_SIZE           4096
#define GART_ENTRY_COUNT         1024
#define GART_ENTRY_VALID         (1 << 0)
#define GART_ENTRY_WRITEABLE     (1 << 1)
#define GART_ENTRY_READABLE      (1 << 2)

/*
 * ============================================================================
 * GPU Generation
 * ============================================================================
 */
typedef enum {
    NVIDIA_GEN_TESLA = 0,
    NVIDIA_GEN_FERMI,
    NVIDIA_GEN_KEPLER,
    NVIDIA_GEN_MAXWELL,
    NVIDIA_GEN_PASCAL,
    NVIDIA_GEN_TURING,
    NVIDIA_GEN_AMPERE,
    NVIDIA_GEN_ADA,
    NVIDIA_GEN_UNKNOWN
} NvidiaGeneration;

/*
 * ============================================================================
 * Device Context
 * ============================================================================
 */
typedef struct NvidiaDevice {
    volatile NOPTR *MmioBase;
    volatile NOPTR *FbBase;
    volatile UINT32 *Gart;
    UINT32 GartTablePhys;
    
    PciDevice *PciDev;
    UINT32 DeviceId;
    NvidiaGeneration Generation;
    UINT32 GartEntries;
    UINT32 VramSize;
    UINT32 Crtc;
    
    UINT32 Irq;
    UINT32 Gsi;
    UINT8 Vector;
    BOOL MsiEnabled;
    BOOL GartIsAperture;
    BOOL Initialized;
    BOOL Active;
    BOOL DualLink;
    
    NOPTR *Framebuffer;
    UINT32 FbWidth;
    UINT32 FbHeight;
    UINT32 FbStride;
    UINT32 FbFormat;
    
    UINT32 PllM, PllN, PllP;
    
    /* GDS integration */
    NOPTR *GdsDevice;
} NvidiaDevice;

/*
 * ============================================================================
 * Prototypes
 * ============================================================================
 */
INT NvidiaInit(PciDevice *PciDev);
INT NvidiaInitAll(NOPTR);
NOPTR NvidiaIrqHandler(NOPTR);
NOPTR NvidiaIrqStub(NOPTR);