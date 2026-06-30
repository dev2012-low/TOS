#include <Network/Udp.h>
#include <Network/IpV4.h>
#include <Lib/String.h>
#include <Memory/Allocator.h>
#include <Kernel/Return.h>
#include <Console.h>

/*
 * ============================================================================
 * Global variables
 * ============================================================================
 */

static UdpHandler GUdpHandlers[65536];
static BOOL GUdpInitialized = FALSE;

static UINT16 UdpChecksumInternal(UdpHeader *Udp, UINT32 Len, UINT32 Src, UINT32 Dst) {
    struct {
        UINT32 Src;
        UINT32 Dst;
        UINT8 Zero;
        UINT8 Proto;
        UINT16 UdpLen;
    } ATTRIBUTE(packed) Pseudo = {
        .Src = Src,
        .Dst = Dst,
        .Zero = 0,
        .Proto = IPV4_PROTO_UDP,
        .UdpLen = Htons((UINT16)Len),
    };

    UINT32 Sum = 0;
    UINT16 *Ptr;
    UINT32 I;

    Ptr = (UINT16 *)&Pseudo;
    for (I = 0; I < sizeof(Pseudo) / 2; I++) {
        Sum += Ptr[I];
    }

    Ptr = (UINT16 *)Udp;
    for (I = 0; I < Len / 2; I++) {
        Sum += Ptr[I];
    }
    if (Len & 1) {
        Sum += *(UINT8 *)((UINT8 *)Udp + Len - 1);
    }

    while (Sum >> 16) {
        Sum = (Sum & 0xFFFF) + (Sum >> 16);
    }

    return (UINT16)~Sum;
}

static BOOL UdpVerifyChecksum(UdpHeader *Udp, UINT32 Len, IpV4Addr Src, IpV4Addr Dst) {
    UINT16 Received = Udp->Checksum;

    if (Received == 0) {
        return TRUE;
    }

    Udp->Checksum = 0;
    if (UdpChecksumInternal(Udp, Len, Src.Addr, Dst.Addr) != Received) {
        Udp->Checksum = Received;
        return FALSE;
    }
    Udp->Checksum = Received;
    return TRUE;
}

/*
 * ============================================================================
 * Registration of handlers
 * ============================================================================
 */

INT UdpRegisterHandler(UINT16 Port, UdpHandler Handler) {
    if (Port >= 65536) RETURN(INCORRECT_VALUE);
    if (GUdpHandlers[Port]) RETURN(ALREADY_EXISTS);

    GUdpHandlers[Port] = Handler;
    RETURN(SUCCESS);
}

NOPTR UdpUnregisterHandler(UINT16 Port) {
    if (Port < 65536) {
        GUdpHandlers[Port] = NULLPTR;
    }
}

static NOPTR UdpInputDispatch(NetworkBuf *Buf, NetworkDevice *Dev) {
    IpV4Addr Src = { .Addr = Buf->IpSrc };
    IpV4Addr Dst = { .Addr = Buf->IpDst };

    (NOPTR)Dev;
    UdpInput(Buf, Src, Dst);
}

/*
 * ============================================================================
 * Incoming UDP processing
 * ============================================================================
 */

INT UdpInput(NetworkBuf *Buf, IpV4Addr Src, IpV4Addr Dst) {
    UdpHeader *Udp;
    UINT16 SrcPort;
    UINT16 DstPort;
    UINT16 Len;
    UdpHandler Handler;

    if (Buf->Len < UDP_HEADER_SIZE) RETURN(INCORRECT_VALUE);

    Udp = (UdpHeader *)Buf->Data;
    SrcPort = Ntohs(Udp->SrcPort);
    DstPort = Ntohs(Udp->DstPort);
    Len = Ntohs(Udp->Length);

    if (Len > Buf->Len || Len < UDP_HEADER_SIZE) RETURN(INCORRECT_VALUE);

    if (!UdpVerifyChecksum(Udp, Len, Src, Dst)) {
        RETURN(INCORRECT_VALUE);
    }

    NetworkPull(Buf, UDP_HEADER_SIZE);

    if (DstPort < 65536 && GUdpHandlers[DstPort]) {
        GUdpHandlers[DstPort](Buf, Src, Dst, SrcPort, DstPort);
    }

    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Sending UDP packet
 * ============================================================================
 */

INT UdpOutput(NetworkBuf *Buf, IpV4Addr Dest, IpV4Addr Src,
              UINT16 DestPort, UINT16 SrcPort) {
    UdpHeader *Udp;
    UINT32 UdpLen;

    NetworkReserve(Buf, UDP_HEADER_SIZE);

    Udp = (UdpHeader *)Buf->Data;
    Udp->SrcPort = Htons(SrcPort);
    Udp->DstPort = Htons(DestPort);
    UdpLen = Buf->Len + UDP_HEADER_SIZE;
    Udp->Length = Htons((UINT16)UdpLen);
    Udp->Checksum = 0;

    Buf->Len = UdpLen;
    Udp->Checksum = UdpChecksumInternal(Udp, UdpLen, Src.Addr, Dest.Addr);
    if (Udp->Checksum == 0) {
        Udp->Checksum = 0xFFFF;
    }

    return IpV4Output(Buf, Dest, Src, IPV4_PROTO_UDP);
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

INT UdpInit(NOPTR) {
    if (GUdpInitialized) {
        RETURN(SUCCESS);
    }

    MemSet(GUdpHandlers, 0, sizeof(GUdpHandlers));
    IpV4RegisterProtocol(IPV4_PROTO_UDP, UdpInputDispatch);

    GUdpInitialized = TRUE;

    RETURN(SUCCESS);
}
