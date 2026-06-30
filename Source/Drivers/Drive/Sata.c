#include <Drive/Sata.h>
#include <Pci.h>
#include <Memory/PhysAlloc.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Kernel/KDriver.h>
#include <Asm/Cpu.h>
#include <Kernel/Return.h>

// ==================== GLOBALS ====================
static HbaMem* GSataHba = NULLPTR;
static UINTPTR GSataPhysBase = 0;
static UINTPTR GSataVirtBase = 0;
static PciDevice* GSataPciDev = NULLPTR;
static SataPort* GSataPorts[32];

// ==================== HELPER FUNCTIONS ====================

static inline UINT64 SataGetTicksMs(NOPTR) {
    return TimerTicks();
}

static inline NOPTR SataMdelay(UINT32 Ms) {
    TimerMdelay(Ms);
}

static inline NOPTR SataUdelay(UINT32 Us) {
    TimerUdelay(Us);
}

static NOPTR SataStopCmd(HbaPort* Port) {
    Port->Cmd &= ~SATA_HBA_PxCMD_ST;
    
    UINT64 Timeout = SataGetTicksMs() + 100;
    while ((Port->Cmd & SATA_HBA_PxCMD_CR) && SataGetTicksMs() < Timeout) {
        CpuPause();
    }
    
    Port->Cmd &= ~SATA_HBA_PxCMD_FRE;
}

static NOPTR SataStartCmd(HbaPort* Port) {
    Port->Cmd &= ~SATA_HBA_PxCMD_ST;
    
    UINT64 Timeout = SataGetTicksMs() + 100;
    while ((Port->Cmd & SATA_HBA_PxCMD_CR) && SataGetTicksMs() < Timeout) {
        CpuPause();
    }
    
    Port->Cmd |= SATA_HBA_PxCMD_FRE;
    Port->Cmd |= SATA_HBA_PxCMD_ST;
}

static INT SataFindCmdSlot(HbaPort* Port) {
    UINT32 Slots = Port->Sact | Port->Ci;
    for (INT I = 0; I < 8; I++) {
        if ((Slots & 1) == 0)
            return I;
        Slots >>= 1;
    }
    return -1;
}

static INT SataAcquireBuffer(SataPort* Port) {
    while (__sync_lock_test_and_set(&Port->BufferSemaphore, 1)) {
        CpuPause();
    }
    
    for (INT I = 0; I < 8; I++) {
        if (!__sync_lock_test_and_set(&Port->BufferLocks[I], 1)) {
            __sync_lock_release(&Port->BufferSemaphore);
            return I;
        }
    }
    
    __sync_lock_release(&Port->BufferSemaphore);
    return -1;
}

static NOPTR SataReleaseBuffer(SataPort* Port, INT Index) {
    if (Index < 0 || Index >= 8) return;
    __sync_lock_release(&Port->BufferLocks[Index]);
    __sync_lock_release(&Port->BufferSemaphore);
}

static NOPTR SataPortLock(SataPort* Port) {
    while (__sync_lock_test_and_set(&Port->PortLock, 1)) {
        CpuPause();
    }
}

static NOPTR SataPortUnlock(SataPort* Port) {
    __sync_synchronize();
    __sync_lock_release(&Port->PortLock);
}

static NOPTR* SataAllocDmaPage(NOPTR) {
    NOPTR* Phys = PhysAllocAllocatePage(PhysAllocGet());
    if (!Phys) return NULLPTR;
    MemSet((NOPTR*)(UINTPTR)Phys, 0, PAGE_SIZE);
    return Phys;
}

// ==================== PORT ACCESS ====================

static INT SataPortAccess(SataPort* Port, UINT64 Lba, UINT32 Count, 
                            UINTPTR PhysBuffer, INT Write) {
    SataPortLock(Port);
    
    HbaPort* Regs = Port->Regs;
    Regs->Ie = 0xFFFFFFFF;
    Regs->Is = 0;
    
    INT Slot = SataFindCmdSlot(Regs);
    if (Slot == -1) {
        SataPortUnlock(Port);
        RETURN(INCORRECT_VALUE);
    }
    
    Regs->Serr = 0;
    Regs->Tfd = 0;
    
    HbaCmdHeader* CmdHeader = &Port->CmdList[Slot];
    
    CmdHeader->Cfl = sizeof(FisRegH2D) / sizeof(UINT32);
    CmdHeader->A = 0;
    CmdHeader->W = Write;
    CmdHeader->C = 0;
    CmdHeader->P = 0;
    CmdHeader->Prdbc = 0;
    CmdHeader->Pmp = 0;
    
    HbaCmdTbl* CmdTbl = Port->CmdTables[Slot];
    MemSet(CmdTbl, 0, sizeof(HbaCmdTbl));
    
    CmdTbl->PrdtEntry[0].Dba = (UINT32)(PhysBuffer & 0xFFFFFFFF);
    CmdTbl->PrdtEntry[0].DbaU = (UINT32)((PhysBuffer >> 32) & 0xFFFFFFFF);
    CmdTbl->PrdtEntry[0].Dbc = Port->SectorSize * Count - 1;
    CmdTbl->PrdtEntry[0].I = 1;
    
    FisRegH2D* CmdFis = (FisRegH2D*)CmdTbl->CFis;
    MemSet(CmdTbl->CFis, 0, sizeof(FisRegH2D));
    
    CmdFis->FisType = SATA_FIS_TYPE_REG_H2D;
    CmdFis->C = 1;
    CmdFis->PmPort = 0;
    
    if (Write) {
        CmdFis->Command = SATA_ATA_CMD_WRITE_DMA_EX;
    } else {
        CmdFis->Command = SATA_ATA_CMD_READ_DMA_EX;
    }
    
    CmdFis->Lba0 = (UINT8)(Lba & 0xFF);
    CmdFis->Lba1 = (UINT8)((Lba >> 8) & 0xFF);
    CmdFis->Lba2 = (UINT8)((Lba >> 16) & 0xFF);
    CmdFis->Device = 1 << 6;
    
    CmdFis->Lba3 = (UINT8)((Lba >> 24) & 0xFF);
    CmdFis->Lba4 = (UINT8)((Lba >> 32) & 0xFF);
    CmdFis->Lba5 = (UINT8)((Lba >> 40) & 0xFF);
    
    CmdFis->CountL = (UINT8)(Count & 0xFF);
    CmdFis->CountH = (UINT8)(Count >> 8);
    
    CmdFis->Control = 0x8;
    
    // Wait for device ready
    UINT64 Timeout = SataGetTicksMs() + 100;
    while ((Regs->Tfd & (SATA_ATA_DEV_BUSY | SATA_ATA_DEV_DRQ)) && SataGetTicksMs() < Timeout) {
        CpuPause();
    }
    
    if (SataGetTicksMs() >= Timeout) {
        SataPortUnlock(Port);
        RETURN(TIMEOUT);
    }
    
    Regs->Ie = 0xFFFFFFFF;
    Regs->Is = 0xFFFFFFFF;
    
    SataStartCmd(Regs);
    Regs->Ci |= 1 << Slot;
    
    // Wait for command completion
    Timeout = SataGetTicksMs() + 200;
    while ((Regs->Ci & (1 << Slot)) && SataGetTicksMs() < Timeout) {
        if (Regs->Is & SATA_HBA_PxIS_TFES) {
            SataStopCmd(Regs);
            SataPortUnlock(Port);
            RETURN(DEVICE_ERROR);
        }
        CpuPause;
    }
    
    if (SataGetTicksMs() >= Timeout) {
        SataStopCmd(Regs);
        SataPortUnlock(Port);
        RETURN(TIMEOUT);
    }
    
    Timeout = SataGetTicksMs() + 100;
    while ((Regs->Tfd & (SATA_ATA_DEV_BUSY | SATA_ATA_DEV_DRQ)) && SataGetTicksMs() < Timeout) {
        CpuPause();
    }
    
    SataStopCmd(Regs);
    
    if (Regs->Is & SATA_HBA_PxIS_TFES) {
        SataPortUnlock(Port);
        RETURN(DEVICE_ERROR);
    }
    
    SataPortUnlock(Port);
    RETURN(SUCCESS);
}

// ==================== IDENTIFY ====================

static NOPTR SataPortIdentify(SataPort* Port) {
    HbaPort* Regs = Port->Regs;
    
    Regs->Ie = 0xFFFFFFFF;
    Regs->Is = 0;
    
    INT Slot = SataFindCmdSlot(Regs);
    if (Slot == -1) return;
    
    Regs->Tfd = 0;
    
    HbaCmdHeader* CmdHeader = &Port->CmdList[Slot];
    
    CmdHeader->Cfl = sizeof(FisRegH2D) / sizeof(UINT32);
    CmdHeader->A = 0;
    CmdHeader->W = 0;
    CmdHeader->C = 0;
    CmdHeader->P = 0;
    CmdHeader->Prdbc = 0;
    CmdHeader->Pmp = 0;
    
    HbaCmdTbl* CmdTbl = Port->CmdTables[Slot];
    MemSet(CmdTbl, 0, sizeof(HbaCmdTbl));
    
    UINTPTR PhysBuf = Port->PhysBuffers[0];
    
    CmdTbl->PrdtEntry[0].Dba = (UINT32)(PhysBuf & 0xFFFFFFFF);
    CmdTbl->PrdtEntry[0].DbaU = (UINT32)((PhysBuf >> 32) & 0xFFFFFFFF);
    CmdTbl->PrdtEntry[0].Dbc = 512 - 1;
    CmdTbl->PrdtEntry[0].I = 1;
    
    FisRegH2D* CmdFis = (FisRegH2D*)CmdTbl->CFis;
    MemSet(CmdTbl->CFis, 0, sizeof(FisRegH2D));
    
    CmdFis->FisType = SATA_FIS_TYPE_REG_H2D;
    CmdFis->C = 1;
    CmdFis->PmPort = 0;
    CmdFis->Command = SATA_ATA_CMD_IDENTIFY;
    
    CmdFis->Lba0 = 0;
    CmdFis->Lba1 = 0;
    CmdFis->Lba2 = 0;
    CmdFis->Device = 0;
    CmdFis->Lba3 = 0;
    CmdFis->Lba4 = 0;
    CmdFis->Lba5 = 0;
    CmdFis->CountL = 0;
    CmdFis->CountH = 0;
    CmdFis->Control = 0;
    
    // Wait for device ready
    UINT64 Timeout = SataGetTicksMs() + 100;
    while ((Regs->Tfd & (SATA_ATA_DEV_BUSY | SATA_ATA_DEV_DRQ)) && SataGetTicksMs() < Timeout) {
        CpuPause();
    }
    
    Regs->Ie = 0xFFFFFFFF;
    Regs->Is = 0xFFFFFFFF;
    
    SataStartCmd(Regs);
    Regs->Ci |= 1 << Slot;
    
    Timeout = SataGetTicksMs() + 200;
    while ((Regs->Ci & (1 << Slot)) && SataGetTicksMs() < Timeout) {
        if (Regs->Is & SATA_HBA_PxIS_TFES) {
            SataStopCmd(Regs);
            return;
        }
        CpuPause();
    }
    
    SataStopCmd(Regs);
    
    if (Regs->Is & SATA_HBA_PxIS_TFES) {
        return;
    }
    
    // Parse IDENTIFY data
    UINT16* Identify = (UINT16*)Port->VirtBuffers[0];
    
    // Get sector size
    if (Identify[106] & (1 << 12)) {
        Port->SectorSize = (Identify[106] & (1 << 12)) ? 512 : 4096;
    } else {
        Port->SectorSize = 512;
    }
    
    // Get total sectors
    if (Identify[83] & (1 << 10)) {
        Port->SupportsLba48 = 1;
        Port->TotalSectors = *(UINT64*)&Identify[100];
    } else {
        Port->SupportsLba48 = 0;
        Port->TotalSectors = *(UINT32*)&Identify[60];
    }
}

// ==================== PORT INITIALIZATION ====================

static SataPort* SataPortInit(INT PortNum, HbaPort* PortRegs, HbaMem* Hba) {
    
    //Step 1: select the port structure
    SataPort* Port = (SataPort*)MemoryAllocate(sizeof(SataPort));
    if (!Port) {
        return NULLPTR;
    }
    
    MemSet(Port, 0, sizeof(SataPort));
    Port->Regs = PortRegs;
    Port->PortNum = PortNum;
    Port->Status = SATA_PORT_UNINITIALIZED;
    Port->SectorSize = 512;
    
    //Step 2: Stop the command engine
    PortRegs->Cmd &= ~SATA_HBA_PxCMD_ST;
    PortRegs->Cmd &= ~SATA_HBA_PxCMD_FRE;
    SataStopCmd(PortRegs);
    //Step 3: select command list
    NOPTR* CmdListPhys = SataAllocDmaPage();
    if (!CmdListPhys) {
        MemoryFree(Port);
        return NULLPTR;
    }
    
    PortRegs->Clb = (UINT32)((UINTPTR)CmdListPhys & 0xFFFFFFFF);
    PortRegs->Clbu = (UINT32)((UINTPTR)CmdListPhys >> 32);
    Port->CmdList = (HbaCmdHeader*)CmdListPhys;
    
    //Step 4: Select FIS
    NOPTR* FisPhys = SataAllocDmaPage();
    if (!FisPhys) {
        MemoryFree(Port);
        return NULLPTR;
    }
    
    PortRegs->Fb = (UINT32)((UINTPTR)FisPhys & 0xFFFFFFFF);
    PortRegs->Fbu = (UINT32)((UINTPTR)FisPhys >> 32);
    Port->Fis = (HbaFis*)FisPhys;
    
    //Step 5: initialize FIS types
    Port->Fis->DsFis.FisType = SATA_FIS_TYPE_DMA_SETUP;
    Port->Fis->PsFis.FisType = SATA_FIS_TYPE_PIO_SETUP;
    Port->Fis->RFis.FisType = SATA_FIS_TYPE_REG_D2H;
    Port->Fis->SdbFis[0] = SATA_FIS_TYPE_DEV_BITS;
    
    //Step 6: allocate command tables for 8 slots
    for (INT I = 0; I < 8; I++) {
        NOPTR* TblPhys = SataAllocDmaPage();
        if (!TblPhys) {
            MemoryFree(Port);
            return NULLPTR;
        }
        
        Port->CmdList[I].Prdtl = 1;
        Port->CmdList[I].Ctba = (UINT32)((UINTPTR)TblPhys & 0xFFFFFFFF);
        Port->CmdList[I].Ctbau = (UINT32)((UINTPTR)TblPhys >> 32);
        Port->CmdTables[I] = (HbaCmdTbl*)TblPhys;
    }
    
    //Step 7: allocate DMA buffers
    for (INT I = 0; I < 8; I++) {
        NOPTR* BufPhys = SataAllocDmaPage();
        if (!BufPhys) {
            MemoryFree(Port);
            return NULLPTR;
        }
        
        Port->PhysBuffers[I] = (UINTPTR)BufPhys;
        Port->VirtBuffers[I] = (NOPTR*)(UINTPTR)BufPhys;
        Port->BufferLocks[I] = 0;
    }
    
    Port->BufferSemaphore = 0;
    Port->PortLock = 0;
    
    //Step 8: configure the port port_num);
    PortRegs->Sctl |= (SATA_SCTL_PORT_IPM_NOPART | SATA_SCTL_PORT_IPM_NOSLUM | SATA_SCTL_PORT_IPM_NODSLP);
    
    if (Hba->Cap & SATA_CAP_SALP) {
        PortRegs->Cmd &= ~SATA_HBA_PxCMD_ASP;
    }
    
    PortRegs->Is = 0;
    PortRegs->Ie = 1;
    PortRegs->Cmd |= SATA_HBA_PxCMD_POD;
    PortRegs->Cmd |= SATA_HBA_PxCMD_SUD;
    
    SataMdelay(10);
    
    //Step 9: check the presence of the device
    INT Spin = 100;
    INT DetValue = 0;
    while (Spin-- > 0) {
        DetValue = PortRegs->Ssts & SATA_HBA_PxSSTS_DET;
        if (DetValue == SATA_HBA_PxSSTS_DET_PRESENT) {
            break;
        }
        if (Spin % 20 == 0) {
        }
        SataMdelay(1);
    }
    
    if (DetValue != SATA_HBA_PxSSTS_DET_PRESENT) {
        Port->Status = SATA_PORT_ERROR;
        return Port;
    }
    
    //Step 10: Activate the INTerface
    PortRegs->Cmd = (PortRegs->Cmd & ~SATA_HBA_PxCMD_ICC) | SATA_HBA_PxCMD_ICC_ACTIVE;
    
    //Step 11: Wait for the device to be ready
    Spin = 1000;
    INT TfdValue = 0;
    while (Spin-- > 0) {
        TfdValue = PortRegs->Tfd & (SATA_ATA_DEV_BUSY | SATA_ATA_DEV_DRQ);
        if (!TfdValue) {
            break;
        }
        if (Spin % 100 == 0) {
            SataMdelay(1);
        }
    }
    
    if (Spin <= 0) {
        Port->Status = SATA_PORT_ERROR;
        return Port;
    }
    
    Port->Status = SATA_PORT_ACTIVE;

    SataPortIdentify(Port);
    return Port;
}

// ==================== PUBLIC FUNCTIONS ====================

INT SataInit(NOPTR) {
    GSataPciDev = PciFindClass(0x01, 0x06);
    if (!GSataPciDev) {
        RETURN(NOT_FOUND);
    }
    if (GSataPciDev->ProgIf != 0x01) {
        RETURN(NOT_FOUND);
    }
    PciEnable(GSataPciDev);
    PciEnableBusmaster(GSataPciDev);
    GSataPhysBase = GSataPciDev->Bars[5] & ~0xF;
    if (!GSataPhysBase) {
        RETURN(NO_OBJECT);
    }
    
    GSataVirtBase = GSataPhysBase;
    GSataHba = (HbaMem*)GSataVirtBase;
    UINT32 Timeout = SataGetTicksMs() + 100;
    while (!(GSataHba->Ghc & SATA_GHC_ENABLE) && SataGetTicksMs() < Timeout) {
        GSataHba->Ghc |= SATA_GHC_ENABLE;
        SataMdelay(1);
    }
    
    GSataHba->Ghc &= ~SATA_GHC_IE;
    
    GSataHba->Is = 0xFFFFFFFF;
    
    UINT32 Pi = GSataHba->Pi;
    
    for (INT I = 0; I < 32; I++) {
        if ((Pi >> I) & 1) {            
            UINT32 Ipm = (GSataHba->Ports[I].Ssts >> 8) & 0x0F;
            UINT32 Det = GSataHba->Ports[I].Ssts & SATA_HBA_PxSSTS_DET;
            
            if (Ipm != SATA_HBA_PORT_IPM_ACTIVE) {
                continue;
            }
            if (Det != SATA_HBA_PxSSTS_DET_PRESENT) {
                continue;
            }
            
            UINT32 Sig = GSataHba->Ports[I].Sig;
            if (Sig == SATA_SIG_ATAPI) {
                continue;
            }
            if (Sig == SATA_SIG_PM) {
                continue;
            }
            if (Sig == SATA_SIG_SEMB) {
                continue;
            }
            
            SataPort* Port = SataPortInit(I, &GSataHba->Ports[I], GSataHba);
            
            if (Port) {
                if (Port->Status == SATA_PORT_ACTIVE) {
                    GSataPorts[I] = Port;
                }
            }
        }
    }
    
    INT ActiveCount = 0;
    for (INT I = 0; I < 32; I++) {
        if (GSataPorts[I] && GSataPorts[I]->Status == SATA_PORT_ACTIVE) {
            ActiveCount++;
        }
    }
    
    RETURN(SUCCESS);
}

// ==================== DISK OPERATIONS ====================

INT SataPortRead(SataPort *Port, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    if (!Port || Port->Status != SATA_PORT_ACTIVE) RETURN(NO_OBJECT);
    
    UINT32 SectorSize = Port->SectorSize;
    UINT8* BufPtr = (UINT8*)Buffer;
    UINT64 RemainingSectors = (Count + SectorSize - 1) / SectorSize;
    
    INT BufIdx = SataAcquireBuffer(Port);
    if (BufIdx < 0) RETURN(INCORRECT_VALUE);
    
    UINTPTR PhysBuf = Port->PhysBuffers[BufIdx];
    NOPTR* VirtBuf = Port->VirtBuffers[BufIdx];
    
    while (RemainingSectors > 0) {
        UINT32 SectorsThis = (RemainingSectors > 8) ? 8 : (UINT32)RemainingSectors;
        UINT32 BytesThis = SectorsThis * SectorSize;
        
        INT Ret = SataPortAccess(Port, Lba, SectorsThis, PhysBuf, 0);
        if (Ret != 0) {
            SataReleaseBuffer(Port, BufIdx);
            RETURN(INCORRECT_VALUE);
        }
        
        MemCpy(BufPtr, VirtBuf, BytesThis);
        
        BufPtr += BytesThis;
        Lba += SectorsThis;
        RemainingSectors -= SectorsThis;
    }
    
    SataReleaseBuffer(Port, BufIdx);
    RETURN(SUCCESS);
}

INT SataPortWrite(SataPort *Port, UINT64 Lba, UINT32 Count, const NOPTR *Buffer) {
    if (!Port || Port->Status != SATA_PORT_ACTIVE) RETURN(NO_OBJECT);
    
    UINT32 SectorSize = Port->SectorSize;
    const UINT8* BufPtr = (const UINT8*)Buffer;
    UINT64 RemainingSectors = (Count + SectorSize - 1) / SectorSize;
    
    INT BufIdx = SataAcquireBuffer(Port);
    if (BufIdx < 0) RETURN(INCORRECT_VALUE);
    
    UINTPTR PhysBuf = Port->PhysBuffers[BufIdx];
    NOPTR* VirtBuf = Port->VirtBuffers[BufIdx];
    
    while (RemainingSectors > 0) {
        UINT32 SectorsThis = (RemainingSectors > 8) ? 8 : (UINT32)RemainingSectors;
        UINT32 BytesThis = SectorsThis * SectorSize;
        
        MemCpy(VirtBuf, BufPtr, BytesThis);
        
        INT Ret = SataPortAccess(Port, Lba, SectorsThis, PhysBuf, 1);
        if (Ret != 0) {
            SataReleaseBuffer(Port, BufIdx);
            RETURN(INCORRECT_VALUE);
        }
        
        BufPtr += BytesThis;
        Lba += SectorsThis;
        RemainingSectors -= SectorsThis;
    }
    
    SataReleaseBuffer(Port, BufIdx);
    RETURN(SUCCESS);
}

SataPort* SataGetPort(INT PortNum) {
    if (PortNum < 0 || PortNum >= 32) return NULLPTR;
    return GSataPorts[PortNum];
}
