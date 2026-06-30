#include <Acpi.h>
#include <Apic.h>
#include <ApicRegs.h>
#include <Lib/String.h>
#include <Asm/Io.h>
#include <Asm/Mmio.h>
#include <Kernel/KDriver.h>
#include <Multiboot2Struct.h>
#include <Kernel/Return.h>
#include <AcpiTables.h>
#include <Console.h>
#include <AcpiEvents.h>

Acpi GAcpi = {0};

EXTERN(Multiboot2Info, MB);

/*
 * ============================================================================= Internal Utilities ============================================================================
 */
UINT8 AcpiChecksum(NOPTR *Table, UINT32 Length) {
    UINT8 Sum = 0;
    UINT8 *Bytes = (UINT8*)Table;
    for (UINT32 I = 0; I < Length; I++) {
        Sum += Bytes[I];
    }
    return Sum;
}

static BOOL AcpiValidateRsdp(RSDPV2 *Rsdp) {
    if (MemCmp(Rsdp->V1.Signature, ACPI_RSDP_SIGNATURE, 8) != 0) {
        return FALSE;
    }

    UINT8 V1Checksum = AcpiChecksum(Rsdp, ACPI_RSDP_V1_SIZE);
    
    if (V1Checksum != 0) {
        return FALSE;
    }
    
    if (Rsdp->V1.Revision >= 2) {
        if (Rsdp->Length > sizeof(RSDPV2)) {
            return FALSE;
        }
        
        UINT8 V2Checksum = AcpiChecksum(Rsdp, Rsdp->Length);
        
        if (V2Checksum != 0) {
            return FALSE;
        }
        GAcpi.UseXsdt = TRUE;
    }
    return TRUE;
}

/*
 * ============================================================================= Table Finding Functions ============================================================================
 */

static NOPTR *AcpiFindRsdt(const CHAR *Signature) {
    if (!GAcpi.Rsdt) return NULLPTR;
    
    UINT32 EntriesCount = (GAcpi.Rsdt->Header.Length - sizeof(SDTHeader)) / 4;
    UINT32 *Entries = (UINT32*)((UINTPTR)GAcpi.Rsdt + sizeof(SDTHeader));
    
    for (UINT32 I = 0; I < EntriesCount; I++) {
        SDTHeader *Header = (SDTHeader*)(UINTPTR)Entries[I];
        if (MemCmp(Header->Signature, Signature, 4) == 0) {
            if (AcpiChecksum(Header, Header->Length) == 0) {
                return Header;
            }
        }
    }
    
    return NULLPTR;
}

static NOPTR *AcpiFindXsdt(const CHAR *Signature) {
    if (!GAcpi.Xsdt) return NULLPTR;
    
    UINT32 EntriesCount = (GAcpi.Xsdt->Header.Length - sizeof(SDTHeader)) / 8;
    UINT64 *Entries = (UINT64*)((UINTPTR)GAcpi.Xsdt + sizeof(SDTHeader));
    
    for (UINT32 I = 0; I < EntriesCount; I++) {
        SDTHeader *Header = (SDTHeader*)(UINTPTR)Entries[I];
        if (MemCmp(Header->Signature, Signature, 4) == 0) {
            if (AcpiChecksum(Header, Header->Length) == 0) {
                return Header;
            }
        }
    }
    
    return NULLPTR;
}

NOPTR *AcpiFindTable(const CHAR *Signature) {
    return AcpiFindTableWithIndex(Signature, 0);
}

NOPTR *AcpiFindTableWithIndex(const CHAR *Signature, INT Index) {
    if (!GAcpi.Rsdt && !GAcpi.Xsdt) return NULLPTR;
    
    INT Found = 0;
    
    if (GAcpi.UseXsdt) {
        UINT32 EntriesCount = (GAcpi.Xsdt->Header.Length - sizeof(SDTHeader)) / 8;
        UINT64 *Entries = (UINT64*)((UINTPTR)GAcpi.Xsdt + sizeof(SDTHeader));
        
        for (UINT32 I = 0; I < EntriesCount; I++) {
            SDTHeader *Header = (SDTHeader*)(UINTPTR)Entries[I];
            if (MemCmp(Header->Signature, Signature, 4) == 0) {
                if (Found == Index) {
                    if (AcpiChecksum(Header, Header->Length) == 0) {
                        return Header;
                    }
                }
                Found++;
            }
        }
    } else {
        UINT32 EntriesCount = (GAcpi.Rsdt->Header.Length - sizeof(SDTHeader)) / 4;
        UINT32 *Entries = (UINT32*)((UINTPTR)GAcpi.Rsdt + sizeof(SDTHeader));
        
        for (UINT32 I = 0; I < EntriesCount; I++) {
            SDTHeader *Header = (SDTHeader*)(UINTPTR)Entries[I];
            if (MemCmp(Header->Signature, Signature, 4) == 0) {
                if (Found == Index) {
                    if (AcpiChecksum(Header, Header->Length) == 0) {
                        return Header;
                    }
                }
                Found++;
            }
        }
    }
    
    return NULLPTR;
}

/*
 * ============================================================================= MADT Parsing ============================================================================
 */

INT AcpiParseMadt(NOPTR) {
    if (!GAcpi.Madt) RETURN(NO_OBJECT);
    
    ApicInfo *Apic = &GAcpi.Apic;
    MemSet(Apic, 0, sizeof(ApicInfo));
    
    Apic->LocalApicAddress = GAcpi.Madt->LocalApicAddress;
    
    UINT8 *Entry = (UINT8*)GAcpi.Madt + sizeof(MADT);
    UINT8 *End = (UINT8*)GAcpi.Madt + GAcpi.Madt->Header.Length;
    
    while (Entry < End) {
        MADTEntryHeader *Header = (MADTEntryHeader*)Entry;
        
        switch (Header->Type) {
            case MADT_TYPE_LOCAL_APIC: {
                MADTLocalApic *Lapic = (MADTLocalApic*)Entry;
                if (Apic->ProcessorCount < 64) {
                    Apic->Processors[Apic->ProcessorCount].AcpiProcessorUid = Lapic->AcpiProcessorId;
                    Apic->Processors[Apic->ProcessorCount].ApicId = Lapic->ApicId;
                    Apic->Processors[Apic->ProcessorCount].Enabled = (Lapic->Flags & 1) != 0;
                    Apic->Processors[Apic->ProcessorCount].X2Apic = FALSE;
                    Apic->ProcessorCount++;
                }
                break;
            }

            case MADT_TYPE_PROCESSOR_LOCAL_X2APIC: {
                MADTLocalX2Apic *X2Apic = (MADTLocalX2Apic*)Entry;
                if (Apic->ProcessorCount < 64) {
                    Apic->Processors[Apic->ProcessorCount].AcpiProcessorUid = X2Apic->AcpiProcessorUid;
                    Apic->Processors[Apic->ProcessorCount].ApicId = X2Apic->LocalX2ApicId;
                    Apic->Processors[Apic->ProcessorCount].Enabled = (X2Apic->Flags & 1) != 0;
                    Apic->Processors[Apic->ProcessorCount].X2Apic = TRUE;
                    Apic->UsesX2Apic = TRUE;
                    Apic->ProcessorCount++;
                }
                break;
            }
            
            case MADT_TYPE_IO_APIC: {
                MADTIoApic *Ioapic = (MADTIoApic*)Entry;
                if (Apic->IoApicCount < 16) {
                    Apic->IoApics[Apic->IoApicCount].Address = Ioapic->IoApicAddress;
                    Apic->IoApics[Apic->IoApicCount].GsiBase = Ioapic->GlobalSystemInterruptBase;
                    Apic->IoApicCount++;
                }
                break;
            }
            
            case MADT_TYPE_INT_SOURCE_OVERRIDE: {
                MADTIntSourceOverride *Override = (MADTIntSourceOverride*)Entry;
                if (Apic->IntOverrideCount < 16) {
                    Apic->IntOverrides[Apic->IntOverrideCount].Bus = Override->Bus;
                    Apic->IntOverrides[Apic->IntOverrideCount].Source = Override->Source;
                    Apic->IntOverrides[Apic->IntOverrideCount].Gsi = Override->GlobalSystemInterrupt;
                    Apic->IntOverrides[Apic->IntOverrideCount].Flags = Override->Flags;
                    Apic->IntOverrideCount++;
                }
                break;
            }
        }
        
        Entry += Header->Length;
    }
    
    RETURN(SUCCESS);
}

UINT32 AcpiGetLocalApicAddr(NOPTR) {
    return GAcpi.Apic.LocalApicAddress;
}

INT AcpiEnableLocalApic(NOPTR) {
    if (ApicIsX2ApicMode() || ApicCpuSupportsX2Apic()) {
        RETURN(SUCCESS);
    }

    UINT32 ApicAddr = GAcpi.Apic.LocalApicAddress;
    if (!ApicAddr) RETURN(NO_OBJECT);
    
    UINT32 Svr = MmioRead32((volatile NOPTR*)(UINTPTR)(ApicAddr + LAPIC_SVR));
    Svr |= (1 << 8);
    Svr = (Svr & 0xFFFFFF00) | 0xFF;
    MmioWrite32((volatile NOPTR*)(UINTPTR)(ApicAddr + LAPIC_SVR), Svr);
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================= Initialization ============================================================================
 */

INT AcpiInit(UINT64 RsdpAddr) {
    if (!RsdpAddr) {
        RETURN(NO_OBJECT);
    }
    
    MemSet(&GAcpi, 0, sizeof(GAcpi));
    GAcpi.Rsdp = (RSDPV2*)(UINTPTR)RsdpAddr;
    
    if (!AcpiValidateRsdp(GAcpi.Rsdp)) {
        RETURN(NO_OBJECT);
    }
  
    if (GAcpi.UseXsdt) {
        GAcpi.Xsdt = (XSDT*)(UINTPTR)GAcpi.Rsdp->XsdtAddress;
        if (AcpiChecksum(GAcpi.Xsdt, GAcpi.Xsdt->Header.Length) != 0) {
            RETURN(INCORRECT_VALUE);
        }
    } else {
        GAcpi.Rsdt = (RSDT*)(UINTPTR)GAcpi.Rsdp->V1.RsdtAddress;
        if (AcpiChecksum(GAcpi.Rsdt, GAcpi.Rsdt->Header.Length) != 0) {
            RETURN(INCORRECT_VALUE);
        }
    }
    GAcpi.Fadt = (FADT*)AcpiFindTable("FACP");
    GAcpi.Madt = (MADT*)AcpiFindTable("APIC");
    GAcpi.Hpet = (HPET*)AcpiFindTable("HPET");
    GAcpi.Mcfg = (MCFG*)AcpiFindTable("MCFG");
    if (GAcpi.Fadt && GAcpi.Fadt->Header.Revision >= 3) {
        GAcpi.ApicBusFreq = GAcpi.Fadt->ApicBusFreq;
        if (GAcpi.ApicBusFreq > 0 && GAcpi.ApicBusFreq < 100000000) {
            // Print later
        } else {
            GAcpi.ApicBusFreq = 0;
        }
    } else {
        GAcpi.ApicBusFreq = 0;
    }
    
    if (GAcpi.Fadt) {
        if (GAcpi.UseXsdt && GAcpi.Fadt->XDsdt) {
            GAcpi.Dsdt = (SDTHeader*)(UINTPTR)GAcpi.Fadt->XDsdt;
        } else if (GAcpi.Fadt->Dsdt) {
            GAcpi.Dsdt = (SDTHeader*)(UINTPTR)GAcpi.Fadt->Dsdt;
        }
    }
    GAcpi.SsdtCount = 0;
    if (GAcpi.UseXsdt && GAcpi.Xsdt) {
    	UINT32 Entries = (GAcpi.Xsdt->Header.Length - sizeof(SDTHeader)) / 8;
    	UINT64 *EntriesPtr = (UINT64*)((UINTPTR)GAcpi.Xsdt + sizeof(SDTHeader));
    	for (UINT32 I = 0; I < Entries && GAcpi.SsdtCount < 16; I++) {
            SDTHeader *Table = (SDTHeader*)(UINTPTR)EntriesPtr[I];
            if (MemCmp(Table->Signature, "SSDT", 4) == 0) {
            	GAcpi.Ssdts[GAcpi.SsdtCount++] = Table;
            }
    	}
    } else if (GAcpi.Rsdt) {
    	UINT32 Entries = (GAcpi.Rsdt->Header.Length - sizeof(SDTHeader)) / 4;
    	UINT32 *EntriesPtr = (UINT32*)((UINTPTR)GAcpi.Rsdt + sizeof(SDTHeader));
    	for (UINT32 I = 0; I < Entries && GAcpi.SsdtCount < 16; I++) {
            SDTHeader *Table = (SDTHeader*)(UINTPTR)EntriesPtr[I];
            if (MemCmp(Table->Signature, "SSDT", 4) == 0) {
            	GAcpi.Ssdts[GAcpi.SsdtCount++] = Table;
            }
    	}
    }
    if (GAcpi.Madt) {
        AcpiParseMadt();
    }

    AcpiEventsInit();

    RETURN(SUCCESS);
}

Acpi *AcpiGetTable(NOPTR) {
    return &GAcpi;
}
