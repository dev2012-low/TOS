#pragma once

#include <AcpiTables.h>
#include <Kernel/Types.h>

/* Processor info */
typedef struct {
    UINT32 AcpiProcessorUid;
    UINT32 ApicId;
    BOOL Enabled;
    BOOL X2Apic;
} AcpiProcessor;

/* IO APIC info */
typedef struct {
    UINT32 Address;
    UINT32 GsiBase;
} AcpiIoApic;

/* Interrupt override info */
typedef struct {
    UINT8  Bus;
    UINT8  Source;
    UINT32 Gsi;
    UINT16 Flags;
} AcpiIntOverride;

/* APIC info */
typedef struct {
    UINT32 LocalApicAddress;
    BOOL UsesX2Apic;
    UINT32 ProcessorCount;
    AcpiProcessor Processors[64];
    UINT32 IoApicCount;
    AcpiIoApic IoApics[16];
    UINT32 IntOverrideCount;
    AcpiIntOverride IntOverrides[16];
} ApicInfo;

/* Main ACPI structure */
typedef struct {
    RSDPV2  *Rsdp;
    RSDT    *Rsdt;
    XSDT    *Xsdt;
    FADT    *Fadt;
    MADT    *Madt;
    HPET    *Hpet;
    MCFG    *Mcfg;
    SDTHeader *Dsdt;
    SDTHeader *Ssdts[16];
    INT SsdtCount;
    
    BOOL     UseXsdt;
    ApicInfo Apic;

    UINT32   ApicBusFreq;
} Acpi;

/* Initialization and table access */
INT AcpiInit(UINT64 RsdpAddr);
Acpi *AcpiGetTable(NOPTR);
UINT8 AcpiChecksum(NOPTR *Table, UINT32 Length);

/* Table finding */
NOPTR *AcpiFindTable(const CHAR *Signature);
NOPTR *AcpiFindTableWithIndex(const CHAR *Signature, INT Index);
INT AcpiParseMadt(NOPTR);
UINT32 AcpiGetLocalApicAddr(NOPTR);
INT AcpiEnableLocalApic(NOPTR);

/* Power management */
NOPTR AcpiReboot(NOPTR);
NOPTR AcpiShutdown(NOPTR);
NOPTR AcpiSleep(UINT8 SleepType);