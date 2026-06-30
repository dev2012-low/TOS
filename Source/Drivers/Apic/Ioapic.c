#include <Ioapic.h>
#include <ApicRegs.h>
#include <Apic.h>
#include <Asm/Mmio.h>
#include <Asm/Io.h>
#include <Kernel/KDriver.h>
#include <Acpi.h>
#include <Lib/String.h>
#include <Memory/Allocator.h>
#include <Kernel/Return.h>

/*
 * ============================================================================= Global Variables ============================================================================
 */

static ListHead GIoapicList;
static UINT32 GIoapicCount = 0;

/*
 * IRQ redirection table (GSI -> vector mapping)
 */
#define MAX_GSI 256

static struct {
    UINT8 Vector;
    BOOL Masked;
    BOOL LevelTriggered;
    BOOL ActiveLow;
    UINT32 DestApicId;
} GIrqRedirects[MAX_GSI];

IoapicOverride IoapicOverrides[256];

/*
 * ============================================================================= IOAPIC Register Access ============================================================================
 */

#define IOAPIC_REG_ID        0x00
#define IOAPIC_REG_VERSION   0x01
#define IOAPIC_REG_ARB       0x02
#define IOAPIC_REDTBL_BASE   0x10

static inline UINT32 IoapicReadReg(volatile NOPTR *Base, UINT32 Reg) {
    MmioWrite32((volatile UINT32*)((UINTPTR)Base + 0x00), Reg);
    return MmioRead32((volatile UINT32*)((UINTPTR)Base + 0x10));
}

static inline NOPTR IoapicWriteReg(volatile NOPTR *Base, UINT32 Reg, UINT32 Val) {
    MmioWrite32((volatile UINT32*)((UINTPTR)Base + 0x00), Reg);
    MmioWrite32((volatile UINT32*)((UINTPTR)Base + 0x10), Val);
}

static NOPTR IoapicSetRedirection(IoapicDevice *Ioapic, UINT32 Index, UINT32 Low, UINT32 High) {
    UINT32 RegLow = IOAPIC_REDTBL_BASE + Index * 2;
    UINT32 RegHigh = RegLow + 1;
    
    IoapicWriteReg(Ioapic->VirtAddr, RegHigh, High);
    IoapicWriteReg(Ioapic->VirtAddr, RegLow, Low);
}

NOPTR IoapicGetRedirection(IoapicDevice *Ioapic, UINT32 Index,
                                    UINT32 *Low, UINT32 *High) {
    UINT32 RegLow = IOAPIC_REDTBL_BASE + Index * 2;
    UINT32 RegHigh = RegLow + 1;
    
    *Low = IoapicReadReg(Ioapic->VirtAddr, RegLow);
    *High = IoapicReadReg(Ioapic->VirtAddr, RegHigh);
}

/*
 * ============================================================================= IRQ Routing ============================================================================
 */

static IoapicDevice *FindIoapicForGsi(UINT32 Gsi) {
    ListHead *Pos;
    IoapicDevice *Best = NULLPTR;
    
    ListForEach(Pos, &GIoapicList) {
        IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
        if (Gsi >= Ioapic->GsiBase && Gsi < Ioapic->GsiBase + Ioapic->MaxRedir) {
            return Ioapic;
        }
        if (Ioapic->GsiBase <= Gsi) {
            if (!Best || Ioapic->GsiBase > Best->GsiBase) {
                Best = Ioapic;
            }
        }
    }
    
    return Best;
}

/*
 * ============================================================================= Initialization ============================================================================
 */

INT IoapicInit(NOPTR) {   
    Acpi *AcpiObj = AcpiGetTable();
    
    if (!AcpiObj || !AcpiObj->Madt) {
        RETURN(NO_OBJECT);
    }
    
    GIoapicList.Next = &GIoapicList;
    GIoapicList.Prev = &GIoapicList;
    GIoapicCount = 0;

    for (UINT32 i = 0; i < MAX_GSI; i++) {
    	GIrqRedirects[i].Vector = 0;
    	GIrqRedirects[i].Masked = TRUE;
    	GIrqRedirects[i].LevelTriggered = FALSE;
    	GIrqRedirects[i].ActiveLow = FALSE;
    	GIrqRedirects[i].DestApicId = 0;
    }
    
    UINT8 *Entry = (UINT8*)AcpiObj->Madt + sizeof(MADT);
    UINT8 *End = (UINT8*)AcpiObj->Madt + AcpiObj->Madt->Header.Length;
    
    while (Entry < End) {
        MADTEntryHeader *Header = (MADTEntryHeader*)Entry;
        
        if (Header->Type == MADT_TYPE_IO_APIC) {
            MADTIoApic *IoapicEntry = (MADTIoApic*)Entry;
            
            IoapicDevice *Ioapic = (IoapicDevice*)MemoryAllocate(sizeof(IoapicDevice));
            
            if (!Ioapic) {
                RETURN(NO_MEMORY);
            }
            
            MemSet(Ioapic, 0, sizeof(IoapicDevice));
            
            Ioapic->Id = IoapicEntry->IoApicId;
            Ioapic->Address = IoapicEntry->IoApicAddress;
            Ioapic->GsiBase = IoapicEntry->GlobalSystemInterruptBase;
            Ioapic->VirtAddr = (volatile NOPTR*)(UINTPTR)Ioapic->Address;
            
            UINT32 Version = IoapicReadReg(Ioapic->VirtAddr, IOAPIC_REG_VERSION);
            
            Ioapic->Version = Version & 0xFF;
            Ioapic->MaxRedir = ((Version >> 16) & 0xFF) + 1;
            
            ListAddTail(&GIoapicList, &Ioapic->Node);
            GIoapicCount++;
        }
        
        Entry += Header->Length;
    }
    
    if (GIoapicCount == 0) {
        RETURN(NO_OBJECT);
    }

    IoapicMaskAll();

    KDriverRegister(KDriverGenerateStruct("IOAPIC", DCL0, TRUE, NULLPTR, NULLPTR));

    RETURN(SUCCESS);
}

/*
 * ============================================================================= Public API Functions ============================================================================
 */

UINT32 IoapicGetCount(NOPTR) {
    return GIoapicCount;
}

INT IoapicRedirectIrq(UINT32 Gsi, UINT8 Vector, UINT32 ApicId, UINT32 Flags) {
    IoapicDevice *Ioapic = FindIoapicForGsi(Gsi);
    if (!Ioapic) RETURN(NO_OBJECT);
    
    UINT32 Index = Gsi - Ioapic->GsiBase;
    if (Index >= Ioapic->MaxRedir) RETURN(INCORRECT_VALUE);
    
    UINT32 Low = Vector & 0xFF;
    Low |= DELIVERY_FIXED;
    
    if (Flags & IOAPIC_FLAG_ACTIVE_LOW) {
        Low |= IOAPIC_REDIR_POLARITY;
    }
    
    if (Flags & IOAPIC_FLAG_LEVEL_TRIGGERED) {
        Low |= IOAPIC_REDIR_TRIGGER;
    }
    
    Low |= IOAPIC_REDIR_MASKED;
    
    UINT32 High = ApicFormatIoapicDestination(ApicId);
    
    GIrqRedirects[Gsi].Vector = Vector;
    GIrqRedirects[Gsi].LevelTriggered = (Flags & IOAPIC_FLAG_LEVEL_TRIGGERED) ? TRUE : FALSE;
    GIrqRedirects[Gsi].ActiveLow = (Flags & IOAPIC_FLAG_ACTIVE_LOW) ? TRUE : FALSE;
    GIrqRedirects[Gsi].DestApicId = ApicId;
    GIrqRedirects[Gsi].Masked = TRUE;
    
    IoapicSetRedirection(Ioapic, Index, Low, High);
    RETURN(SUCCESS);
}

INT IoapicUnredirectIrq(UINT32 Gsi) {
    IoapicDevice *Ioapic = FindIoapicForGsi(Gsi);
    if (!Ioapic) RETURN(NO_OBJECT);
    
    UINT32 Index = Gsi - Ioapic->GsiBase;
    if (Index >= Ioapic->MaxRedir) RETURN(INCORRECT_VALUE);
    
    UINT32 Low, High;
    IoapicGetRedirection(Ioapic, Index, &Low, &High);
    Low |= IOAPIC_REDIR_MASKED;
    IoapicSetRedirection(Ioapic, Index, Low, High);
    
    GIrqRedirects[Gsi].Vector = 0;
    GIrqRedirects[Gsi].Masked = TRUE;
    
    RETURN(SUCCESS);
}

NOPTR IoapicMaskIrq(UINT32 Gsi) {
    IoapicDevice *Ioapic = FindIoapicForGsi(Gsi);
    if (!Ioapic) return;
    
    UINT32 Index = Gsi - Ioapic->GsiBase;
    if (Index >= Ioapic->MaxRedir) return;
    
    UINT32 Low, High;
    IoapicGetRedirection(Ioapic, Index, &Low, &High);
    Low |= IOAPIC_REDIR_MASKED;
    IoapicSetRedirection(Ioapic, Index, Low, High);
    
    GIrqRedirects[Gsi].Masked = TRUE;
}

NOPTR IoapicUnmaskIrq(UINT32 Gsi) {
    IoapicDevice *Ioapic = FindIoapicForGsi(Gsi);
    if (!Ioapic) return;
    
    UINT32 Index = Gsi - Ioapic->GsiBase;
    if (Index >= Ioapic->MaxRedir) return;
    
    UINT32 Low, High;
    IoapicGetRedirection(Ioapic, Index, &Low, &High);
    Low &= ~IOAPIC_REDIR_MASKED;
    IoapicSetRedirection(Ioapic, Index, Low, High);
    
    GIrqRedirects[Gsi].Masked = FALSE;
}

NOPTR IoapicMaskAll(NOPTR) {
    ListHead *Pos;
    
    ListForEach(Pos, &GIoapicList) {
        IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
        
        for (UINT32 I = 0; I < Ioapic->MaxRedir; I++) {
            UINT32 Low, High;
            IoapicGetRedirection(Ioapic, I, &Low, &High);
            Low |= IOAPIC_REDIR_MASKED;
            IoapicSetRedirection(Ioapic, I, Low, High);
            
            UINT32 Gsi = Ioapic->GsiBase + I;
            if (Gsi < MAX_GSI) {
                GIrqRedirects[Gsi].Masked = TRUE;
            }
        }
    }
}

NOPTR IoapicUnmaskAll(NOPTR) {
    ListHead *Pos;
    
    ListForEach(Pos, &GIoapicList) {
        IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
        
        for (UINT32 I = 0; I < Ioapic->MaxRedir; I++) {
            UINT32 Low, High;
            IoapicGetRedirection(Ioapic, I, &Low, &High);
            Low &= ~IOAPIC_REDIR_MASKED;
            IoapicSetRedirection(Ioapic, I, Low, High);
            
            UINT32 Gsi = Ioapic->GsiBase + I;
            if (Gsi < MAX_GSI) {
                GIrqRedirects[Gsi].Masked = FALSE;
            }
        }
    }
}

NOPTR IoapicEoi(UINT32 Gsi) {
    (NOPTR)Gsi;
    ApicEoi();
}

UINT32 IoapicGetVersion(UINT32 Index) {
    ListHead *Pos;
    UINT32 Current = 0;
    
    ListForEach(Pos, &GIoapicList) {
        if (Current == Index) {
            IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
            return Ioapic->Version;
        }
        Current++;
    }
    
    return 0;
}

UINT32 IoapicGetGsiBase(UINT32 Index) {
    ListHead *Pos;
    UINT32 Current = 0;
    
    ListForEach(Pos, &GIoapicList) {
        if (Current == Index) {
            IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
            return Ioapic->GsiBase;
        }
        Current++;
    }
    
    return 0;
}

UINT32 IoapicGetIrqCount(UINT32 Index) {
    ListHead *Pos;
    UINT32 Current = 0;
    
    ListForEach(Pos, &GIoapicList) {
        if (Current == Index) {
            IoapicDevice *Ioapic = ListEntry(Pos, IoapicDevice, Node);
            return Ioapic->MaxRedir;
        }
        Current++;
    }
    
    return 0;
}

INT IoapicProcessOverrides(NOPTR) {
    Acpi *AcpiObj = AcpiGetTable();
    if (!AcpiObj || !AcpiObj->Madt) {
        RETURN(NO_OBJECT);
    }
    
    for (UINT32 i = 0; i < 256; i++) {
        IoapicOverrides[i].Gsi = 0;
        IoapicOverrides[i].Flags = 0;
        IoapicOverrides[i].Valid = FALSE;
    }
    
    UINT8 *Entry = (UINT8*)AcpiObj->Madt + sizeof(MADT);
    UINT8 *End = (UINT8*)AcpiObj->Madt + AcpiObj->Madt->Header.Length;
    
    UINT32 OverrideCount = 0;
    
    while (Entry < End) {
        MADTEntryHeader *Header = (MADTEntryHeader*)Entry;
        
        if (Header->Type == MADT_TYPE_INT_SOURCE_OVERRIDE) {
            OverrideCount++;
            MADTIntSourceOverride *Override = (MADTIntSourceOverride*)Entry;
            
            UINT32 Flags = 0;

            if ((Override->Flags & 0x3) == 0x3) {
                Flags |= IOAPIC_FLAG_ACTIVE_LOW;
            }

            if (((Override->Flags >> 2) & 0x3) == 0x3) {
                Flags |= IOAPIC_FLAG_LEVEL_TRIGGERED;
            }

            if (Override->Source < 256) {
                IoapicOverrides[Override->Source].Gsi = Override->GlobalSystemInterrupt;
                IoapicOverrides[Override->Source].Flags = Flags;
                IoapicOverrides[Override->Source].Valid = TRUE;
            }
        }
        
        Entry += Header->Length;
    }

    RETURN(SUCCESS);
}

INT IoapicGetOverride(UINT32 Source, UINT32 *Gsi, UINT32 *Flags) {
    if (Source >= 256) RETURN(INCORRECT_VALUE);

    if (IoapicOverrides[Source].Valid) {
        if (Gsi) *Gsi = IoapicOverrides[Source].Gsi;
        if (Flags) *Flags = IoapicOverrides[Source].Flags;
        RETURN(SUCCESS);
    }

    if (Gsi) *Gsi = Source;
    if (Flags) *Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    RETURN(SUCCESS);
}

NOPTR IoapicReadRedirection(NOPTR *Ioapic, UINT32 Index, UINT32 *Low, UINT32 *High) {
    IoapicDevice *Dev = (IoapicDevice*)Ioapic;
    if (!Dev || !Dev->VirtAddr) return;
    
    UINT32 RegLow = IOAPIC_REDTBL_BASE + Index * 2;
    UINT32 RegHigh = RegLow + 1;
    
    *Low = IoapicReadReg(Dev->VirtAddr, RegLow);
    *High = IoapicReadReg(Dev->VirtAddr, RegHigh);
}

IoapicDevice* IoapicGetFirst(NOPTR) {
    if (ListEmpty(&GIoapicList)) {
        return NULLPTR;
    }
    return ListEntry(GIoapicList.Next, IoapicDevice, Node);
}