#include <Gpu/Nvidia.h>
#include <Gdl/Gdl.h>
#include <Pci.h>
#include <Asm/Mmio.h>
#include <Asm/Io.h>
#include <Memory/PhysAlloc.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Console.h>
#include <Time/Timer.h>
#include <Kernel/Return.h>
#include <Kernel/KDriver.h>
#include <Ioapic.h>
#include <Apic.h>
#include <Kernel/Idt.h>

/*
 * ============================================================================
 * Global State
 * ============================================================================
 */
static NvidiaDevice *GPrimaryNvidia = NULLPTR;
static KDriver *GNvidiaDriver = NULLPTR;

/*
 * ============================================================================
 * MMIO Helpers
 * ============================================================================
 */
static inline UINT32 NvidiaRead32(NvidiaDevice *Dev, UINT32 Reg) {
    return MmioRead32((volatile UINT32*)((UINTPTR)Dev->MmioBase + Reg));
}

static inline NOPTR NvidiaWrite32(NvidiaDevice *Dev, UINT32 Reg, UINT32 Val) {
    MmioWrite32((volatile UINT32*)((UINTPTR)Dev->MmioBase + Reg), Val);
}

static inline NOPTR NvidiaSetBit32(NvidiaDevice *Dev, UINT32 Reg, UINT32 Bit) {
    UINT32 Val = NvidiaRead32(Dev, Reg);
    NvidiaWrite32(Dev, Reg, Val | Bit);
}

static inline NOPTR NvidiaClearBit32(NvidiaDevice *Dev, UINT32 Reg, UINT32 Bit) {
    UINT32 Val = NvidiaRead32(Dev, Reg);
    NvidiaWrite32(Dev, Reg, Val & ~Bit);
}

/*
 * ============================================================================
 * Generation Detection
 * ============================================================================
 */
static NvidiaGeneration NvidiaDetectGeneration(UINT32 DeviceId) {
    /* Newer generations first (avoid overlap with older broad ranges) */
    
    /* Ada Lovelace */
    if (DeviceId >= 0x2600 && DeviceId <= 0x28FF) {
        return NVIDIA_GEN_ADA;
    }
    
    /* Ampere */
    if (DeviceId >= 0x2200 && DeviceId <= 0x25FF) {
        return NVIDIA_GEN_AMPERE;
    }
    
    /* Turing */
    if (DeviceId >= 0x1E00 && DeviceId <= 0x1FFF) {
        return NVIDIA_GEN_TURING;
    }
    
    /* Pascal (GP102-GP108) */
    if ((DeviceId >= 0x1B00 && DeviceId <= 0x1BFF) ||
        (DeviceId >= 0x1C00 && DeviceId <= 0x1CFF) ||
        (DeviceId >= 0x1D00 && DeviceId <= 0x1DFF)) {
        return NVIDIA_GEN_PASCAL;
    }
    
    /* Maxwell (GM107-GM206) */
    if ((DeviceId >= 0x1380 && DeviceId <= 0x13FF) ||
        (DeviceId >= 0x13C0 && DeviceId <= 0x13FF) ||
        (DeviceId >= 0x1400 && DeviceId <= 0x14FF) ||
        (DeviceId >= 0x15F0 && DeviceId <= 0x15FF)) {
        return NVIDIA_GEN_MAXWELL;
    }
    
    /* Kepler (GK104-GK208) — before Fermi overlap at 0x10xx */
    if ((DeviceId >= 0x1180 && DeviceId <= 0x11FF) ||
        (DeviceId >= 0x1020 && DeviceId <= 0x107F) ||
        (DeviceId >= 0x1280 && DeviceId <= 0x12FF)) {
        return NVIDIA_GEN_KEPLER;
    }
    
    /* Fermi (GF100-GF119) — before Tesla overlap at 0x06xx */
    if ((DeviceId >= 0x06C0 && DeviceId <= 0x06FF) ||
        (DeviceId >= 0x0CA0 && DeviceId <= 0x0CFF) ||
        (DeviceId >= 0x1080 && DeviceId <= 0x10FF) ||
        (DeviceId >= 0x1200 && DeviceId <= 0x12FF)) {
        return NVIDIA_GEN_FERMI;
    }
    
    /* Tesla (G80-G200) */
    if ((DeviceId >= 0x0600 && DeviceId <= 0x06BF) ||
        (DeviceId >= 0x05E0 && DeviceId <= 0x05FF)) {
        return NVIDIA_GEN_TESLA;
    }
    
    return NVIDIA_GEN_UNKNOWN;
}

/*
 * ============================================================================
 * Pixel Clock Calculation
 * ============================================================================
 */
static UINT32 NvidiaCalcPixelClock(UINT32 Width, UINT32 Height, UINT32 Refresh, BOOL DualLink) {
    UINT32 HTotal = Width + 160;
    UINT32 VTotal = Height + 30;
    UINT32 PixelClock = (HTotal * VTotal * Refresh) / 1000;
    
    if (DualLink) {
        PixelClock *= 2;
    }
    
    if (PixelClock < 10000) PixelClock = 10000;
    if (PixelClock > 400000) PixelClock = 400000;
    
    return PixelClock;
}

/*
 * ============================================================================
 * VBIOS PLL Reading
 * ============================================================================
 */
static NOPTR NvidiaReadPllFromVbios(NvidiaDevice *Dev) {
    UINT32 VbiosPhys = (UINT32)(Dev->PciDev->Bars[NVIDIA_BAR_ROM] & ~0x3);
    UINT32 RomSize = (UINT32)Dev->PciDev->BarSizes[NVIDIA_BAR_ROM];
    
    Dev->PllM = 24;
    Dev->PllN = 1;
    Dev->PllP = 1;
    
    if (!VbiosPhys) {
        ConsolePrint("[NVIDIA] No VBIOS ROM, using defaults\n");
        return;
    }
    
    if (!RomSize || RomSize > 0x200000) {
        RomSize = 0x10000;
    }
    
    volatile UINT8 *Vbios = (volatile UINT8*)(UINTPTR)VbiosPhys;
    
    for (UINT32 Offset = 0; Offset + 0x23 < RomSize; Offset += 512) {
        if (Vbios[Offset] == 0x55 && Vbios[Offset + 1] == 0xAA) {
            Dev->PllM = Vbios[Offset + 0x20];
            Dev->PllN = Vbios[Offset + 0x21];
            Dev->PllP = Vbios[Offset + 0x22];
            
            if (Dev->PllM == 0) Dev->PllM = 24;
            if (Dev->PllN == 0) Dev->PllN = 1;
            if (Dev->PllP == 0) Dev->PllP = 1;
            
            ConsolePrint("[NVIDIA] VBIOS PLL: M=%u, N=%u, P=%u\n",
                        Dev->PllM, Dev->PllN, Dev->PllP);
            return;
        }
    }
    
    ConsolePrint("[NVIDIA] No valid VBIOS found, using defaults\n");
}

/*
 * ============================================================================
 * PLL Setup
 * ============================================================================
 */
static INT NvidiaSetupPll(NvidiaDevice *Dev, UINT32 Width, UINT32 Height, UINT32 Refresh) {
    UINT32 PixelClock = NvidiaCalcPixelClock(Width, Height, Refresh, Dev->DualLink);
    UINT32 PllCoeff = (Dev->PllM << 16) | (Dev->PllN << 8) | Dev->PllP;
    INT Timeout = 10000;
    
    NvidiaWrite32(Dev, NV_PRAMDAC_PLL_COEFF, PllCoeff);
    NvidiaSetBit32(Dev, NV_PRAMDAC_PLL, NV_PRAMDAC_PLL_ENABLE);
    
    while (Timeout--) {
        if (NvidiaRead32(Dev, NV_PRAMDAC_PLL_STATUS) & NV_PRAMDAC_PLL_LOCK) {
            ConsolePrint("[NVIDIA] PLL locked: %u kHz, coeff=0x%X\n", PixelClock, PllCoeff);
            RETURN(SUCCESS);
        }
        TimerUdelay(10);
    }
    
    ConsolePrint("[NVIDIA] PLL lock timeout!\n");
    RETURN(TIMEOUT);
}

/*
 * ============================================================================
 * CRTC Timing Setup
 * ============================================================================
 */
static NOPTR NvidiaCrtcSetTiming(NvidiaDevice *Dev, UINT32 Width, UINT32 Height, UINT32 Refresh) {
    GdlMode *Mode = GdlModeCreate(Width, Height, Refresh ? Refresh : 60);
    UINT32 HTotal, VTotal;
    
    if (Mode) {
        HTotal = Mode->HTotal;
        VTotal = Mode->VTotal;
        MemoryFree(Mode);
    } else {
        HTotal = Width + 160;
        VTotal = Height + 30;
    }
    
    UINT32 Timing = ((HTotal - 1) << 16) | (Width - 1);
    UINT32 Size = ((Height - 1) << 16) | (Width - 1);
    
    NvidiaWrite32(Dev, NV_PCRTC0_TIMING, Timing);
    NvidiaWrite32(Dev, NV_PCRTC0_SIZE, Size);
    
    ConsolePrint("[NVIDIA] CRTC timing: %ux%u@%u (HT=%u, VT=%u)\n",
                Width, Height, Refresh ? Refresh : 60, HTotal, VTotal);
}

/*
 * ============================================================================
 * CRTC Enable/Disable
 * ============================================================================
 */
static NOPTR NvidiaCrtcEnable(NvidiaDevice *Dev, BOOL Enable) {
    if (!Dev) return;
    
    if (Enable) {
        NvidiaSetBit32(Dev, NV_PCRTC0_CONTROL, NV_PCRTC_CONTROL_ENABLE);
        TimerMdelay(20);
        Dev->Active = TRUE;
        ConsolePrint("[NVIDIA] CRTC enabled\n");
    } else {
        NvidiaClearBit32(Dev, NV_PCRTC0_CONTROL, NV_PCRTC_CONTROL_ENABLE);
        Dev->Active = FALSE;
        ConsolePrint("[NVIDIA] CRTC disabled\n");
    }
}

/*
 * ============================================================================
 * GART Management
 * ============================================================================
 */
static NOPTR NvidiaGartWrite(NvidiaDevice *Dev, UINT32 Index, UINT64 PhysAddr) {
    UINT32 Entry = (PhysAddr & ~0xFFF) | GART_ENTRY_VALID | GART_ENTRY_WRITEABLE;
    
    if (!Dev->GartIsAperture) {
        Entry |= GART_ENTRY_READABLE;
    }
    
    Dev->Gart[Index] = Entry;
}

static NOPTR NvidiaGartInit(NvidiaDevice *Dev, UINT64 FbPhys, UINT32 FbSize) {
    UINT32 Pages = (FbSize + GART_PAGE_SIZE - 1) / GART_PAGE_SIZE;
    
    if (Pages > Dev->GartEntries) {
        Pages = Dev->GartEntries;
    }
    
    for (UINT32 I = 0; I < Pages; I++) {
        UINT64 PagePhys = FbPhys + (I * GART_PAGE_SIZE);
        NvidiaGartWrite(Dev, I, PagePhys);
    }
    
    ConsolePrint("[NVIDIA] GART: %u pages (%s)\n", Pages,
                Dev->GartIsAperture ? "aperture" : "table");
}

static INT NvidiaSetupGart(NvidiaDevice *Dev, PciDevice *PciDev) {
    UINT32 BarGart;
    
    Dev->GartEntries = GART_ENTRY_COUNT;
    Dev->GartIsAperture = FALSE;
    
    /* Tesla: prefer BAR4 GART window when available */
    if (Dev->Generation <= NVIDIA_GEN_TESLA) {
        BarGart = (UINT32)(PciDev->Bars[NVIDIA_BAR_GART] & ~0xFFF);
        if (BarGart) {
            Dev->GartTablePhys = BarGart;
            Dev->Gart = (volatile UINT32*)(UINTPTR)BarGart;
            ConsolePrint("[NVIDIA] GART via BAR4 at 0x%X\n", BarGart);
            goto ClearGart;
        }
    }
    
    /* Allocate system RAM page table and program GPU */
    Dev->GartTablePhys = (UINT32)(UINTPTR)PhysAllocAllocatePage(PhysAllocGet());
    if (!Dev->GartTablePhys) {
        ConsolePrint("[NVIDIA] GART allocation failed\n");
        RETURN(NO_MEMORY);
    }
    
    Dev->Gart = (volatile UINT32*)(UINTPTR)Dev->GartTablePhys;
    ConsolePrint("[NVIDIA] GART table at phys 0x%X\n", Dev->GartTablePhys);
    
    if (Dev->Generation >= NVIDIA_GEN_FERMI) {
        NvidiaWrite32(Dev, NV_PBUS_PCI_NV_19, Dev->GartTablePhys | 1);
    }
    
ClearGart:
    for (UINT32 I = 0; I < Dev->GartEntries; I++) {
        Dev->Gart[I] = 0;
    }
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * MSI Setup
 * ============================================================================
 */
static INT NvidiaEnableMsi(NvidiaDevice *Dev) {
    UINT8 MsiCap = PciFindCap(Dev->PciDev, PCI_CAP_ID_MSI);
    
    if (!MsiCap) {
        ConsolePrint("[NVIDIA] No MSI capability\n");
        RETURN(NOT_SUPPORTED);
    }
    
    Dev->Vector = 48 + (Dev->Irq % 16);
    
    if (PciEnableMsi(Dev->PciDev, Dev->Vector, ApicGetId()) != SUCCESS) {
        ConsolePrint("[NVIDIA] MSI enable failed\n");
        RETURN(IO_ERROR);
    }
    
    Dev->MsiEnabled = TRUE;
    ConsolePrint("[NVIDIA] MSI enabled, vector=%u\n", Dev->Vector);
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * IRQ Setup
 * ============================================================================
 */
static INT NvidiaSetupIrq(NvidiaDevice *Dev) {
    UINT32 Flags;
    
    Dev->Irq = Dev->PciDev->InterruptLine;
    
    /* Try MSI first */
    if (NvidiaEnableMsi(Dev) == SUCCESS) {
        IdtSetGate(Dev->Vector, NvidiaIrqStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
        RETURN(SUCCESS);
    }
    
    /* Fallback to legacy IRQ */
    if (IoapicGetOverride(Dev->Irq, &Dev->Gsi, &Flags) != SUCCESS) {
        Dev->Gsi = Dev->Irq;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }
    
    Dev->Vector = 40 + (Dev->Gsi % 16);
    
    if (IoapicRedirectIrq(Dev->Gsi, Dev->Vector, ApicGetId(), Flags) != SUCCESS) {
        ConsolePrint("[NVIDIA] Failed to redirect IRQ %u\n", Dev->Irq);
        RETURN(IO_ERROR);
    }
    
    IdtSetGate(Dev->Vector, NvidiaIrqStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IoapicUnmaskIrq(Dev->Gsi);
    
    ConsolePrint("[NVIDIA] Legacy IRQ %u (GSI %u) -> vector %u\n",
                Dev->Irq, Dev->Gsi, Dev->Vector);
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Public Display Functions
 * ============================================================================
 */
INT NvidiaSetMode(NvidiaDevice *Dev, UINT32 Width, UINT32 Height, UINT32 Bpp) {
    if (!Dev) RETURN(NO_OBJECT);
    
    Dev->FbWidth = Width;
    Dev->FbHeight = Height;
    Dev->FbStride = Width * (Bpp / 8);
    
    /* Check dual-link for high resolutions */
    if (Width > 1920 && Dev->Generation <= NVIDIA_GEN_KEPLER) {
        Dev->DualLink = TRUE;
        ConsolePrint("[NVIDIA] Dual-link DVI enabled for %ux%u\n", Width, Height);
    } else {
        Dev->DualLink = FALSE;
    }
    
    NvidiaCrtcEnable(Dev, FALSE);
    NvidiaReadPllFromVbios(Dev);
    
    if (NvidiaSetupPll(Dev, Width, Height, 60) != SUCCESS) {
        RETURN(DEVICE_ERROR);
    }
    
    NvidiaCrtcSetTiming(Dev, Width, Height, 60);
    
    /* Surface format */
    UINT32 Format;
    switch (Dev->FbFormat) {
        case SURFACE_FORMAT_RGB565:
            Format = SURFACE_FORMAT_RGB565;
            break;
        case SURFACE_FORMAT_ARGB8888:
            Format = SURFACE_FORMAT_ARGB8888;
            break;
        default:
            Format = SURFACE_FORMAT_XRGB8888;
            break;
    }
    
    NvidiaWrite32(Dev, NV_PDISPLAY_SURFACE_PITCH, Dev->FbStride);
    NvidiaWrite32(Dev, NV_PDISPLAY_SURFACE_FORMAT, Format);
    NvidiaWrite32(Dev, NV_PDISPLAY_SURFACE, 0);
    
    ConsolePrint("[NVIDIA] Mode: %ux%u@%u bpp (Gen%u, dual=%s)\n",
                Width, Height, Bpp, Dev->Generation, Dev->DualLink ? "yes" : "no");
    
    RETURN(SUCCESS);
}

INT NvidiaSetFramebuffer(NvidiaDevice *Dev, UINT64 PhysAddr, UINT32 Stride) {
    if (!Dev) RETURN(NO_OBJECT);
    
    NvidiaGartInit(Dev, PhysAddr, Stride * Dev->FbHeight);
    NvidiaWrite32(Dev, NV_PDISPLAY_SURFACE, 0);
    Dev->FbStride = Stride;
    
    ConsolePrint("[NVIDIA] Framebuffer: phys=0x%llX, stride=%u\n", PhysAddr, Stride);
    
    RETURN(SUCCESS);
}

INT NvidiaEnable(NvidiaDevice *Dev, BOOL Enable) {
    if (!Dev) RETURN(NO_OBJECT);
    
    NvidiaCrtcEnable(Dev, Enable);
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * IRQ Handler
 * ============================================================================
 */
NOPTR NvidiaIrqHandler(NOPTR) {
    NvidiaDevice *Dev = GPrimaryNvidia;
    UINT32 Intr;
    
    if (!Dev || !Dev->Initialized) {
        return;
    }
    
    Intr = NvidiaRead32(Dev, NV_PMC_INTR_STATUS);
    
    if (Intr & NV_PMC_INTR_VBLANK0) {
        /* VBlank - can do page flip here */
        NvidiaWrite32(Dev, NV_PMC_INTR_STATUS, NV_PMC_INTR_VBLANK0);
    }
    
    if (!Dev->MsiEnabled && Dev->Gsi) {
        IoapicEoi(Dev->Gsi);
    }
    
    ApicEoi();
}

/*
 * ============================================================================
 * GDL Integration
 * ============================================================================
 */
static INT NvidiaGdlSetMode(GdlCrtc *Crtc, GdlMode *Mode) {
    NvidiaDevice *Dev = (NvidiaDevice*)Crtc->Priv;
    
    if (!Dev || !Mode) return GDL_ERR_INVALID_PARAM;
    
    return NvidiaSetMode(Dev, Mode->Width, Mode->Height, 32);
}

static INT NvidiaGdlSetFb(GdlCrtc *Crtc, GdlFramebuffer *Fb) {
    NvidiaDevice *Dev = (NvidiaDevice*)Crtc->Priv;
    
    if (!Dev || !Fb) return GDL_ERR_INVALID_PARAM;
    
    return NvidiaSetFramebuffer(Dev, Fb->PAddr, Fb->Pitch);
}

static NOPTR NvidiaGdlEnable(GdlCrtc *Crtc) {
    NvidiaDevice *Dev = (NvidiaDevice*)Crtc->Priv;
    NvidiaEnable(Dev, TRUE);
}

static NOPTR NvidiaGdlDisable(GdlCrtc *Crtc) {
    NvidiaDevice *Dev = (NvidiaDevice*)Crtc->Priv;
    NvidiaEnable(Dev, FALSE);
}

/*
 * ============================================================================
 * GDL Device Registration
 * ============================================================================
 */
static INT NvidiaRegisterGdl(NvidiaDevice *Dev) {
    GdlDevice *Gpu;
    GdlCrtc *Crtc;
    
    Gpu = (GdlDevice*)MemoryAllocate(sizeof(GdlDevice));
    if (!Gpu) RETURN(NO_MEMORY);
    
    MemSet(Gpu, 0, sizeof(GdlDevice));
    SnPrintf(Gpu->Name, sizeof(Gpu->Name), "NVIDIA Gen%u", Dev->Generation);
    Gpu->VendorId = NVIDIA_VENDOR_ID;
    Gpu->DeviceId = Dev->DeviceId;
    Gpu->PciDev = Dev->PciDev;
    Gpu->MmioBase = Dev->MmioBase;
    Gpu->Priv = Dev;
    Gpu->Initialized = TRUE;
    
    ListInit(&Gpu->Crtcs);
    ListInit(&Gpu->Encoders);
    ListInit(&Gpu->Connectors);
    
    /* Create CRTC */
    Crtc = GdlCrtcCreate(Gpu, 0);
    if (!Crtc) {
        MemoryFree(Gpu);
        RETURN(NO_MEMORY);
    }
    
    Crtc->Priv = Dev;
    Crtc->SetMode = NvidiaGdlSetMode;
    Crtc->SetFb = NvidiaGdlSetFb;
    Crtc->Enable = NvidiaGdlEnable;
    Crtc->Disable = NvidiaGdlDisable;
    
    GdlDeviceRegister(Gpu);
    Dev->GdsDevice = Gpu;
    
    ConsolePrint("[NVIDIA] Registered with GDL\n");
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Main Initialization
 * ============================================================================
 */
INT NvidiaInit(PciDevice *PciDev) {
    NvidiaDevice *Dev;
    UINT32 MmioPhys;
    INT Result;
    
    if (!PciDev) {
        RETURN(NO_OBJECT);
    }
    
    ConsolePrint("[NVIDIA] Found GPU at %02X:%02X.%X (%04X:%04X)\n",
                PciDev->Bus, PciDev->Slot, PciDev->Function,
                PciDev->VendorId, PciDev->DeviceId);
    
    Dev = (NvidiaDevice*)MemoryAllocate(sizeof(NvidiaDevice));
    if (!Dev) {
        RETURN(NO_MEMORY);
    }
    
    MemSet(Dev, 0, sizeof(NvidiaDevice));
    Dev->PciDev = PciDev;
    Dev->DeviceId = PciDev->DeviceId;
    Dev->Generation = NvidiaDetectGeneration(PciDev->DeviceId);
    
    if (Dev->Generation == NVIDIA_GEN_UNKNOWN) {
        ConsolePrint("[NVIDIA] Unknown GPU generation (0x%04X), using Pascal fallback\n",
                    PciDev->DeviceId);
        Dev->Generation = NVIDIA_GEN_PASCAL;
    }
    
    /* Enable PCI bus mastering */
    PciEnableBusmaster(PciDev);
    PciEnable(PciDev);
    
    /* Detect VRAM size from framebuffer BAR */
    if (PciDev->BarSizes[NVIDIA_BAR_FB]) {
        Dev->VramSize = (UINT32)PciDev->BarSizes[NVIDIA_BAR_FB];
        ConsolePrint("[NVIDIA] VRAM BAR size: %u MB\n", Dev->VramSize / (1024 * 1024));
    }
    
    /* Get MMIO BAR */
    MmioPhys = (UINT32)(PciDev->Bars[NVIDIA_BAR_MMIO] & ~0xF);
    if (!MmioPhys) {
        ConsolePrint("[NVIDIA] No MMIO BAR\n");
        MemoryFree(Dev);
        RETURN(NO_OBJECT);
    }
    
    Dev->MmioBase = (volatile NOPTR*)(UINTPTR)MmioPhys;
    ConsolePrint("[NVIDIA] MMIO at 0x%X\n", MmioPhys);
    
    /* Setup GART (non-fatal if fails) */
    if (NvidiaSetupGart(Dev, PciDev) != SUCCESS) {
        ConsolePrint("[NVIDIA] GART setup failed, continuing\n");
    }
    
    /* Default framebuffer format and mode */
    Dev->FbFormat = SURFACE_FORMAT_XRGB8888;
    Dev->FbWidth = 1024;
    Dev->FbHeight = 768;
    Dev->FbStride = Dev->FbWidth * 4;
    
    /* Allocate framebuffer based on default mode (min 2 MB) */
    {
        UINT32 FbBytes = Dev->FbStride * Dev->FbHeight;
        UINT32 FbPages = (FbBytes + 4095) / 4096;
        if (FbPages < 512) {
            FbPages = 512;
        }
        Dev->Framebuffer = PhysAllocAllocateRange(PhysAllocGet(), FbPages);
    }
    if (!Dev->Framebuffer) {
        ConsolePrint("[NVIDIA] Framebuffer allocation failed\n");
        MemoryFree(Dev);
        RETURN(NO_MEMORY);
    }
    
    /* Setup IRQ */
    Result = NvidiaSetupIrq(Dev);
    if (Result != SUCCESS) {
        ConsolePrint("[NVIDIA] IRQ setup failed, using polling\n");
    }
    
    /* Enable VBlank interrupts */
    NvidiaWrite32(Dev, NV_PMC_INTR_ENABLE, NV_PMC_INTR_VBLANK0);
    
    /* Set default mode */
    NvidiaSetMode(Dev, 1024, 768, 32);
    NvidiaSetFramebuffer(Dev, (UINT64)(UINTPTR)Dev->Framebuffer, Dev->FbStride);
    NvidiaEnable(Dev, TRUE);
    
    /* Register with GDL */
    NvidiaRegisterGdl(Dev);
    
    Dev->Initialized = TRUE;
    GPrimaryNvidia = Dev;
    
    /* Register kernel driver */
    if (!GNvidiaDriver) {
        GNvidiaDriver = KDriverGenerateStruct("NVIDIA", DCL1, TRUE, Dev, NULLPTR);
        if (GNvidiaDriver) {
            KDriverRegister(GNvidiaDriver);
        }
    }
    
    ConsolePrint("[NVIDIA] Initialized (Gen%u, MSI=%s)\n",
                Dev->Generation, Dev->MsiEnabled ? "yes" : "no");
    
    RETURN(SUCCESS);
}

INT NvidiaInitAll(NOPTR) {
    PciDevice *Dev = PciGetFirst();
    INT Found = 0;
    
    while (Dev) {
        if (Dev->VendorId == NVIDIA_VENDOR_ID) {
            NvidiaInit(Dev);
            Found++;
        }
        Dev = PciGetNext(Dev);
    }
    
    if (Found == 0) {
        ConsolePrint("[NVIDIA] No compatible GPU found\n");
        RETURN(NOT_FOUND);
    }
    
    RETURN(SUCCESS);
}