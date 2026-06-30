#include <Network/E1000.h>
#include <Network/Network.h>
#include <Pci.h>
#include <Memory/Allocator.h>
#include <Kernel/Paging.h>
#include <Kernel/Return.h>
#include <Kernel/KDriver.h>
#include <Asm/Mmio.h>
#include <Asm/Cpu.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Console.h>
#include <Time/Timer.h>

#define E1000_VENDOR_INTEL      0x8086
#define E1000_DEV_QEMU          0x100E
#define E1000_DEV_82574L        0x10D3
#define E1000_DEV_82540EM       0x100E
#define E1000_MAX_DEVS          4
#define E1000_RX_DESC_COUNT     32
#define E1000_TX_DESC_COUNT     8
#define E1000_RX_BUF_SIZE       (NETWORK_MTU + 16)

#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_EECD      0x0010
#define E1000_RCTL      0x0100
#define E1000_TCTL      0x0400
#define E1000_TIPG      0x0410
#define E1000_RDBAL     0x2800
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810
#define E1000_RDT       0x2818
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_RAH       0x5404
#define E1000_RAL       0x5400
#define E1000_MTA       0x5200

#define E1000_CTRL_RST  (1 << 26)
#define E1000_CTRL_SLU  (1 << 6)
#define E1000_CTRL_TE   (1 << 3)
#define E1000_CTRL_RE   (1 << 2)
#define E1000_RCTL_EN   (1 << 1)
#define E1000_TCTL_EN   (1 << 1)
#define E1000_TCTL_PSP  (1 << 3)

typedef struct {
    UINT64 Addr;
    UINT16 Length;
    UINT16 Checksum;
    UINT8 Status;
    UINT8 Errors;
    UINT16 Special;
} ATTRIBUTE(packed) E1000RxDesc;

typedef struct {
    UINT64 Addr;
    UINT16 Length;
    UINT8 Cso;
    UINT8 Cmd;
    UINT8 Status;
    UINT8 Css;
    UINT16 Special;
} ATTRIBUTE(packed) E1000TxDesc;

typedef struct {
    PciDevice *Pci;
    volatile UINT8 *Mmio;
    E1000RxDesc *RxDesc;
    E1000TxDesc *TxDesc;
    UINT8 *RxBuffers[E1000_RX_DESC_COUNT];
    UINT8 *TxBuffer;
    UINT16 RxTail;
    UINT16 TxTail;
    UINT16 TxHead;
    NetworkDevice NetDev;
} E1000Dev;

static E1000Dev GE1000Devs[E1000_MAX_DEVS];
static UINT32 GE1000Count = 0;

static inline UINT32 E1000Read32(E1000Dev *Dev, UINT32 Reg) {
    return MmioRead32((volatile NOPTR *)(Dev->Mmio + Reg));
}

static inline NOPTR E1000Write32(E1000Dev *Dev, UINT32 Reg, UINT32 Val) {
    MmioWrite32((volatile NOPTR *)(Dev->Mmio + Reg), Val);
}

static NOPTR E1000Reset(E1000Dev *Dev) {
    UINT32 Ctrl;
    INT I;

    E1000Write32(Dev, E1000_CTRL, E1000Read32(Dev, E1000_CTRL) | E1000_CTRL_RST);
    for (I = 0; I < 1000; I++) {
        if (!(E1000Read32(Dev, E1000_CTRL) & E1000_CTRL_RST)) {
            break;
        }
        TimerUdelay(10);
    }
    Ctrl = E1000Read32(Dev, E1000_CTRL);
    E1000Write32(Dev, E1000_CTRL, Ctrl | E1000_CTRL_SLU);
}

static BOOL E1000SetupRx(E1000Dev *Dev) {
    UINT64 DescPhys;
    UINT32 I;

    Dev->RxDesc = (E1000RxDesc *)MemoryAllocate(sizeof(E1000RxDesc) * E1000_RX_DESC_COUNT + 128);
    if (!Dev->RxDesc) {
        return FALSE;
    }
    MemSet(Dev->RxDesc, 0, sizeof(E1000RxDesc) * E1000_RX_DESC_COUNT + 128);

    for (I = 0; I < E1000_RX_DESC_COUNT; I++) {
        Dev->RxBuffers[I] = (UINT8 *)MemoryAllocate(E1000_RX_BUF_SIZE);
        if (!Dev->RxBuffers[I]) {
            return FALSE;
        }
        Dev->RxDesc[I].Addr = VirtToPhys((UINT64)(UINTPTR)Dev->RxBuffers[I]);
        Dev->RxDesc[I].Status = 0;
    }

    DescPhys = VirtToPhys((UINT64)(UINTPTR)Dev->RxDesc);
    E1000Write32(Dev, E1000_RDBAL, (UINT32)DescPhys);
    E1000Write32(Dev, E1000_RDBAH, (UINT32)(DescPhys >> 32));
    E1000Write32(Dev, E1000_RDLEN, sizeof(E1000RxDesc) * E1000_RX_DESC_COUNT);
    E1000Write32(Dev, E1000_RDH, 0);
    Dev->RxTail = E1000_RX_DESC_COUNT - 1;
    E1000Write32(Dev, E1000_RDT, Dev->RxTail);

    E1000Write32(Dev, E1000_RCTL, E1000_RCTL_EN | (1 << 4) | (1 << 15) | (1 << 25) | (3 << 16));
    E1000Write32(Dev, E1000_CTRL, E1000Read32(Dev, E1000_CTRL) | E1000_CTRL_TE | E1000_CTRL_RE);
    return TRUE;
}

static BOOL E1000SetupTx(E1000Dev *Dev) {
    UINT64 DescPhys;

    Dev->TxDesc = (E1000TxDesc *)MemoryAllocate(sizeof(E1000TxDesc) * E1000_TX_DESC_COUNT + 128);
    Dev->TxBuffer = (UINT8 *)MemoryAllocate(NETWORK_BUFFER_SIZE);
    if (!Dev->TxDesc || !Dev->TxBuffer) {
        return FALSE;
    }
    MemSet(Dev->TxDesc, 0, sizeof(E1000TxDesc) * E1000_TX_DESC_COUNT + 128);

    DescPhys = VirtToPhys((UINT64)(UINTPTR)Dev->TxDesc);
    E1000Write32(Dev, E1000_TDBAL, (UINT32)DescPhys);
    E1000Write32(Dev, E1000_TDBAH, (UINT32)(DescPhys >> 32));
    E1000Write32(Dev, E1000_TDLEN, sizeof(E1000TxDesc) * E1000_TX_DESC_COUNT);
    E1000Write32(Dev, E1000_TDH, 0);
    E1000Write32(Dev, E1000_TDT, 0);
    Dev->TxTail = 0;
    Dev->TxHead = 0;

    E1000Write32(Dev, E1000_TIPG, 0x0060200A);
    E1000Write32(Dev, E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (0x10 << 12) | (0x40 << 16));
    return TRUE;
}

static NOPTR E1000ReadMac(E1000Dev *Dev) {
    UINT32 Ral = E1000Read32(Dev, E1000_RAL);
    UINT32 Rah = E1000Read32(Dev, E1000_RAH);

    Dev->NetDev.MacAddr[0] = (UINT8)(Ral & 0xFF);
    Dev->NetDev.MacAddr[1] = (UINT8)((Ral >> 8) & 0xFF);
    Dev->NetDev.MacAddr[2] = (UINT8)((Ral >> 16) & 0xFF);
    Dev->NetDev.MacAddr[3] = (UINT8)((Ral >> 24) & 0xFF);
    Dev->NetDev.MacAddr[4] = (UINT8)(Rah & 0xFF);
    Dev->NetDev.MacAddr[5] = (UINT8)((Rah >> 8) & 0xFF);
}

static INT E1000Open(NetworkDevice *NetDev) {
    (NOPTR)NetDev;
    return SUCCESS;
}

static INT E1000Stop(NetworkDevice *NetDev) {
    (NOPTR)NetDev;
    return SUCCESS;
}

static INT E1000Xmit(NetworkBuf *Buf, NetworkDevice *NetDev) {
    E1000Dev *Dev = (E1000Dev *)NetDev->Priv;
    E1000TxDesc *Desc;
    UINT16 Idx;
    UINT32 Spin;

    if (!Buf || Buf->Len == 0 || Buf->Len > NETWORK_MTU) {
        return INCORRECT_VALUE;
    }

    Idx = Dev->TxTail;
    Desc = &Dev->TxDesc[Idx];
    MemCpy(Dev->TxBuffer, Buf->Data, Buf->Len);
    Desc->Addr = VirtToPhys((UINT64)(UINTPTR)Dev->TxBuffer);
    Desc->Length = (UINT16)Buf->Len;
    Desc->Cmd = (1 << 0) | (1 << 3);
    Desc->Status = 0;
    NetworkFreeBuf(Buf);

    Dev->TxTail = (UINT16)((Dev->TxTail + 1) % E1000_TX_DESC_COUNT);
    E1000Write32(Dev, E1000_TDT, Dev->TxTail);

    Spin = 0;
    while (!(Desc->Status & (1 << 0))) {
        if (++Spin > 5000000) {
            NetDev->Stats.TxErrors++;
            return DEVICE_ERROR;
        }
        CpuPause();
    }

    NetDev->Stats.TxPackets++;
    return SUCCESS;
}

static INT E1000Poll(NetworkDevice *NetDev) {
    E1000Dev *Dev = (E1000Dev *)NetDev->Priv;
    UINT16 Next = (UINT16)((Dev->RxTail + 1) % E1000_RX_DESC_COUNT);

    while (Dev->RxDesc[Next].Status & (1 << 0)) {
        E1000RxDesc *Desc = &Dev->RxDesc[Next];
        UINT8 *Buf = Dev->RxBuffers[Next];
        NetworkBuf *NetBuf;

        if (Desc->Length > 0) {
            NetBuf = NetworkAllocBuf(Desc->Length);
            if (NetBuf) {
                MemCpy(NetBuf->Data, Buf, Desc->Length);
                NetBuf->Len = Desc->Length;
                NetworkRx(NetBuf, NetDev);
            }
        }

        Desc->Status = 0;
        Dev->RxTail = Next;
        E1000Write32(Dev, E1000_RDT, Dev->RxTail);
        Next = (UINT16)((Dev->RxTail + 1) % E1000_RX_DESC_COUNT);
    }

    return SUCCESS;
}

static INT E1000Attach(E1000Dev *Dev, UINT32 Index) {
    NetworkDevice *Net = &Dev->NetDev;
    UINT32 I;

    MemSet(Net, 0, sizeof(*Net));
    SnPrintf(Net->Name, sizeof(Net->Name), "e1000-%u", Index);
    Net->Type = NETWORK_TYPE_ETHERNET;
    Net->Mtu = NETWORK_MTU;
    Net->Up = TRUE;
    Net->Running = TRUE;
    Net->Open = E1000Open;
    Net->Stop = E1000Stop;
    Net->Xmit = E1000Xmit;
    Net->Poll = E1000Poll;
    Net->Priv = Dev;

    E1000Reset(Dev);
    for (I = 0; I < 128; I++) {
        E1000Write32(Dev, E1000_MTA + I * 4, 0);
    }
    if (!E1000SetupRx(Dev) || !E1000SetupTx(Dev)) {
        return DEVICE_ERROR;
    }

    E1000ReadMac(Dev);
    NetworkRegisterDevice(Net);
    ConsolePrint("[E1000] %s: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 Net->Name,
                 Net->MacAddr[0], Net->MacAddr[1], Net->MacAddr[2],
                 Net->MacAddr[3], Net->MacAddr[4], Net->MacAddr[5]);
    return SUCCESS;
}

static INT E1000Probe(PciDevice *Pci) {
    E1000Dev *Dev;
    UINT32 Index;
    UINT32 MmioBase;

    if (!Pci || GE1000Count >= E1000_MAX_DEVS) {
        return NO_OBJECT;
    }
    if (Pci->VendorId != E1000_VENDOR_INTEL) {
        return NOT_FOUND;
    }
    if (Pci->DeviceId != E1000_DEV_QEMU && Pci->DeviceId != E1000_DEV_82574L) {
        return NOT_FOUND;
    }

    Index = GE1000Count;
    Dev = &GE1000Devs[Index];
    MemSet(Dev, 0, sizeof(*Dev));
    Dev->Pci = Pci;

    PciEnable(Pci);
    PciEnableBusmaster(Pci);

    MmioBase = Pci->Bars[0] & ~0xFULL;
    if (!MmioBase) {
        return NO_OBJECT;
    }
    Dev->Mmio = (volatile UINT8 *)(UINTPTR)MmioBase;

    if (IsError(E1000Attach(Dev, Index)).IsError) {
        return DEVICE_ERROR;
    }

    GE1000Count++;
    KDriverRegister(KDriverGenerateStruct("E1000", DCL1, TRUE, Dev, NULLPTR));
    return SUCCESS;
}

INT E1000Init(NOPTR) {
    PciDevice *Dev;

    Dev = PciGetFirst();
    while (Dev) {
        E1000Probe(Dev);
        Dev = PciGetNext(Dev);
    }

    ConsolePrint("[E1000] interfaces: %u\n", GE1000Count);
    RETURN(SUCCESS);
}
