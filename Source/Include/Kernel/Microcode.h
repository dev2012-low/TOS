#pragma once

#include <Kernel/Types.h>

// ==================== Intel/VIA/Zhaoxin microcode header ====================
typedef struct ATTRIBUTE(packed) {
    UINT32 HeaderVersion;
    UINT32 UpdateRevision;
    UINT32 Date;              // DDMMYYYY
    UINT32 ProcessorSignature;
    UINT32 Checksum;
    UINT32 LoaderRevision;
    UINT32 ProcessorFlags;
    UINT32 DataSize;
    UINT32 TotalSize;
    UINT8  Reserved[12];
} IntelMicrocodeHeader;

// AMD microcode header (для справки)
typedef struct ATTRIBUTE(packed) {
    UINT32 PatchLevel;
    UINT32 PatchChecksum;
    UINT32 PatchFlags;
    UINT32 PatchInternalId;
    UINT32 PatchProcessorSignature;
    UINT8  Reserved[12];
} AmdMicrocodeHeader;

// ==================== Vendor enum ====================
typedef enum {
    CPU_VENDOR_INTEL,
    CPU_VENDOR_AMD,
    CPU_VENDOR_VIA,
    CPU_VENDOR_ZHAOXIN,
    CPU_VENDOR_UNKNOWN
} CpuVendor;

// ==================== Vendor info ====================
typedef struct {
    CpuVendor Vendor;
    const CHAR *Name;
    BOOL SupportsMicrocode;
    const CHAR *MsrMethod;
} CpuVendorInfo;

// ==================== Return codes ====================
#define MICROCODE_OK            0
#define MICROCODE_ERR_SIZE      -1
#define MICROCODE_ERR_CHECKSUM  -2
#define MICROCODE_ERR_SIG       -3
#define MICROCODE_ERR_MSR       -4
#define MICROCODE_ERR_UNSUPPORTED -5

// ==================== Public API ====================
CpuVendor CpuDetectVendor(NOPTR);
CpuVendorInfo *CpuGetVendorInfo(NOPTR);
INT CpuMicrocodeGetRevision(UINT32 *Revision);
INT CpuMicrocodeLoad(const UINT8 *Data, UINT32 Size);
