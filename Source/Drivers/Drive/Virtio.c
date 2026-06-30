#include <Drive/Virtio.h>
#include <Drive/Drive.h>
#include <Pci.h>
#include <Memory/Allocator.h>
#include <Memory/PhysAlloc.h>
#include <Kernel/Paging.h>
#include <Kernel/Return.h>
#include <Kernel/KDriver.h>
#include <Asm/Mmio.h>
#include <Asm/Cpu.h>
#include <Asm/Io.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Console.h>
#include <Time/Timer.h>

#define VIRTIO_VENDOR_ID            0x1AF4
#define VIRTIO_DEV_BLK_LEGACY       0x1001
#define VIRTIO_DEV_BLK_MODERN       0x1042
#define VIRTIO_PCI_CAP_VNDR         0x09
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4

#define VIRTIO_STATUS_ACK           1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_FAILED        128

#define VIRTIO_BLK_F_SIZE_MAX       1
#define VIRTIO_BLK_T_IN             0
#define VIRTIO_BLK_T_OUT            1
#define VIRTIO_BLK_T_FLUSH          4

#define VIRTIO_LEGACY_REG_FEATURES  0x00
#define VIRTIO_LEGACY_REG_GFEATURES 0x04
#define VIRTIO_LEGACY_REG_QADDR     0x08
#define VIRTIO_LEGACY_REG_QSIZE     0x0C
#define VIRTIO_LEGACY_REG_QSELECT   0x0E
#define VIRTIO_LEGACY_REG_QNOTIFY   0x10
#define VIRTIO_LEGACY_REG_STATUS    0x12
#define VIRTIO_LEGACY_REG_ISR       0x13

#define VIRTIO_QUEUE_SIZE           8
#define VIRTIO_SECTOR_SIZE          512
#define VIRTIO_MAX_DRIVES           4

typedef struct {
    volatile UINT32 DeviceFeatureSelect;
    volatile UINT32 DeviceFeature;
    volatile UINT32 GuestFeatureSelect;
    volatile UINT32 GuestFeature;
    volatile UINT16 MsixConfig;
    volatile UINT16 NumQueues;
    volatile UINT8 DeviceStatus;
    volatile UINT8 ConfigGeneration;
    volatile UINT16 Reserved;
    volatile UINT16 QueueSelect;
    volatile UINT16 QueueSize;
    volatile UINT16 QueueMsixVector;
    volatile UINT16 QueueEnable;
    volatile UINT16 QueueNotifyOff;
    volatile UINT64 QueueDesc;
    volatile UINT64 QueueDriver;
    volatile UINT64 QueueDevice;
} VirtioPciCommonCfg;

typedef struct {
    UINT64 Addr;
    UINT32 Len;
    UINT16 Flags;
    UINT16 Next;
} VirtioDesc;

typedef struct {
    UINT16 Flags;
    UINT16 Idx;
    UINT16 Ring[VIRTIO_QUEUE_SIZE];
    UINT16 UsedEvent;
} VirtioAvail;

typedef struct {
    UINT32 Id;
    UINT32 Len;
} VirtioUsedElem;

typedef struct {
    UINT16 Flags;
    UINT16 Idx;
    VirtioUsedElem Ring[VIRTIO_QUEUE_SIZE];
    UINT16 AvailEvent;
} VirtioUsed;

typedef struct {
    UINT32 Type;
    UINT32 Reserved;
    UINT64 Sector;
} VirtioBlkReqHdr;

typedef struct {
    PciDevice *Pci;
    volatile VirtioPciCommonCfg *Common;
    volatile UINT8 *Notify;
    volatile UINT8 *DeviceCfg;
    volatile UINT8 *Isr;
    UINT32 NotifyOffset;
    UINT32 NotifyMultiplier;
    UINT16 IoBase;
    BOOL Legacy;
    UINT16 QueueSize;
    VirtioDesc *Desc;
    VirtioAvail *Avail;
    VirtioUsed *Used;
    UINT8 *ReqBuf;
    UINT8 *DataBuf;
    UINT8 *StatusByte;
    UINT64 CapacitySectors;
    Drive *RegisteredDrive;
} VirtioBlkDev;

static VirtioBlkDev GVirtioDevs[VIRTIO_MAX_DRIVES];
static UINT32 GVirtioCount = 0;

static inline UINT8 VirtioLegacyIn8(VirtioBlkDev *Dev, UINT16 Off) {
    return Inb(Dev->IoBase + Off);
}

static inline UINT16 VirtioLegacyIn16(VirtioBlkDev *Dev, UINT16 Off) {
    return Inw(Dev->IoBase + Off);
}

static inline NOPTR VirtioLegacyOut8(VirtioBlkDev *Dev, UINT16 Off, UINT8 Val) {
    Outb(Dev->IoBase + Off, Val);
}

static inline NOPTR VirtioLegacyOut16(VirtioBlkDev *Dev, UINT16 Off, UINT16 Val) {
    Outw(Dev->IoBase + Off, Val);
}

static inline UINT32 VirtioLegacyIn32(VirtioBlkDev *Dev, UINT16 Off) {
    return Inl(Dev->IoBase + Off);
}

static inline NOPTR VirtioLegacyOut32(VirtioBlkDev *Dev, UINT16 Off, UINT32 Val) {
    Outl(Dev->IoBase + Off, Val);
}

static BOOL VirtioWalkCaps(PciDevice *Dev, VirtioBlkDev *VDev) {
    UINT8 Cap = PciFindCap(Dev, VIRTIO_PCI_CAP_VNDR);

    while (Cap) {
        UINT32 CapHdr = PciRead(Dev, Cap);
        UINT8 Type = (CapHdr >> 24) & 0xFF;
        UINT8 Bar = (PciRead(Dev, Cap + 4) & 0xFF);
        UINT32 Offset = PciRead(Dev, Cap + 8);
        UINT64 BarAddr = Dev->Bars[Bar] & ~0xFULL;

        if (Type == VIRTIO_PCI_CAP_COMMON_CFG) {
            VDev->Common = (volatile VirtioPciCommonCfg *)(UINTPTR)(BarAddr + Offset);
        } else if (Type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
            VDev->Notify = (volatile UINT8 *)(UINTPTR)BarAddr;
            VDev->NotifyOffset = Offset;
            VDev->NotifyMultiplier = (PciRead(Dev, Cap + 16) & 0xFFFF);
            if (VDev->NotifyMultiplier == 0) {
                VDev->NotifyMultiplier = 4;
            }
        } else if (Type == VIRTIO_PCI_CAP_DEVICE_CFG) {
            VDev->DeviceCfg = (volatile UINT8 *)(UINTPTR)(BarAddr + Offset);
        } else if (Type == VIRTIO_PCI_CAP_ISR_CFG) {
            VDev->Isr = (volatile UINT8 *)(UINTPTR)(BarAddr + Offset);
        }

        Cap = (CapHdr >> 8) & 0xFF;
    }

    return (VDev->Common != NULLPTR);
}

static NOPTR VirtioSetStatus(VirtioBlkDev *Dev, UINT8 Status) {
    if (Dev->Legacy) {
        VirtioLegacyOut8(Dev, VIRTIO_LEGACY_REG_STATUS, Status);
    } else {
        MmioWrite8((volatile NOPTR *)&Dev->Common->DeviceStatus, Status);
    }
}

static UINT8 VirtioGetStatus(VirtioBlkDev *Dev) {
    if (Dev->Legacy) {
        return VirtioLegacyIn8(Dev, VIRTIO_LEGACY_REG_STATUS);
    }
    return MmioRead8((volatile NOPTR *)&Dev->Common->DeviceStatus);
}

static NOPTR VirtioReset(VirtioBlkDev *Dev) {
    if (Dev->Legacy) {
        VirtioLegacyOut8(Dev, VIRTIO_LEGACY_REG_STATUS, 0);
        TimerUdelay(1000);
        return;
    }
    MmioWrite8((volatile NOPTR *)&Dev->Common->DeviceStatus, 0);
    TimerUdelay(1000);
}

static BOOL VirtioSetupQueue(VirtioBlkDev *Dev) {
    UINT64 DescPhys;
    UINT64 AvailPhys;
    UINT64 UsedPhys;
    UINT64 QueuePhys;

    Dev->QueueSize = VIRTIO_QUEUE_SIZE;
    Dev->Desc = (VirtioDesc *)MemoryAllocate(PAGE_SIZE * 2);
    if (!Dev->Desc) {
        return FALSE;
    }

    Dev->Avail = (VirtioAvail *)((UINT8 *)Dev->Desc + PAGE_SIZE);
    Dev->Used = (VirtioUsed *)((UINT8 *)Dev->Avail + PAGE_SIZE);
    Dev->ReqBuf = (UINT8 *)MemoryAllocate(PAGE_SIZE);
    Dev->DataBuf = (UINT8 *)MemoryAllocate(PAGE_SIZE);
    Dev->StatusByte = (UINT8 *)MemoryAllocate(PAGE_SIZE);
    if (!Dev->ReqBuf || !Dev->DataBuf || !Dev->StatusByte) {
        return FALSE;
    }

    MemSet(Dev->Desc, 0, PAGE_SIZE * 2);
    MemSet(Dev->Avail, 0, PAGE_SIZE);
    MemSet(Dev->Used, 0, PAGE_SIZE);

    DescPhys = VirtToPhys((UINT64)(UINTPTR)Dev->Desc);
    AvailPhys = VirtToPhys((UINT64)(UINTPTR)Dev->Avail);
    UsedPhys = VirtToPhys((UINT64)(UINTPTR)Dev->Used);
    QueuePhys = DescPhys;

    if (Dev->Legacy) {
        VirtioLegacyOut16(Dev, VIRTIO_LEGACY_REG_QSELECT, 0);
        VirtioLegacyOut16(Dev, VIRTIO_LEGACY_REG_QSIZE, Dev->QueueSize);
        VirtioLegacyOut32(Dev, VIRTIO_LEGACY_REG_QADDR, (UINT32)(QueuePhys >> 12));
    } else {
        MmioWrite16((volatile NOPTR *)&Dev->Common->QueueSelect, 0);
        MmioWrite16((volatile NOPTR *)&Dev->Common->QueueSize, Dev->QueueSize);
        MmioWrite64((volatile NOPTR *)&Dev->Common->QueueDesc, DescPhys);
        MmioWrite64((volatile NOPTR *)&Dev->Common->QueueDriver, AvailPhys);
        MmioWrite64((volatile NOPTR *)&Dev->Common->QueueDevice, UsedPhys);
        MmioWrite16((volatile NOPTR *)&Dev->Common->QueueEnable, 1);
    }

    (void)AvailPhys;
    (void)UsedPhys;
    return TRUE;
}

static BOOL VirtioNegotiate(VirtioBlkDev *Dev) {
    UINT32 Features = 0;

    VirtioReset(Dev);
    VirtioSetStatus(Dev, VIRTIO_STATUS_ACK);
    VirtioSetStatus(Dev, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    if (Dev->Legacy) {
        Features = VirtioLegacyIn32(Dev, VIRTIO_LEGACY_REG_FEATURES);
        VirtioLegacyOut32(Dev, VIRTIO_LEGACY_REG_GFEATURES, Features & VIRTIO_BLK_F_SIZE_MAX);
    } else {
        MmioWrite32((volatile NOPTR *)&Dev->Common->GuestFeatureSelect, 0);
        MmioWrite32((volatile NOPTR *)&Dev->Common->GuestFeature, 0);
    }

    VirtioSetStatus(Dev, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    if (!(VirtioGetStatus(Dev) & VIRTIO_STATUS_FEATURES_OK)) {
        VirtioSetStatus(Dev, VIRTIO_STATUS_FAILED);
        return FALSE;
    }

    if (!VirtioSetupQueue(Dev)) {
        VirtioSetStatus(Dev, VIRTIO_STATUS_FAILED);
        return FALSE;
    }

    VirtioSetStatus(Dev, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                          VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    return TRUE;
}

static BOOL VirtioSubmit(VirtioBlkDev *Dev, UINT32 OutDescs, UINT32 InDescs, UINT16 *UsedLen) {
    UINT16 LastUsed;
    UINT16 Idx;
    UINT32 Spin;

    LastUsed = Dev->Used->Idx;
    Dev->Avail->Ring[Dev->Avail->Idx % Dev->QueueSize] = 0;
    Idx = Dev->Avail->Idx;
    Dev->Avail->Idx++;

    if (Dev->Legacy) {
        VirtioLegacyOut16(Dev, VIRTIO_LEGACY_REG_QSELECT, 0);
        VirtioLegacyOut16(Dev, VIRTIO_LEGACY_REG_QNOTIFY, 0);
    } else if (Dev->Notify) {
        volatile UINT16 *NotifyReg = (volatile UINT16 *)(Dev->Notify + Dev->NotifyOffset +
            (UINT64)MmioRead16((volatile NOPTR *)&Dev->Common->QueueNotifyOff) *
            Dev->NotifyMultiplier);
        *NotifyReg = 0;
    }

    Spin = 0;
    while (Dev->Used->Idx == LastUsed) {
        if (++Spin > 5000000) {
            return FALSE;
        }
        if (Dev->Isr) {
            (void)*Dev->Isr;
        }
        CpuPause();
    }

    if (UsedLen) {
        *UsedLen = Dev->Used->Ring[LastUsed % Dev->QueueSize].Len;
    }

    (void)OutDescs;
    (void)InDescs;
    return (Dev->StatusByte[0] == 0);
}

static INT VirtioBlkTransfer(VirtioBlkDev *Dev, UINT32 Type, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    VirtioBlkReqHdr *Hdr;
    UINT32 DataLen;
    UINT16 UsedLen;

    if (!Dev || !Buffer || Count == 0) {
        return INCORRECT_VALUE;
    }

    DataLen = Count * VIRTIO_SECTOR_SIZE;
    Hdr = (VirtioBlkReqHdr *)Dev->ReqBuf;
    Hdr->Type = Type;
    Hdr->Reserved = 0;
    Hdr->Sector = Lba;

    if (Type == VIRTIO_BLK_T_OUT) {
        MemCpy(Dev->DataBuf, Buffer, DataLen);
    }

    Dev->Desc[0].Addr = VirtToPhys((UINT64)(UINTPTR)Hdr);
    Dev->Desc[0].Len = sizeof(VirtioBlkReqHdr);
    Dev->Desc[0].Flags = 1;
    Dev->Desc[0].Next = 1;

    Dev->Desc[1].Addr = VirtToPhys((UINT64)(UINTPTR)Dev->DataBuf);
    Dev->Desc[1].Len = DataLen;
    Dev->Desc[1].Flags = (Type == VIRTIO_BLK_T_IN) ? 3 : 2;
    Dev->Desc[1].Next = 2;

    Dev->StatusByte[0] = 0xFF;
    Dev->Desc[2].Addr = VirtToPhys((UINT64)(UINTPTR)Dev->StatusByte);
    Dev->Desc[2].Len = 1;
    Dev->Desc[2].Flags = 2;
    Dev->Desc[2].Next = 0;

    if (!VirtioSubmit(Dev, (Type == VIRTIO_BLK_T_OUT) ? 2 : 1,
                      (Type == VIRTIO_BLK_T_IN) ? 2 : 1, &UsedLen)) {
        return DEVICE_ERROR;
    }

    if (Type == VIRTIO_BLK_T_IN) {
        MemCpy(Buffer, Dev->DataBuf, DataLen);
    }

    return SUCCESS;
}

static INT VirtioDriveRead(Drive *D, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    VirtioBlkDev *Dev = (VirtioBlkDev *)D->Priv;
    return VirtioBlkTransfer(Dev, VIRTIO_BLK_T_IN, Lba, Count, Buffer);
}

static INT VirtioDriveWrite(Drive *D, UINT64 Lba, UINT32 Count, const NOPTR *Buffer) {
    VirtioBlkDev *Dev = (VirtioBlkDev *)D->Priv;
    return VirtioBlkTransfer(Dev, VIRTIO_BLK_T_OUT, Lba, Count, (NOPTR *)Buffer);
}

static INT VirtioDriveSync(Drive *D) {
    VirtioBlkDev *Dev = (VirtioBlkDev *)D->Priv;
    VirtioBlkReqHdr *Hdr = (VirtioBlkReqHdr *)Dev->ReqBuf;
    UINT16 UsedLen;

    Hdr->Type = VIRTIO_BLK_T_FLUSH;
    Hdr->Reserved = 0;
    Hdr->Sector = 0;

    Dev->Desc[0].Addr = VirtToPhys((UINT64)(UINTPTR)Hdr);
    Dev->Desc[0].Len = sizeof(VirtioBlkReqHdr);
    Dev->Desc[0].Flags = 1;
    Dev->Desc[0].Next = 1;

    Dev->StatusByte[0] = 0xFF;
    Dev->Desc[1].Addr = VirtToPhys((UINT64)(UINTPTR)Dev->StatusByte);
    Dev->Desc[1].Len = 1;
    Dev->Desc[1].Flags = 2;
    Dev->Desc[1].Next = 0;

    if (!VirtioSubmit(Dev, 1, 1, &UsedLen)) {
        (void)D;
        return DEVICE_ERROR;
    }
    return SUCCESS;
}

static BOOL VirtioReadCapacity(VirtioBlkDev *Dev) {
    if (Dev->DeviceCfg) {
        Dev->CapacitySectors = MmioRead64((volatile NOPTR *)(Dev->DeviceCfg));
    } else if (Dev->Legacy) {
        Dev->CapacitySectors = 204800;
    } else {
        Dev->CapacitySectors = 0;
    }
    return Dev->CapacitySectors > 0;
}

static INT VirtioAttachDrive(VirtioBlkDev *Dev, UINT32 Index) {
    Drive *D;

    if (!VirtioNegotiate(Dev)) {
        return DEVICE_ERROR;
    }
    if (!VirtioReadCapacity(Dev)) {
        return DEVICE_ERROR;
    }

    D = (Drive *)MemoryAllocate(sizeof(Drive));
    if (!D) {
        return NO_MEMORY;
    }

    MemSet(D, 0, sizeof(Drive));
    D->Type = DRIVE_TYPE_VIRTIO;
    D->SectorSize = VIRTIO_SECTOR_SIZE;
    D->TotalSectors = Dev->CapacitySectors;
    D->Priv = Dev;
    D->Read = VirtioDriveRead;
    D->Write = VirtioDriveWrite;
    D->Sync = VirtioDriveSync;
    SnPrintf(D->Name, DRIVE_NAME_MAX, "VIRTIO%u", Index);
    Dev->RegisteredDrive = D;
    DriveRegister(D);
    ConsolePrint("[VirtIO] %s: %llu sectors\n", D->Name, Dev->CapacitySectors);
    return SUCCESS;
}

static INT VirtioProbeDevice(PciDevice *Pci) {
    VirtioBlkDev *Dev;
    UINT32 Index;

    if (!Pci) {
        return NO_OBJECT;
    }
    if (GVirtioCount >= VIRTIO_MAX_DRIVES) {
        return NO_MEMORY;
    }

    Index = GVirtioCount;
    Dev = &GVirtioDevs[Index];
    MemSet(Dev, 0, sizeof(*Dev));
    Dev->Pci = Pci;

    PciEnable(Pci);
    PciEnableBusmaster(Pci);

    if (Pci->DeviceId == VIRTIO_DEV_BLK_LEGACY && (Pci->Bars[0] & 1)) {
        Dev->Legacy = TRUE;
        Dev->IoBase = (UINT16)(Pci->Bars[0] & ~3);
    } else if (Pci->DeviceId == VIRTIO_DEV_BLK_MODERN || Pci->DeviceId == VIRTIO_DEV_BLK_LEGACY) {
        Dev->Legacy = FALSE;
        if (!VirtioWalkCaps(Pci, Dev)) {
            return NOT_FOUND;
        }
    } else {
        return NOT_FOUND;
    }

    if (IsError(VirtioAttachDrive(Dev, Index)).IsError) {
        return DEVICE_ERROR;
    }

    GVirtioCount++;
    KDriverRegister(KDriverGenerateStruct("VirtIO-Blk", DCL1, TRUE, Dev, NULLPTR));
    return SUCCESS;
}

INT VirtioInit(NOPTR) {
    PciDevice *Dev;

    ConsolePrint(" VirtIO drives found: ");
    Dev = PciGetFirst();
    while (Dev) {
        if (Dev->VendorId == VIRTIO_VENDOR_ID &&
            (Dev->DeviceId == VIRTIO_DEV_BLK_LEGACY || Dev->DeviceId == VIRTIO_DEV_BLK_MODERN)) {
            VirtioProbeDevice(Dev);
        }
        Dev = PciGetNext(Dev);
    }
    ConsolePrint("%u\n", GVirtioCount);
    RETURN(SUCCESS);
}

UINT32 VirtioGetDriveCount(NOPTR) {
    return GVirtioCount;
}
