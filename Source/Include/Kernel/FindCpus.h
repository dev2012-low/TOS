#pragma once

#include <Kernel/Types.h>

#define FINDCPU_MAX_CPUS 256

/*
 * Information about each CPU
 */
typedef struct {
    UINT32 LapicId;
    UINT32 ApicVersion;
    UINT32 ProcessorId;
    BOOL Enabled;
    BOOL Bsp;
    UINT32 AcpiId;
} FindCpuInfo;

/*
 * Global system information
 */
typedef struct {
    UINT32 TotalLogicalCpus;
    UINT32 TotalCores;
    UINT32 ThreadsPerCore;
    UINT32 Packages;
    BOOL SmtEnabled;
    BOOL TopologyValid;
    FindCpuInfo Cpus[FINDCPU_MAX_CPUS];
    UINT32 CpuCount;
} SystemTopology;

/*
 * Functions
 */
NOPTR FindCpusDetect(SystemTopology *Topology);
UINT32 FindCpusGetBspId(NOPTR);
const CHAR* FindCpusGetTopologyString(SystemTopology *Topology);
UINT32 GetCpuCount(NOPTR);