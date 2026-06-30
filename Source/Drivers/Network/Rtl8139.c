#include <Network/Rtl8139.h>
#include <Network/Network.h>
#include <Pci.h>
#include <Memory/Allocator.h>
#include <Kernel/Paging.h>
#include <Kernel/Return.h>
#include <Kernel/KDriver.h>
#include <Asm/Io.h>
#include <Asm/Cpu.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Console.h>
#include <Time/Timer.h>

#define RTL8139_VENDOR          0x10EC
#define RTL8139_DEVICE          0x8139
#define RTL8139_MAX_DEVS        4
#define RTL8139_RX_BUF_SIZE     (8192 + 16)
#define RTL8139_TX_COUNT        4

#define RTL8139_IDR0            0x00
#define RTL8139_RBSTART         0x30
#define RTL8139_CMD             0x37
#define RTL8139_CAPR            0x38
#define RTL8139_CBR             0x3A
#define RTL8139_IMR             0x3C
#define RTL8139_ISR             0x3E
#define RTL8139_TCR             0x40
#define RTL8139_RCR             0x44
#define RTL8139_CONFIG1         0x52

#define RTL8139_CMD_RESET       0x10
#define RTL8139_CMD_RX_ENABLE   0x08
#define RTL8139_CMD_TX_ENABLE   0x04
#define RTL8139_RCR_AAP         (1 << 0)
#define RTL8139_RCR_APM         (1 << 1)
#define RTL8139_RCR_AM          (1 << 2)
#define RTL8139_RCR_AB          (1 << 3)
#define RTL8139_RCR_WRAP        (1 << 7)

typedef struct {
    PciDevice *Pci;
    UINT16 IoBase;
    UINT8 *RxBuffer;
    UINT8 *TxBuffers[RTL8139_TX_COUNT];
    UINT32 RxOffset;
    NetworkDevice NetDev;
} Rtl8139Dev;

static Rtl8139Dev GRtl8139Devs[RTL8139_MAX_DEVS];
static UINT32 GRtl8139Count = 0;

static inline UINT8 RtlIn8(Rtl8139Dev *Dev, UINT16 Reg) {
    return Inb(Dev->IoBase + Reg);
}

static inline UINT16 RtlIn16(Rtl8139Dev *Dev, UINT16 Reg) {
    return Inw(Dev->IoBase + Reg);
}

static inline UINT32 RtlIn32(Rtl8139Dev *Dev, UINT16 Reg) {
    return Inl(Dev->IoBase + Reg);
}

static inline NOPTR RtlOut8(Rtl8139Dev *Dev, UINT16 Reg, UINT8 Val) {
    Outb(Dev->IoBase + Reg, Val);
}

static inline NOPTR RtlOut16(Rtl8139Dev *Dev, UINT16 Reg, UINT16 Val) {
    Outw(Dev->IoBase + Reg, Val);
}

static inline NOPTR RtlOut32(Rtl8139Dev *Dev, UINT16 Reg, UINT32 Val) {
    Outl(Dev->IoBase + Reg, Val);
}

static NOPTR Rtl8139Reset(Rtl8139Dev *Dev) {
    RtlOut8(Dev, RTL8139_CMD, RTL8139_CMD_RESET);
    TimerMdelay(10);
    while (RtlIn8(Dev, RTL8139_CMD) & RTL8139_CMD_RESET) {
        TimerUdelay(100);
    }
}

static NOPTR Rtl8139ReadMac(Rtl8139Dev *Dev) {
    INT I;

    for (I = 0; I < 6; I++) {
        Dev->NetDev.MacAddr[I] = RtlIn8(Dev, RTL8139_IDR0 + I);
    }
}

static INT Rtl8139Open(NetworkDevice *NetDev) {
    (NOPTR)NetDev;
    return SUCCESS;
}

static INT Rtl8139Stop(NetworkDevice *NetDev) {
    (NOPTR)NetDev;
    return SUCCESS;
}

static INT Rtl8139Xmit(NetworkBuf *Buf, NetworkDevice *NetDev) {
    Rtl8139Dev *Dev = (Rtl8139Dev *)NetDev->Priv;
    static UINT8 TxIdx = 0;
    UINT32 Phys;
    UINT32 Spin;

    UINT32 PacketLen = Buf->Len;

    if (!Buf || PacketLen == 0 || PacketLen > NETWORK_MTU) {
        return INCORRECT_VALUE;
    }

    MemCpy(Dev->TxBuffers[TxIdx], Buf->Data, PacketLen);
    Phys = (UINT32)VirtToPhys((UINT64)(UINTPTR)Dev->TxBuffers[TxIdx]);
    NetworkFreeBuf(Buf);

    RtlOut32(Dev, 0x20 + TxIdx * 4, Phys);
    RtlOut32(Dev, 0x10 + TxIdx * 4, PacketLen);
    RtlOut8(Dev, RTL8139_CMD, RTL8139_CMD_TX_ENABLE | RTL8139_CMD_RX_ENABLE);

    Spin = 0;
    while (RtlIn32(Dev, 0x10 + TxIdx * 4) & (1U << 31)) {
        if (++Spin > 5000000) {
            NetDev->Stats.TxErrors++;
            return DEVICE_ERROR;
        }
        CpuPause();
    }

    TxIdx = (UINT8)((TxIdx + 1) % RTL8139_TX_COUNT);
    NetDev->Stats.TxPackets++;
    return SUCCESS;
}

static INT Rtl8139Poll(NetworkDevice *NetDev) {
    Rtl8139Dev *Dev = (Rtl8139Dev *)NetDev->Priv;
    UINT16 Cbuf = RtlIn16(Dev, RTL8139_CBR) % RTL8139_RX_BUF_SIZE;

    while (Dev->RxOffset != Cbuf) {
        UINT32 Offset = Dev->RxOffset % RTL8139_RX_BUF_SIZE;
        UINT16 *Header = (UINT16 *)(Dev->RxBuffer + Offset);
        UINT16 Status = Header[0];
        UINT16 Length = Header[1];
        NetworkBuf *NetBuf;

        if (!(Status & 0x01) || Length < 4) {
            break;
        }
        Length -= 4;

        NetBuf = NetworkAllocBuf(Length);
        if (NetBuf) {
            UINT32 DataOff = (Offset + 4) % RTL8139_RX_BUF_SIZE;
            if (DataOff + Length <= RTL8139_RX_BUF_SIZE) {
                MemCpy(NetBuf->Data, Dev->RxBuffer + DataOff, Length);
            } else {
                UINT32 First = RTL8139_RX_BUF_SIZE - DataOff;
                MemCpy(NetBuf->Data, Dev->RxBuffer + DataOff, First);
                MemCpy(NetBuf->Data + First, Dev->RxBuffer, Length - First);
            }
            NetBuf->Len = Length;
            NetworkRx(NetBuf, NetDev);
        }

        Dev->RxOffset = (Dev->RxOffset + Length + 4 + 3) & ~3;
        Dev->RxOffset %= RTL8139_RX_BUF_SIZE;
        RtlOut16(Dev, RTL8139_CAPR, (UINT16)(Dev->RxOffset - 16));
    }

    RtlOut16(Dev, RTL8139_ISR, 0x0005);
    return SUCCESS;
}

static INT Rtl8139Attach(Rtl8139Dev *Dev, UINT32 Index) {
    NetworkDevice *Net = &Dev->NetDev;
    UINT32 RxPhys;
    INT I;

    MemSet(Net, 0, sizeof(*Net));
    SnPrintf(Net->Name, sizeof(Net->Name), "rtl%d", Index);
    Net->Type = NETWORK_TYPE_ETHERNET;
    Net->Mtu = NETWORK_MTU;
    Net->Up = TRUE;
    Net->Running = TRUE;
    Net->Open = Rtl8139Open;
    Net->Stop = Rtl8139Stop;
    Net->Xmit = Rtl8139Xmit;
    Net->Poll = Rtl8139Poll;
    Net->Priv = Dev;

    Dev->RxBuffer = (UINT8 *)MemoryAllocate(RTL8139_RX_BUF_SIZE + 16);
    if (!Dev->RxBuffer) {
        return NO_MEMORY;
    }
    for (I = 0; I < RTL8139_TX_COUNT; I++) {
        Dev->TxBuffers[I] = (UINT8 *)MemoryAllocate(NETWORK_BUFFER_SIZE);
        if (!Dev->TxBuffers[I]) {
            return NO_MEMORY;
        }
    }

    Rtl8139Reset(Dev);
    RxPhys = (UINT32)VirtToPhys((UINT64)(UINTPTR)Dev->RxBuffer);
    RtlOut32(Dev, RTL8139_RBSTART, RxPhys);
    RtlOut8(Dev, RTL8139_CONFIG1, 0x00);
    RtlOut32(Dev, RTL8139_RCR, RTL8139_RCR_AAP | RTL8139_RCR_APM | RTL8139_RCR_AM |
                               RTL8139_RCR_AB | RTL8139_RCR_WRAP | (1 << 18));
    RtlOut32(Dev, RTL8139_TCR, (1 << 18) | (1 << 19));
    RtlOut16(Dev, RTL8139_IMR, 0x0005);
    RtlOut8(Dev, RTL8139_CMD, RTL8139_CMD_RX_ENABLE | RTL8139_CMD_TX_ENABLE);
    Dev->RxOffset = 0;

    Rtl8139ReadMac(Dev);
    NetworkRegisterDevice(Net);
    ConsolePrint("[RTL8139] %s: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 Net->Name,
                 Net->MacAddr[0], Net->MacAddr[1], Net->MacAddr[2],
                 Net->MacAddr[3], Net->MacAddr[4], Net->MacAddr[5]);
    return SUCCESS;
}

static INT Rtl8139Probe(PciDevice *Pci) {
    Rtl8139Dev *Dev;
    UINT32 Index;

    if (!Pci || GRtl8139Count >= RTL8139_MAX_DEVS) {
        return NO_OBJECT;
    }
    if (Pci->VendorId != RTL8139_VENDOR || Pci->DeviceId != RTL8139_DEVICE) {
        return NOT_FOUND;
    }

    Index = GRtl8139Count;
    Dev = &GRtl8139Devs[Index];
    MemSet(Dev, 0, sizeof(*Dev));
    Dev->Pci = Pci;

    PciEnable(Pci);
    PciEnableBusmaster(Pci);

    Dev->IoBase = (UINT16)(Pci->Bars[0] & ~3);
    if (!Dev->IoBase) {
        return NO_OBJECT;
    }

    if (IsError(Rtl8139Attach(Dev, Index)).IsError) {
        return DEVICE_ERROR;
    }

    GRtl8139Count++;
    KDriverRegister(KDriverGenerateStruct("RTL8139", DCL1, TRUE, Dev, NULLPTR));
    return SUCCESS;
}

INT Rtl8139Init(NOPTR) {
    PciDevice *Dev;

    Dev = PciGetFirst();
    while (Dev) {
        Rtl8139Probe(Dev);
        Dev = PciGetNext(Dev);
    }

    ConsolePrint("[RTL8139] interfaces: %u\n", GRtl8139Count);
    RETURN(SUCCESS);
}
