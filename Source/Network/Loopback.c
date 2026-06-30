#include <Network/Loopback.h>
#include <Network/Network.h>
#include <Network/IpV4.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Kernel/Return.h>

static NetworkDevice GLoopbackDev;
static BOOL GLoopbackInitialized = FALSE;

static INT LoopbackOpen(NetworkDevice *Dev) {
    (NOPTR)Dev;
    return SUCCESS;
}

static INT LoopbackStop(NetworkDevice *Dev) {
    (NOPTR)Dev;
    return SUCCESS;
}

static INT LoopbackXmit(NetworkBuf *Buf, NetworkDevice *Dev) {
    NetworkBuf *RxBuf;
    UINT32 CopyLen;

    (NOPTR)Dev;

    if (!Buf || Buf->Len == 0) {
        return INCORRECT_VALUE;
    }

    CopyLen = Buf->Len;
    if (CopyLen > NETWORK_MTU) {
        CopyLen = NETWORK_MTU;
    }

    RxBuf = NetworkAllocBuf(CopyLen);
    if (!RxBuf) {
        Dev->Stats.TxErrors++;
        NetworkFreeBuf(Buf);
        return NO_MEMORY;
    }

    MemCpy(RxBuf->Data, Buf->Data, CopyLen);
    RxBuf->Len = CopyLen;
    NetworkFreeBuf(Buf);

    NetworkRx(RxBuf, &GLoopbackDev);
    return SUCCESS;
}

INT LoopbackInit(NOPTR) {
    IpV4Addr LoopAddr;
    IpV4Addr LoopMask;

    if (GLoopbackInitialized) {
        RETURN(SUCCESS);
    }

    MemSet(&GLoopbackDev, 0, sizeof(GLoopbackDev));
    StrCpy(GLoopbackDev.Name, "lo");
    GLoopbackDev.Type = NETWORK_TYPE_LOOPBACK;
    GLoopbackDev.Mtu = NETWORK_MTU;
    GLoopbackDev.Up = TRUE;
    GLoopbackDev.Running = TRUE;
    GLoopbackDev.Open = LoopbackOpen;
    GLoopbackDev.Stop = LoopbackStop;
    GLoopbackDev.Xmit = LoopbackXmit;

    GLoopbackDev.MacAddr[0] = 0x00;
    GLoopbackDev.MacAddr[1] = 0x00;
    GLoopbackDev.MacAddr[2] = 0x00;
    GLoopbackDev.MacAddr[3] = 0x00;
    GLoopbackDev.MacAddr[4] = 0x00;
    GLoopbackDev.MacAddr[5] = 0x01;

    LoopAddr = IpV4PTon("127.0.0.1");
    LoopMask = IpV4PTon("255.0.0.0");
    MemCpy(GLoopbackDev.IpAddr, &LoopAddr.Addr, 4);
    MemCpy(GLoopbackDev.Netmask, &LoopMask.Addr, 4);

    NetworkRegisterDevice(&GLoopbackDev);

    IpV4RouteAdd(IpV4PTon("127.0.0.0"), LoopMask, (IpV4Addr){0},
                 LoopAddr, &GLoopbackDev, 0);

    GLoopbackInitialized = TRUE;
    RETURN(SUCCESS);
}
