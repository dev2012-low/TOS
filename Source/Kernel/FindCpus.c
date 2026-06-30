#include <Kernel/FindCpus.h>
#include <Asm/Cpu.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Apic.h>

/*
 * Global variable for storing topology (for get_cpu_count)
 */
static SystemTopology GSystemTopology = {0};
static BOOL GTopologyInitialized = FALSE;

/*
 * Helper function for extracting bits
 */
static inline UINT32 ExtractBits(UINT32 Value, UINT32 High, UINT32 Low) {
    return (Value >> Low) & ((1 << (High - Low + 1)) - 1);
}

/*
 * Step 1: Check CPUID leaf 0x0B support
 */
static BOOL SupportsLeaf0xB(NOPTR) {
    UINT32 MaxLeaf;
    UINT32 Dummy;
    
    Cpuid(0, &MaxLeaf, &Dummy, &Dummy, &Dummy);
    
    return (MaxLeaf >= 0x0B);
}

/*
 * Getting the LAPIC ID of the current CPU
 */
static UINT32 GetCurrentLapicId(NOPTR) {
    UINT32 Eax, Ebx, Ecx, Edx;
    
    /*
 * CPUID leaf 0x0B, ECX=0 gives APIC ID in EDX[31:24]
 */
    if (SupportsLeaf0xB()) {
        CpuidLeaf(0x0B, 0, &Eax, &Ebx, &Ecx, &Edx);
        return (Edx >> 24) & 0xFF;
    }
    
    /*
 * Fallback: leaf 0x01
 */
    Cpuid(0x01, &Eax, &Ebx, &Ecx, &Edx);
    return (Ebx >> 24) & 0xFF;
}

/*
 * Step 1a: Legacy method (for older CPUs without leaf 0x0B)
 */
static NOPTR DetectLegacy(SystemTopology *Topology) {
    UINT32 Eax, Ebx, Ecx, Edx;
    
    /*
 * CPUID leaf 0x01: LogicalProcessorsPerPackage
 */
    Cpuid(0x01, &Eax, &Ebx, &Ecx, &Edx);
    
    /*
 * EBX[23:16] = Logical processors per package
 */
    UINT32 LogicalPerPackage = ExtractBits(Ebx, 23, 16);
    if (LogicalPerPackage == 0) LogicalPerPackage = 1;
    
    /*
 * CPUID leaf 0x04, ECX=0: Cores per package
 */
    CpuidLeaf(0x04, 0, &Eax, &Ebx, &Ecx, &Edx);
    
    /*
 * EAX[31:26] = Cores per package (APIC ID shift count)
 */
    UINT32 CoresPerPackage = ExtractBits(Eax, 31, 26);
    
    /*
 * If cores_per_package == 0, then there is 1 core
 */
    if (CoresPerPackage == 0) {
        CoresPerPackage = 1;
    }
    
    Topology->TotalCores = CoresPerPackage;
    Topology->TotalLogicalCpus = LogicalPerPackage;
    Topology->ThreadsPerCore = LogicalPerPackage / CoresPerPackage;
    Topology->SmtEnabled = (LogicalPerPackage > CoresPerPackage);
    Topology->Packages = 1; /*
 * Legacy method does not detect packages
 */
    Topology->TopologyValid = TRUE;
}

/*
 * Step 2: Extended Topology Enumeration (leaf 0x0B)
 */
static NOPTR DetectExtendedTopology(SystemTopology *Topology) {
    UINT32 Eax, Ebx, Ecx, Edx;
    UINT32 SmtLevelCores = 0;
    UINT32 CoreLevelCores = 0;
    UINT32 ThreadsPerCore = 1;
    UINT32 CoresPerPackage = 1;
    
    /*
 * Level 0: SMT (streams)
 */
    CpuidLeaf(0x0B, 0, &Eax, &Ebx, &Ecx, &Edx);
    SmtLevelCores = Ebx & 0xFFFF;  /*
 * Number of logical processors at this level
 */
    
    /*
 * Level 1: Core
 */
    CpuidLeaf(0x0B, 1, &Eax, &Ebx, &Ecx, &Edx);
    CoreLevelCores = Ebx & 0xFFFF;
    

    if (CoreLevelCores == 0) {
        CoreLevelCores = 1;
        ThreadsPerCore = SmtLevelCores;
        CoresPerPackage = 1;
    } else {
        ThreadsPerCore = SmtLevelCores;
        CoresPerPackage = CoreLevelCores / SmtLevelCores;
    }
    
    Topology->TotalCores = CoresPerPackage;
    Topology->TotalLogicalCpus = CoreLevelCores;
    Topology->ThreadsPerCore = ThreadsPerCore;
    Topology->SmtEnabled = (ThreadsPerCore > 1);
    Topology->Packages = 1;
    Topology->TopologyValid = TRUE;
}

/*
 * Main CPU detection function
 */
NOPTR FindCpusDetect(SystemTopology *Topology) {
    if (!Topology) return;
    
    MemSet(Topology, 0, sizeof(SystemTopology));
    
    if (SupportsLeaf0xB()) {
        DetectExtendedTopology(Topology);
    } else {
        DetectLegacy(Topology);
    }
    
    /*
 * Fill in information about each CPU (simplified - BSP only)
 */
    Topology->CpuCount = Topology->TotalLogicalCpus;
    if (Topology->CpuCount > FINDCPU_MAX_CPUS) {
        Topology->CpuCount = FINDCPU_MAX_CPUS;
    }
    
    /*
 * Fill in BSP (current CPU)
 */
    Topology->Cpus[0].LapicId = GetCurrentLapicId();
    Topology->Cpus[0].ApicVersion = ApicGetVersion();
    Topology->Cpus[0].ProcessorId = 0;
    Topology->Cpus[0].Enabled = TRUE;
    Topology->Cpus[0].Bsp = TRUE;
    
    /*
 * For the rest of the CPUs, for now we just fill in the stubs
 */
    for (UINT32 I = 1; I < Topology->CpuCount; I++) {
        Topology->Cpus[I].LapicId = I;  /*
 * TODO: real APIC IDs from MADT/MP table
 */
        Topology->Cpus[I].Enabled = TRUE;
        Topology->Cpus[I].Bsp = FALSE;
    }
    
    GSystemTopology = *Topology;
    GTopologyInitialized = TRUE;
}

/*
 * Get BSP ID
 */
UINT32 FindCpusGetBspId(NOPTR) {
    if (!GTopologyInitialized) {
        return 0;
    }
    
    for (UINT32 I = 0; I < GSystemTopology.CpuCount; I++) {
        if (GSystemTopology.Cpus[I].Bsp) {
            return GSystemTopology.Cpus[I].LapicId;
        }
    }
    
    return GetCurrentLapicId();
}

/*
 * Get a string representation of the topology
 */
const CHAR* FindCpusGetTopologyString(SystemTopology *Topology) {
    static CHAR Buffer[128];
    
    if (!Topology || !Topology->TopologyValid) {
        return "Unknown";
    }
    
    SnPrintf(Buffer, sizeof(Buffer), "%uC/%uT (%u logical, SMT=%s)",
             Topology->TotalCores,
             Topology->ThreadsPerCore,
             Topology->TotalLogicalCpus,
             Topology->SmtEnabled ? "on" : "off");
    
    return Buffer;
}

/*
 * SIMPLE FUNCTION TO GET THE NUMBER OF CPU
 */
UINT32 GetCpuCount(NOPTR) {
    /*
 * If the topology is already initialized, return from it
 */
    if (GTopologyInitialized) {
        return GSystemTopology.TotalLogicalCpus;
    }
    
    /*
 * Otherwise we detect on the fly
 */
    SystemTopology TempTopology;
    FindCpusDetect(&TempTopology);
    
    return TempTopology.TotalLogicalCpus;
}

/*
 * Additional function: get the number of physical cores
 */
UINT32 GetCoreCount(NOPTR) {
    if (GTopologyInitialized) {
        return GSystemTopology.TotalCores;
    }
    
    SystemTopology TempTopology;
    FindCpusDetect(&TempTopology);
    
    return TempTopology.TotalCores;
}

/*
 * Check if SMT/Hyper-Threading is enabled
 */
BOOL IsSmtEnabled(NOPTR) {
    if (GTopologyInitialized) {
        return GSystemTopology.SmtEnabled;
    }
    
    SystemTopology TempTopology;
    FindCpusDetect(&TempTopology);
    
    return TempTopology.SmtEnabled;
}
