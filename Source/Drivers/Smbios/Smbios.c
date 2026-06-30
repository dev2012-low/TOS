#include <Smbios.h>
#include <Multiboot2Parser.h>
#include <Multiboot2Struct.h>
#include <Kernel/KDriver.h>
#include <Kernel/Return.h>
#include <Console.h>
#include <Lib/String.h>
#include <stddef.h>

EXTERN(Multiboot2Info, MB);

#define EFI_TABLE_SIGNATURE 0x00000000535454424FULL /* EFI_SYSTEM_TABLE_SIGNATURE */
#define SMBIOS_EFI_GUID \
    { 0xEB9D2D31, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }
#define SMBIOS3_EFI_GUID \
    { 0xF2FD1544, 0x9794, 0x4A2C, { 0x99, 0x2E, 0xE5, 0xBB, 0xCF, 0x65, 0xE5, 0xB9 } }

typedef struct ATTRIBUTE(packed) EfiGuid {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8 Data4[8];
} EfiGuid;

typedef struct ATTRIBUTE(packed) EfiConfigEntry {
    EfiGuid VendorGuid;
    UINT64 VendorTable;
} EfiConfigEntry;

typedef struct ATTRIBUTE(packed) EfiSystemTable {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 Crc32;
    UINT32 Reserved;
    UINT64 FirmwareVendor;
    UINT64 FirmwareRevision;
    UINT64 ConsoleInHandle;
    UINT64 ConIn;
    UINT64 ConsoleOutHandle;
    UINT64 ConOut;
    UINT64 StandardErrorHandle;
    UINT64 StdErr;
    UINT64 RuntimeServices;
    UINT64 BootServices;
    UINT64 NumberOfTableEntries;
    UINT64 ConfigurationTable;
} EfiSystemTable;

static SmbiosTable GSmbios;
static KDriver *GSmbiosDriver;

static BOOL GuidEqual(const EfiGuid *A, const EfiGuid *B) {
    return MemCmp(A, B, sizeof(EfiGuid)) == 0;
}

static UINT8 SmbiosChecksum(const UINT8 *Data, UINT32 Len) {
    UINT8 Sum = 0;
    for (UINT32 I = 0; I < Len; I++) {
        Sum += Data[I];
    }
    return Sum;
}

static const CHAR *SmbiosStringAt(const UINT8 *Struct, UINT8 Index) {
    if (!Struct || Index == 0) {
        return "";
    }
    const SmbiosHeader *Hdr = (const SmbiosHeader *)Struct;
    const CHAR *Str = (const CHAR *)(Struct + Hdr->Length);
    UINT8 Cur = 1;
    while (*Str) {
        if (Cur == Index) {
            return Str;
        }
        while (*Str) {
            Str++;
        }
        Str++;
        Cur++;
    }
    return "";
}

const CHAR *SmbiosGetString(const SmbiosHeader *Hdr, UINT8 Index) {
    if (!Hdr) {
        return "";
    }
    return SmbiosStringAt((const UINT8 *)Hdr, Index);
}

static BOOL SmbiosIndexStructures(NOPTR) {
    const UINT8 *Ptr = GSmbios.Table;
    const UINT8 *End = GSmbios.Table + GSmbios.TableLen;
    UINT16 Count = 0;

    while (Ptr < End && Count < SMBIOS_MAX_STRUCTURES) {
        const SmbiosHeader *Hdr = (const SmbiosHeader *)Ptr;
        if (Hdr->Length < sizeof(SmbiosHeader)) {
            break;
        }
        GSmbios.Structures[Count++] = (SmbiosHeader *)Ptr;

        Ptr += Hdr->Length;
        while (Ptr < End - 1 && (Ptr[0] || Ptr[1])) {
            Ptr++;
        }
        Ptr += 2;
        if (Hdr->Type == SMBIOS_TYPE_END) {
            break;
        }
    }
    GSmbios.StructureCount = Count;
    return Count > 0;
}

static BOOL SmbiosLoadTable(UINT64 Phys, UINT32 Len, SmbiosSource Src,
                            UINT8 Major, UINT8 Minor) {
    if (!Phys || Len < sizeof(SmbiosHeader)) {
        return FALSE;
    }
    GSmbios.TablePhys = Phys;
    GSmbios.TableLen = Len;
    GSmbios.Table = (const UINT8 *)(UINTPTR)Phys;
    GSmbios.Source = Src;
    GSmbios.Major = Major;
    GSmbios.Minor = Minor;
    return SmbiosIndexStructures();
}

static BOOL SmbiosScanLegacy(NOPTR) {
    for (UINT32 Addr = 0x000F0000; Addr < 0x00100000; Addr += 16) {
        const CHAR *Anchor = (const CHAR *)(UINTPTR)Addr;
        if (MemCmp(Anchor, SMBIOS_ANCHOR_LEGACY, 4) != 0) {
            continue;
        }
        const SmbiosLegacyEntry *Ep = (const SmbiosLegacyEntry *)(UINTPTR)Addr;
        if (SmbiosChecksum((const UINT8 *)Ep, Ep->Length) != 0) {
            continue;
        }
        if (MemCmp((const CHAR *)(UINTPTR)(Addr + 0x10), "_DMI_", 5) != 0) {
            continue;
        }
        if (SmbiosChecksum((const UINT8 *)(UINTPTR)(Addr + 0x10), 0x0F) != 0) {
            continue;
        }
        UINT16 TableLen = *(const UINT16 *)(UINTPTR)(Addr + 0x16);
        UINT32 TableAddr = *(const UINT32 *)(UINTPTR)(Addr + 0x18);
        UINT8 Major = *(const UINT8 *)(UINTPTR)(Addr + 0x06);
        UINT8 Minor = *(const UINT8 *)(UINTPTR)(Addr + 0x07);
        return SmbiosLoadTable(TableAddr, TableLen, SMBIOS_SOURCE_LEGACY, Major, Minor);
    }
    return FALSE;
}

static BOOL SmbiosScanEfi(NOPTR) {
    Multiboot2Info *Info = Multiboot2ParserGet();
    UINT64 StAddr = 0;

    if (!Info) {
        return FALSE;
    }
    if (Info->Efi.SystemTable64) {
        StAddr = Info->Efi.SystemTable64;
    } else if (Info->Efi.SystemTable32) {
        StAddr = (UINT64)Info->Efi.SystemTable32;
    } else {
        return FALSE;
    }

    const EfiSystemTable *St = (const EfiSystemTable *)(UINTPTR)StAddr;
    if (St->Signature != EFI_TABLE_SIGNATURE) {
        return FALSE;
    }

    static const EfiGuid SmbiosGuid = SMBIOS_EFI_GUID;
    static const EfiGuid Smbios3Guid = SMBIOS3_EFI_GUID;

    const EfiConfigEntry *Entries =
        (const EfiConfigEntry *)(UINTPTR)St->ConfigurationTable;
    for (UINT64 I = 0; I < St->NumberOfTableEntries; I++) {
        const EfiConfigEntry *E = &Entries[I];
        if (GuidEqual(&E->VendorGuid, &Smbios3Guid)) {
            const Smbios30Entry *Ep = (const Smbios30Entry *)(UINTPTR)E->VendorTable;
            if (MemCmp(Ep->Anchor, SMBIOS_ANCHOR_30, 5) != 0) {
                continue;
            }
            if (SmbiosChecksum((const UINT8 *)Ep, Ep->Length) != 0) {
                continue;
            }
            return SmbiosLoadTable(Ep->TableAddr, (UINT32)Ep->TableMax,
                                   SMBIOS_SOURCE_EFI3, Ep->Major, Ep->Minor);
        }
        if (GuidEqual(&E->VendorGuid, &SmbiosGuid)) {
            const SmbiosLegacyEntry *Ep =
                (const SmbiosLegacyEntry *)(UINTPTR)E->VendorTable;
            if (MemCmp(Ep->Anchor, SMBIOS_ANCHOR_LEGACY, 4) != 0) {
                continue;
            }
            if (SmbiosChecksum((const UINT8 *)Ep, Ep->Length) != 0) {
                continue;
            }
            return SmbiosLoadTable(Ep->TableAddr, Ep->TableLen, SMBIOS_SOURCE_EFI,
                                   Ep->Major, Ep->Minor);
        }
    }
    return FALSE;
}

SmbiosHeader *SmbiosFindType(UINT8 Type) {
    if (!GSmbios.Initialized) {
        return NULLPTR;
    }
    for (UINT16 I = 0; I < GSmbios.StructureCount; I++) {
        if (GSmbios.Structures[I]->Type == Type) {
            return GSmbios.Structures[I];
        }
    }
    return NULLPTR;
}

SmbiosTable *SmbiosGetTable(NOPTR) {
    return GSmbios.Initialized ? &GSmbios : NULLPTR;
}

BOOL SmbiosIsReady(NOPTR) {
    return GSmbios.Initialized;
}

static const CHAR *SmbiosField(UINT8 Type, UINT8 FieldIndex) {
    SmbiosHeader *Hdr = SmbiosFindType(Type);
    if (!Hdr || Hdr->Length <= FieldIndex) {
        return "";
    }
    const UINT8 *Base = (const UINT8 *)Hdr;
    return SmbiosGetString(Hdr, Base[FieldIndex]);
}

const CHAR *SmbiosGetBiosVendor(NOPTR) {
    return SmbiosField(SMBIOS_TYPE_BIOS, offsetof(SmbiosBiosInfo, Vendor));
}

const CHAR *SmbiosGetSystemManufacturer(NOPTR) {
    return SmbiosField(SMBIOS_TYPE_SYSTEM, offsetof(SmbiosSystemInfo, Manufacturer));
}

const CHAR *SmbiosGetSystemProduct(NOPTR) {
    return SmbiosField(SMBIOS_TYPE_SYSTEM, offsetof(SmbiosSystemInfo, ProductName));
}

const CHAR *SmbiosGetBoardManufacturer(NOPTR) {
    return SmbiosField(SMBIOS_TYPE_BASEBOARD, offsetof(SmbiosBaseboardInfo, Manufacturer));
}

const CHAR *SmbiosGetBoardProduct(NOPTR) {
    return SmbiosField(SMBIOS_TYPE_BASEBOARD, offsetof(SmbiosBaseboardInfo, Product));
}

const CHAR *SmbiosGetProcessorManufacturer(NOPTR) {
    return SmbiosField(SMBIOS_TYPE_PROCESSOR, offsetof(SmbiosProcessorInfo, Manufacturer));
}

const CHAR *SmbiosGetProcessorVersion(NOPTR) {
    return SmbiosField(SMBIOS_TYPE_PROCESSOR, offsetof(SmbiosProcessorInfo, Version));
}

NOPTR SmbiosPrintInfo(NOPTR) {
    if (!GSmbios.Initialized) {
        return;
    }

    const CHAR *Src = "?";
    switch (GSmbios.Source) {
    case SMBIOS_SOURCE_LEGACY: Src = "legacy"; break;
    case SMBIOS_SOURCE_EFI:    Src = "EFI"; break;
    case SMBIOS_SOURCE_EFI3:   Src = "EFI3"; break;
    default: break;
    }

    const CHAR *Bios = SmbiosGetBiosVendor();
    const CHAR *SysM = SmbiosGetSystemManufacturer();
    const CHAR *SysP = SmbiosGetSystemProduct();
    const CHAR *BrdM = SmbiosGetBoardManufacturer();
    const CHAR *BrdP = SmbiosGetBoardProduct();
    const CHAR *CpuM = SmbiosGetProcessorManufacturer();
    const CHAR *CpuV = SmbiosGetProcessorVersion();

    SmbiosHeader *Proc = SmbiosFindType(SMBIOS_TYPE_PROCESSOR);
    if (Proc && Proc->Length >= sizeof(SmbiosProcessorInfo)) {
        const SmbiosProcessorInfo *Pi = (const SmbiosProcessorInfo *)Proc;
    }
}

INT SmbiosInit(NOPTR) {
    MemSet(&GSmbios, 0, sizeof(GSmbios));

    if (SmbiosScanEfi() || SmbiosScanLegacy()) {
        GSmbios.Initialized = TRUE;
        if (!GSmbiosDriver) {
            GSmbiosDriver = KDriverGenerateStruct("SMBIOS", DCL1, TRUE, NULLPTR, NULLPTR);
            if (GSmbiosDriver) {
                KDriverRegister(GSmbiosDriver);
            }
        }
        RETURN(SUCCESS);
    }

    RETURN(NOT_FOUND);
}
