#pragma once

#include <Kernel/Types.h>

#define SMBIOS_ANCHOR_LEGACY   "_SM_"
#define SMBIOS_ANCHOR_30       "_SM3_"
#define SMBIOS_MAX_STRINGS       256
#define SMBIOS_MAX_STRUCTURES    128

#define SMBIOS_TYPE_BIOS        0
#define SMBIOS_TYPE_SYSTEM      1
#define SMBIOS_TYPE_BASEBOARD   2
#define SMBIOS_TYPE_CHASSIS     3
#define SMBIOS_TYPE_PROCESSOR   4
#define SMBIOS_TYPE_END         127

typedef struct ATTRIBUTE(packed) SmbiosLegacyEntry {
    CHAR Anchor[4];
    UINT8 Checksum;
    UINT8 Length;
    UINT8 Major;
    UINT8 Minor;
    UINT16 MaxSize;
    UINT8 Revision;
    UINT8 FormattedArea[5];
    CHAR IntAnchor[5];
    UINT8 IntChecksum;
    UINT16 TableLen;
    UINT32 TableAddr;
    UINT16 Count;
    UINT8 BcdRevision;
} SmbiosLegacyEntry;

typedef struct ATTRIBUTE(packed) Smbios30Entry {
    CHAR Anchor[5];
    UINT8 Checksum;
    UINT8 Length;
    UINT8 Major;
    UINT8 Minor;
    UINT8 DocRev;
    UINT8 Revision;
    UINT8 Reserved;
    UINT32 TableMax;
    UINT64 TableAddr;
} Smbios30Entry;

typedef struct ATTRIBUTE(packed) SmbiosHeader {
    UINT8 Type;
    UINT8 Length;
    UINT16 Handle;
} SmbiosHeader;

typedef struct ATTRIBUTE(packed) SmbiosBiosInfo {
    SmbiosHeader Hdr;
    UINT8 Vendor;
    UINT8 Version;
    UINT16 StartSegment;
    UINT8 ReleaseDate;
    UINT8 RomSize;
    UINT64 Characteristics;
    UINT8 CharacteristicsExt[2];
    UINT8 SystemBiosMajor;
    UINT8 SystemBiosMinor;
    UINT8 EcfMajor;
    UINT8 EcfMinor;
} SmbiosBiosInfo;

typedef struct ATTRIBUTE(packed) SmbiosSystemInfo {
    SmbiosHeader Hdr;
    UINT8 Manufacturer;
    UINT8 ProductName;
    UINT8 Version;
    UINT8 SerialNumber;
    UINT8 Uuid[16];
    UINT8 WakeUpType;
} SmbiosSystemInfo;

typedef struct ATTRIBUTE(packed) SmbiosBaseboardInfo {
    SmbiosHeader Hdr;
    UINT8 Manufacturer;
    UINT8 Product;
    UINT8 Version;
    UINT8 SerialNumber;
    UINT8 AssetTag;
    UINT8 FeatureFlags;
    UINT8 Location;
    UINT16 ChassisHandle;
    UINT8 BoardType;
    UINT8 ContainedHandles;
} SmbiosBaseboardInfo;

typedef struct ATTRIBUTE(packed) SmbiosProcessorInfo {
    SmbiosHeader Hdr;
    UINT8 SocketDesignation;
    UINT8 ProcessorType;
    UINT8 ProcessorFamily;
    UINT8 Manufacturer;
    UINT64 ProcessorId;
    UINT8 Version;
    UINT8 Voltage;
    UINT16 ExternalClock;
    UINT16 MaxSpeed;
    UINT16 CurrentSpeed;
    UINT8 Status;
    UINT8 ProcessorUpgrade;
    UINT16 L1CacheHandle;
    UINT16 L2CacheHandle;
    UINT16 L3CacheHandle;
    UINT8 SerialNumber;
    UINT8 AssetTag;
    UINT8 PartNumber;
    UINT8 CoreCount;
    UINT8 EnabledCoreCount;
    UINT8 ThreadCount;
} SmbiosProcessorInfo;

typedef enum {
    SMBIOS_SOURCE_NONE = 0,
    SMBIOS_SOURCE_LEGACY,
    SMBIOS_SOURCE_EFI,
    SMBIOS_SOURCE_EFI3
} SmbiosSource;

typedef struct {
    BOOL Initialized;
    SmbiosSource Source;
    UINT8 Major;
    UINT8 Minor;
    UINT64 TablePhys;
    UINT32 TableLen;
    UINT16 StructureCount;
    const UINT8 *Table;
    SmbiosHeader *Structures[SMBIOS_MAX_STRUCTURES];
} SmbiosTable;

INT SmbiosInit(NOPTR);
BOOL SmbiosIsReady(NOPTR);
SmbiosTable *SmbiosGetTable(NOPTR);
const CHAR *SmbiosGetString(const SmbiosHeader *Hdr, UINT8 Index);
SmbiosHeader *SmbiosFindType(UINT8 Type);
NOPTR SmbiosPrintInfo(NOPTR);
const CHAR *SmbiosGetBiosVendor(NOPTR);
const CHAR *SmbiosGetSystemManufacturer(NOPTR);
const CHAR *SmbiosGetSystemProduct(NOPTR);
const CHAR *SmbiosGetBoardManufacturer(NOPTR);
const CHAR *SmbiosGetBoardProduct(NOPTR);
const CHAR *SmbiosGetProcessorManufacturer(NOPTR);
const CHAR *SmbiosGetProcessorVersion(NOPTR);
