#include <Network/VirtioNet.h>
#include <Network/Network.h>
#include <Network/IpV4.h>
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
#define VIRTIO_DEV_NET_LEGACY       0x1000
#define VIRTIO_DEV_NET_MODERN       0x1041
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

#define VIRTIO_NET_F_MAC            (1U << 5)

#define VIRTIO_NET_QUEUE_RX         0
#define VIRTIO_NET_QUEUE_TX         1
#define VIRTIO_NET_HDR_LEN          10

#define VIRTIO_LEGACY_REG_FEATURES  0x00
#define VIRTIO_LEGACY_REG_GFEATURES 0x04
#define VIRTIO_LEGACY_REG_QADDR     0x08
#define VIRTIO_LEGACY_REG_QSIZE     0x0C
#define VIRTIO_LEGACY_REG_QSELECT   0x0E
#define VIRTIO_LEGACY_REG_QNOTIFY   0x10
#define VIRTIO_LEGACY_REG_STATUS    0x12
#define VIRTIO_LEGACY_REG_ISR       0x13

#define VIRTIO_QUEUE_SIZE           16
#define VIRTIO_MAX_NET_DEVS         4
#define VIRTIO_NET_RX_BUF_SIZE      (VIRTIO_NET_HDR_LEN + NETWORK_MTU + 2)

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
    UINT8 Flags;
    UINT8 GsoType;
    UINT16 HdrLen;
    UINT16 CsumStart;
    UINT16 CsumOffset;
} VirtioNetHdr;

typedef struct {
    VirtioDesc *Desc;
    VirtioAvail *Avail;
    VirtioUsed *Used;
    UINT8 *RxBuffers[VIRTIO_QUEUE_SIZE];
    UINT16 FreeHead;
    UINT16 NumFree;
    UINT16 LastUsedIdx;
} VirtioNetQueue;

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
    VirtioNetQueue Rx;
    VirtioNetQueue Tx;
    UINT8 *TxBuf;
    NetworkDevice NetDev;
} VirtioNetDev;

static VirtioNetDev GVirtioNetDevs[VIRTIO_MAX_NET_DEVS];
static UINT32 GVirtioNetCount = 0;

static inline UINT8 VirtioLegacyIn8(VirtioNetDev *Dev, UINT16 Off) {
    return Inb(Dev->IoBase + Off);
}

static inline NOPTR VirtioLegacyOut8(VirtioNetDev *Dev, UINT16 Off, UINT8 Val) {
    Outb(Dev->IoBase + Off, Val);
}

static inline UINT16 VirtioLegacyIn16(VirtioNetDev *Dev, UINT16 Off) {
    return Inw(Dev->IoBase + Off);
}

static inline NOPTR VirtioLegacyOut16(VirtioNetDev *Dev, UINT16 Off, UINT16 Val) {
    Outw(Dev->IoBase + Off, Val);
}

static inline UINT32 VirtioLegacyIn32(VirtioNetDev *Dev, UINT16 Off) {
    return Inl(Dev->IoBase + Off);
}

static inline NOPTR VirtioLegacyOut32(VirtioNetDev *Dev, UINT16 Off, UINT32 Val) {
    Outl(Dev->IoBase + Off, Val);
}

static BOOL VirtioWalkCaps(PciDevice *Pci, VirtioNetDev *VDev) {
    UINT8 Cap = PciFindCap(Pci, VIRTIO_PCI_CAP_VNDR);

    while (Cap) {
        UINT32 CapHdr = PciRead(Pci, Cap);
        UINT8 Type = (CapHdr >> 24) & 0xFF;
        UINT8 Bar = (PciRead(Pci, Cap + 4) & 0xFF);
        UINT32 Offset = PciRead(Pci, Cap + 8);
        UINT64 BarAddr = Pci->Bars[Bar] & ~0xFULL;

        if (Type == VIRTIO_PCI_CAP_COMMON_CFG) {
            VDev->Common = (volatile VirtioPciCommonCfg *)(UINTPTR)(BarAddr + Offset);
        } else if (Type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
            VDev->Notify = (volatile UINT8 *)(UINTPTR)BarAddr;
            VDev->NotifyOffset = Offset;
            VDev->NotifyMultiplier = (PciRead(Pci, Cap + 16) & 0xFFFF);
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

static NOPTR VirtioSetStatus(VirtioNetDev *Dev, UINT8 Status) {
    if (Dev->Legacy) {
        VirtioLegacyOut8(Dev, VIRTIO_LEGACY_REG_STATUS, Status);
    } else {
        MmioWrite8((volatile NOPTR *)&Dev->Common->DeviceStatus, Status);
    }
}

static UINT8 VirtioGetStatus(VirtioNetDev *Dev) {
    if (Dev->Legacy) {
        return VirtioLegacyIn8(Dev, VIRTIO_LEGACY_REG_STATUS);
    }
    return MmioRead8((volatile NOPTR *)&Dev->Common->DeviceStatus);
}

static NOPTR VirtioReset(VirtioNetDev *Dev) {
    if (Dev->Legacy) {
        VirtioLegacyOut8(Dev, VIRTIO_LEGACY_REG_STATUS, 0);
    } else {
        MmioWrite8((volatile NOPTR *)&Dev->Common->DeviceStatus, 0);
    }
    TimerUdelay(1000);
}

static NOPTR VirtioNotifyQueue(VirtioNetDev *Dev, UINT16 QueueId) {
    if (Dev->Legacy) {
        VirtioLegacyOut16(Dev, VIRTIO_LEGACY_REG_QSELECT, QueueId);
        VirtioLegacyOut16(Dev, VIRTIO_LEGACY_REG_QNOTIFY, QueueId);
    } else if (Dev->Notify) {
        volatile UINT16 *NotifyReg = (volatile UINT16 *)(Dev->Notify + Dev->NotifyOffset +
            (UINT64)MmioRead16((volatile NOPTR *)&Dev->Common->QueueNotifyOff) *
            Dev->NotifyMultiplier);
        *NotifyReg = QueueId;
    }
}

static BOOL VirtioSetupQueue(VirtioNetDev *Dev, VirtioNetQueue *Queue, UINT16 QueueId) {
    UINT64 DescPhys;
    UINT64 AvailPhys;
    UINT64 UsedPhys;
    UINT64 QueuePhys;

    Queue->Desc = (VirtioDesc *)MemoryAllocate(PAGE_SIZE * 3);
    if (!Queue->Desc) {
        return FALSE;
    }

    Queue->Avail = (VirtioAvail *)((UINT8 *)Queue->Desc + PAGE_SIZE);
    Queue->Used = (VirtioUsed *)((UINT8 *)Queue->Avail + PAGE_SIZE);
    MemSet(Queue->Desc, 0, PAGE_SIZE * 3);
    MemSet(Queue->Avail, 0, PAGE_SIZE);
    MemSet(Queue->Used, 0, PAGE_SIZE);

    Queue->FreeHead = 0;
    Queue->NumFree = VIRTIO_QUEUE_SIZE;
    Queue->LastUsedIdx = 0;

    for (UINT16 I = 0; I < VIRTIO_QUEUE_SIZE; I++) {
        Queue->Desc[I].Next = (I + 1 < VIRTIO_QUEUE_SIZE) ? (I + 1) : 0;
    }

    DescPhys = VirtToPhys((UINT64)(UINTPTR)Queue->Desc);
    AvailPhys = VirtToPhys((UINT64)(UINTPTR)Queue->Avail);
    UsedPhys = VirtToPhys((UINT64)(UINTPTR)Queue->Used);
    QueuePhys = DescPhys;

    if (Dev->Legacy) {
        VirtioLegacyOut16(Dev, VIRTIO_LEGACY_REG_QSELECT, QueueId);
        VirtioLegacyOut16(Dev, VIRTIO_LEGACY_REG_QSIZE, VIRTIO_QUEUE_SIZE);
        VirtioLegacyOut32(Dev, VIRTIO_LEGACY_REG_QADDR, (UINT32)(QueuePhys >> 12));
    } else {
        MmioWrite16((volatile NOPTR *)&Dev->Common->QueueSelect, QueueId);
        MmioWrite16((volatile NOPTR *)&Dev->Common->QueueSize, VIRTIO_QUEUE_SIZE);
        MmioWrite64((volatile NOPTR *)&Dev->Common->QueueDesc, DescPhys);
        MmioWrite64((volatile NOPTR *)&Dev->Common->QueueDriver, AvailPhys);
        MmioWrite64((volatile NOPTR *)&Dev->Common->QueueDevice, UsedPhys);
        MmioWrite16((volatile NOPTR *)&Dev->Common->QueueEnable, 1);
    }

    (void)AvailPhys;
    (void)UsedPhys;
    return TRUE;
}

static BOOL VirtioNetRefillRx(VirtioNetDev *Dev, VirtioNetQueue *Rx, UINT16 DescIdx) {
    UINT8 *Buf;

    Buf = (UINT8 *)MemoryAllocate(VIRTIO_NET_RX_BUF_SIZE);
    if (!Buf) {
        return FALSE;
    }

    Rx->RxBuffers[DescIdx] = Buf;
    Rx->Desc[DescIdx].Addr = VirtToPhys((UINT64)(UINTPTR)Buf);
    Rx->Desc[DescIdx].Len = VIRTIO_NET_RX_BUF_SIZE;
    Rx->Desc[DescIdx].Flags = 2;
    Rx->Desc[DescIdx].Next = 0;

    Rx->Avail->Ring[Rx->Avail->Idx % VIRTIO_QUEUE_SIZE] = DescIdx;
    Rx->Avail->Idx++;
    return TRUE;
}

static NOPTR VirtioNetFillRxQueue(VirtioNetDev *Dev) {
    VirtioNetQueue *Rx = &Dev->Rx;

    for (UINT16 I = 0; I < VIRTIO_QUEUE_SIZE; I++) {
        if (!VirtioNetRefillRx(Dev, Rx, I)) {
            break;
        }
    }
    VirtioNotifyQueue(Dev, VIRTIO_NET_QUEUE_RX);
}

static BOOL VirtioNetNegotiate(VirtioNetDev *Dev) {
    UINT32 Features = 0;

    VirtioReset(Dev);
    VirtioSetStatus(Dev, VIRTIO_STATUS_ACK);
    VirtioSetStatus(Dev, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    if (Dev->Legacy) {
        Features = VirtioLegacyIn32(Dev, VIRTIO_LEGACY_REG_FEATURES);
        VirtioLegacyOut32(Dev, VIRTIO_LEGACY_REG_GFEATURES, Features & VIRTIO_NET_F_MAC);
    } else {
        MmioWrite32((volatile NOPTR *)&Dev->Common->GuestFeatureSelect, 0);
        MmioWrite32((volatile NOPTR *)&Dev->Common->GuestFeature, VIRTIO_NET_F_MAC);
    }

    VirtioSetStatus(Dev, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    if (!(VirtioGetStatus(Dev) & VIRTIO_STATUS_FEATURES_OK)) {
        VirtioSetStatus(Dev, VIRTIO_STATUS_FAILED);
        return FALSE;
    }

    if (!VirtioSetupQueue(Dev, &Dev->Rx, VIRTIO_NET_QUEUE_RX)) {
        VirtioSetStatus(Dev, VIRTIO_STATUS_FAILED);
        return FALSE;
    }
    if (!VirtioSetupQueue(Dev, &Dev->Tx, VIRTIO_NET_QUEUE_TX)) {
        VirtioSetStatus(Dev, VIRTIO_STATUS_FAILED);
        return FALSE;
    }

    Dev->TxBuf = (UINT8 *)MemoryAllocate(VIRTIO_NET_RX_BUF_SIZE);
    if (!Dev->TxBuf) {
        VirtioSetStatus(Dev, VIRTIO_STATUS_FAILED);
        return FALSE;
    }

    VirtioSetStatus(Dev, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                          VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    VirtioNetFillRxQueue(Dev);
    return TRUE;
}

static NOPTR VirtioNetReadMac(VirtioNetDev *Dev) {
    if (Dev->DeviceCfg) {
        for (INT I = 0; I < 6; I++) {
            Dev->NetDev.MacAddr[I] = MmioRead8((volatile NOPTR *)(Dev->DeviceCfg + I));
        }
    } else if (Dev->Legacy) {
        for (INT I = 0; I < 6; I++) {
            Dev->NetDev.MacAddr[I] = Inb(Dev->IoBase + 0x14 + I);
        }
    }
}

static INT VirtioNetOpen(NetworkDevice *NetDev) {
    (NOPTR)NetDev;
    return SUCCESS;
}

static INT VirtioNetStop(NetworkDevice *NetDev) {
    (NOPTR)NetDev;
    return SUCCESS;
}

static INT VirtioNetXmit(NetworkBuf *Buf, NetworkDevice *NetDev) {
    VirtioNetDev *Dev = (VirtioNetDev *)NetDev->Priv;
    VirtioNetQueue *Tx = &Dev->Tx;
    VirtioNetHdr *Hdr;
    UINT32 TotalLen;
    UINT16 LastUsed;
    UINT32 Spin;

    if (!Buf || Buf->Len == 0) {
        return INCORRECT_VALUE;
    }

    if (Buf->Len + VIRTIO_NET_HDR_LEN > VIRTIO_NET_RX_BUF_SIZE) {
        NetDev->Stats.TxDropped++;
        NetworkFreeBuf(Buf);
        return INCORRECT_VALUE;
    }

    Hdr = (VirtioNetHdr *)Dev->TxBuf;
    MemSet(Hdr, 0, sizeof(VirtioNetHdr));
    MemCpy(Dev->TxBuf + VIRTIO_NET_HDR_LEN, Buf->Data, Buf->Len);
    TotalLen = VIRTIO_NET_HDR_LEN + Buf->Len;
    NetworkFreeBuf(Buf);

    Tx->Desc[0].Addr = VirtToPhys((UINT64)(UINTPTR)Dev->TxBuf);
    Tx->Desc[0].Len = TotalLen;
    Tx->Desc[0].Flags = 0;
    Tx->Desc[0].Next = 0;

    LastUsed = Tx->Used->Idx;
    Tx->Avail->Ring[Tx->Avail->Idx % VIRTIO_QUEUE_SIZE] = 0;
    Tx->Avail->Idx++;

    VirtioNotifyQueue(Dev, VIRTIO_NET_QUEUE_TX);

    Spin = 0;
    while (Tx->Used->Idx == LastUsed) {
        if (++Spin > 5000000) {
            NetDev->Stats.TxErrors++;
            return DEVICE_ERROR;
        }
        if (Dev->Isr) {
            (void)*Dev->Isr;
        }
        CpuPause();
    }

    return SUCCESS;
}

static INT VirtioNetPoll(NetworkDevice *NetDev) {
    VirtioNetDev *Dev = (VirtioNetDev *)NetDev->Priv;
    VirtioNetQueue *Rx = &Dev->Rx;
    UINT16 UsedIdx = Rx->LastUsedIdx;

    while (UsedIdx != Rx->Used->Idx) {
        VirtioUsedElem *Elem = &Rx->Used->Ring[UsedIdx % VIRTIO_QUEUE_SIZE];
        UINT8 *Buf = Rx->RxBuffers[Elem->Id];
        UINT32 PacketLen;
        NetworkBuf *NetBuf;

        if (Buf && Elem->Len > VIRTIO_NET_HDR_LEN) {
            PacketLen = Elem->Len - VIRTIO_NET_HDR_LEN;
            NetBuf = NetworkAllocBuf(PacketLen);
            if (NetBuf) {
                MemCpy(NetBuf->Data, Buf + VIRTIO_NET_HDR_LEN, PacketLen);
                NetBuf->Len = PacketLen;
                NetworkRx(NetBuf, NetDev);
            }
        }

        if (Buf) {
            MemoryFree(Buf);
            Rx->RxBuffers[Elem->Id] = NULLPTR;
        }

        VirtioNetRefillRx(Dev, Rx, (UINT16)Elem->Id);
        UsedIdx++;
    }

    Rx->LastUsedIdx = UsedIdx;
    return SUCCESS;
}

static INT VirtioNetAttach(VirtioNetDev *Dev, UINT32 Index) {
    NetworkDevice *Net = &Dev->NetDev;

    MemSet(Net, 0, sizeof(*Net));
    SnPrintf(Net->Name, sizeof(Net->Name), "eth%u", Index);
    Net->Type = NETWORK_TYPE_ETHERNET;
    Net->Mtu = NETWORK_MTU;
    Net->Up = TRUE;
    Net->Running = TRUE;
    Net->Open = VirtioNetOpen;
    Net->Stop = VirtioNetStop;
    Net->Xmit = VirtioNetXmit;
    Net->Poll = VirtioNetPoll;
    Net->Priv = Dev;

    if (!VirtioNetNegotiate(Dev)) {
        return DEVICE_ERROR;
    }

    VirtioNetReadMac(Dev);
    NetworkRegisterDevice(Net);

    ConsolePrint("[VirtIO-Net] %s: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 Net->Name,
                 Net->MacAddr[0], Net->MacAddr[1], Net->MacAddr[2],
                 Net->MacAddr[3], Net->MacAddr[4], Net->MacAddr[5]);
    return SUCCESS;
}

static INT VirtioNetProbe(PciDevice *Pci) {
    VirtioNetDev *Dev;
    UINT32 Index;

    if (!Pci || GVirtioNetCount >= VIRTIO_MAX_NET_DEVS) {
        return NO_OBJECT;
    }

    Index = GVirtioNetCount;
    Dev = &GVirtioNetDevs[Index];
    MemSet(Dev, 0, sizeof(*Dev));
    Dev->Pci = Pci;

    PciEnable(Pci);
    PciEnableBusmaster(Pci);

    if (Pci->DeviceId == VIRTIO_DEV_NET_LEGACY && (Pci->Bars[0] & 1)) {
        Dev->Legacy = TRUE;
        Dev->IoBase = (UINT16)(Pci->Bars[0] & ~3);
    } else if (Pci->DeviceId == VIRTIO_DEV_NET_MODERN || Pci->DeviceId == VIRTIO_DEV_NET_LEGACY) {
        Dev->Legacy = FALSE;
        if (!VirtioWalkCaps(Pci, Dev)) {
            return NOT_FOUND;
        }
    } else {
        return NOT_FOUND;
    }

    if (IsError(VirtioNetAttach(Dev, Index)).IsError) {
        return DEVICE_ERROR;
    }

    GVirtioNetCount++;
    KDriverRegister(KDriverGenerateStruct("VirtIO-Net", DCL1, TRUE, Dev, NULLPTR));
    return SUCCESS;
}

INT VirtioNetInit(NOPTR) {
    PciDevice *Dev;

    Dev = PciGetFirst();
    while (Dev) {
        if (Dev->VendorId == VIRTIO_VENDOR_ID &&
            (Dev->DeviceId == VIRTIO_DEV_NET_LEGACY || Dev->DeviceId == VIRTIO_DEV_NET_MODERN)) {
            VirtioNetProbe(Dev);
        }
        Dev = PciGetNext(Dev);
    }

    ConsolePrint("[VirtIO-Net] interfaces: %u\n", GVirtioNetCount);
    RETURN(SUCCESS);
}
