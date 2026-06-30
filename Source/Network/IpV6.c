#include <Network/IpV6.h>
#include <Network/ICmp.h>
#include <Network/Network.h>
#include <Network/Ethernet.h>
#include <Lib/String.h>
#include <Memory/Allocator.h>
#include <Kernel/Return.h>
#include <Console.h>

/*
 * ============================================================================
 * Constant addresses
 * ============================================================================
 */

const IpV6Addr IPV6_ADDR_UNSPECIFIED = { .Octets = {0} };
const IpV6Addr IPV6_ADDR_LOOPBACK = { .Octets = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1} };
const IpV6Addr IPV6_ADDR_ALL_NODES = { .Octets = {0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,1} };
const IpV6Addr IPV6_ADDR_ALL_ROUTERS = { .Octets = {0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,2} };

static NetworkRxHandler GIpV6Protocols[256];
static UINT32 GIpV6HopLimit = 64;
static BOOL GIpV6Initialized = FALSE;

/*
 * ============================================================================
 * IPv6 addressing functions
 * ============================================================================
 */

static UINT8 HexToU8(CHAR C) {
    if (C >= '0' && C <= '9') return (UINT8)(C - '0');
    if (C >= 'a' && C <= 'f') return (UINT8)(C - 'a' + 10);
    if (C >= 'A' && C <= 'F') return (UINT8)(C - 'A' + 10);
    return 0;
}

IpV6Addr IpV6PTon(const CHAR *Str) {
    IpV6Addr Addr = { .Octets = {0} };
    UINT16 Words[8] = {0};
    INT WordIdx = 0;
    INT CompressIdx = -1;
    INT I = 0;
    
    if (!Str) return Addr;
    
    /* Skip :: at the beginning */
    if (Str[0] == ':' && Str[1] == ':') {
        CompressIdx = 0;
        I = 2;
    }
    
    while (Str[I] && WordIdx < 8) {
        if (Str[I] == ':') {
            I++;
            if (Str[I] == ':') {
                CompressIdx = WordIdx;
                I++;
                continue;
            }
            WordIdx++;
            continue;
        }
        
        UINT16 Val = 0;
        INT J;
        for (J = 0; J < 4 && Str[I] && Str[I] != ':'; J++, I++) {
            Val = (UINT16)((Val << 4) | HexToU8(Str[I]));
        }
        Words[WordIdx] = Val;
        if (Str[I] != ':') WordIdx++;
    }
    
    /* Expanding compression */
    if (CompressIdx >= 0) {
        INT NumWords = WordIdx;
        INT NumMissing = 8 - NumWords;
        for (INT J = 7; J >= CompressIdx + NumMissing; J--) {
            Words[J] = Words[J - NumMissing];
        }
        for (INT J = CompressIdx; J < CompressIdx + NumMissing; J++) {
            Words[J] = 0;
        }
    }
    
    /* Convert words to bytes (big-endian) */
    for (INT J = 0; J < 8; J++) {
        Addr.Octets[J * 2] = (UINT8)((Words[J] >> 8) & 0xFF);
        Addr.Octets[J * 2 + 1] = (UINT8)(Words[J] & 0xFF);
    }
    
    return Addr;
}

NOPTR IpV6NTop(IpV6Addr Addr, CHAR *Buf, UINT32 Size) {
    UINT16 Words[8];
    INT CompressStart = -1;
    INT CompressLen = 0;
    INT CurrentStart = -1;
    INT CurrentLen = 0;
    CHAR *Ptr;
    INT I;
    
    if (!Buf || Size == 0) return;
    
    /* Converting bytes to words (big-endian) */
    for (I = 0; I < 8; I++) {
        Words[I] = (UINT16)((Addr.Octets[I * 2] << 8) | Addr.Octets[I * 2 + 1]);
    }
    
    /* Finding the longest sequence of zeros to compress :: */
    for (I = 0; I < 8; I++) {
        if (Words[I] == 0) {
            if (CurrentStart == -1) {
                CurrentStart = I;
                CurrentLen = 1;
            } else {
                CurrentLen++;
            }
        } else {
            if (CurrentLen > CompressLen) {
                CompressLen = CurrentLen;
                CompressStart = CurrentStart;
            }
            CurrentStart = -1;
            CurrentLen = 0;
        }
    }
    if (CurrentLen > CompressLen) {
        CompressLen = CurrentLen;
        CompressStart = CurrentStart;
    }
    
    Ptr = Buf;
    for (I = 0; I < 8 && (USIZE)(Ptr - Buf) < Size - 1; I++) {
        if (CompressStart >= 0 && I == CompressStart) {
            if (Ptr != Buf) *Ptr++ = ':';
            *Ptr++ = ':';
            I += CompressLen - 1;
            continue;
        }
        
        if (I > 0) *Ptr++ = ':';
        
        /* Format the word in hex (without leading zeros) */
        if (Words[I] >= 0x1000) {
            *Ptr++ = "0123456789abcdef"[(Words[I] >> 12) & 0xF];
            *Ptr++ = "0123456789abcdef"[(Words[I] >> 8) & 0xF];
            *Ptr++ = "0123456789abcdef"[(Words[I] >> 4) & 0xF];
            *Ptr++ = "0123456789abcdef"[Words[I] & 0xF];
        } else if (Words[I] >= 0x100) {
            *Ptr++ = "0123456789abcdef"[(Words[I] >> 8) & 0xF];
            *Ptr++ = "0123456789abcdef"[(Words[I] >> 4) & 0xF];
            *Ptr++ = "0123456789abcdef"[Words[I] & 0xF];
        } else if (Words[I] >= 0x10) {
            *Ptr++ = "0123456789abcdef"[(Words[I] >> 4) & 0xF];
            *Ptr++ = "0123456789abcdef"[Words[I] & 0xF];
        } else {
            *Ptr++ = "0123456789abcdef"[Words[I] & 0xF];
        }
    }
    *Ptr = '\0';
}

BOOL IpV6AddrIsMulticast(IpV6Addr Addr) {
    return Addr.Octets[0] == 0xFF;
}

BOOL IpV6AddrIsLinkLocal(IpV6Addr Addr) {
    return (Addr.Octets[0] == 0xFE && (Addr.Octets[1] & 0xC0) == 0x80);
}

BOOL IpV6AddrIsLoopback(IpV6Addr Addr) {
    return MemCmp(&Addr, &IPV6_ADDR_LOOPBACK, 16) == 0;
}

BOOL IpV6AddrIsUnspecified(IpV6Addr Addr) {
    return MemCmp(&Addr, &IPV6_ADDR_UNSPECIFIED, 16) == 0;
}

/*
 * ============================================================================
 * IPv6 Checksum (pseudo-header for TCP/UDP/ICMPv6)
 * ============================================================================
 */

UINT16 IpV6PseudoChecksum(IpV6Addr *Src, IpV6Addr *Dst, UINT8 NextHeader, UINT32 PayloadLen) {
    UINT32 Sum = 0;
    UINT16 *Ptr;
    INT I;
    
    /* Source address (16 bytes) */
    Ptr = (UINT16*)Src->Octets;
    for (I = 0; I < 8; I++) {
        Sum += Ptr[I];
    }
    
    /* Destination address (16 bytes) */
    Ptr = (UINT16*)Dst->Octets;
    for (I = 0; I < 8; I++) {
        Sum += Ptr[I];
    }
    
    /* Upper layer packet length */
    Sum += (PayloadLen >> 16) + (PayloadLen & 0xFFFF);
    
    /* Next header + zero */
    Sum += NextHeader;
    
    /* Fold */
    while (Sum >> 16) {
        Sum = (Sum & 0xFFFF) + (Sum >> 16);
    }
    
    return ~Sum;
}

/*
 * ============================================================================
 * Sending an IPv6 packet
 * ============================================================================
 */

INT IpV6Output(NetworkBuf *Buf, IpV6Addr *Dest, IpV6Addr *Src, UINT8 NextHeader) {
    IpV6Header *Ip6H;
    NetworkDevice *Dev;
    UINT32 PayloadLen;
    
    if (!Buf || !Dest) RETURN(NO_OBJECT);

    Dev = NetworkGetFirstActive();
    if (!Dev) RETURN(NO_OBJECT);

    PayloadLen = Buf->Len;
    NetworkReserve(Buf, IPV6_HEADER_SIZE);
    
    Ip6H = (IpV6Header*)Buf->Data;
    MemSet(Ip6H, 0, sizeof(IpV6Header));
    
    Ip6H->Version = 6;
    Ip6H->TrafficClass = 0;
    Ip6H->FlowLabel = 0;
    Ip6H->PayloadLen = Htons((UINT16)PayloadLen);
    Ip6H->NextHeader = NextHeader;
    Ip6H->HopLimit = (UINT8)GIpV6HopLimit;
    
    if (Src) {
        MemCpy(Ip6H->SrcAddr, Src->Octets, 16);
    } else {
        MemCpy(Ip6H->SrcAddr, IPV6_ADDR_UNSPECIFIED.Octets, 16);
    }
    MemCpy(Ip6H->DstAddr, Dest->Octets, 16);

    NetworkPut(Buf, IPV6_HEADER_SIZE);
    Buf->Protocol = NETWORK_ETH_P_IPV6;
    RETURN(NetworkTx(Buf, Dev));
}

/*
 * ============================================================================
 * Processing incoming IPv6 packet
 * ============================================================================
 */

NOPTR IpV6Input(NetworkBuf *Buf, NetworkDevice *Dev) {
    IpV6Header *Ip6H;
    UINT16 PayloadLen;
    NetworkRxHandler Handler;
    
    if (!Buf || Buf->Len < IPV6_HEADER_SIZE) {
        ConsolePrint("Packet too short\n");
        return;
    }
    
    Ip6H = (IpV6Header*)Buf->Data;
    
    /* Version check */
    if (Ip6H->Version != 6) {
        ConsolePrint("Invalid version %d\n", Ip6H->Version);
        return;
    }
    
    /* Length check */
    PayloadLen = Ntohs(Ip6H->PayloadLen);
    if (Buf->Len < IPV6_HEADER_SIZE + PayloadLen) {
        ConsolePrint("Truncated packet\n");
        return;
    }
    
    /* Hop limit */
    if (Ip6H->HopLimit <= 1) {
        /* TODO: ICMPv6 Time Exceeded */
        return;
    }
    Ip6H->HopLimit--;
    
    /* Remove the header */
    NetworkPull(Buf, IPV6_HEADER_SIZE);
    
    /* Pass to upper level protocol */
    if (Ip6H->NextHeader < 256 && GIpV6Protocols[Ip6H->NextHeader]) {
        GIpV6Protocols[Ip6H->NextHeader](Buf, (NOPTR*)Dev);
    } else {
        ConsolePrint("Unsupported next header %d\n", Ip6H->NextHeader);
    }
}

/*
 * ============================================================================
 * Registering protocols
 * ============================================================================
 */

INT IpV6RegisterProtocol(UINT8 NextHeader, NetworkRxHandler Handler) {
    if (NextHeader >= 256) RETURN(INCORRECT_VALUE);
    if (GIpV6Protocols[NextHeader]) RETURN(ALREADY_EXISTS);
    
    GIpV6Protocols[NextHeader] = Handler;
    RETURN(SUCCESS);
}

NOPTR IpV6UnregisterProtocol(UINT8 NextHeader) {
    if (NextHeader < 256) {
        GIpV6Protocols[NextHeader] = NULLPTR;
    }
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

INT IpV6Init(NOPTR) {
    if (GIpV6Initialized) {
        RETURN(SUCCESS);
    }
    
    MemSet(GIpV6Protocols, 0, sizeof(GIpV6Protocols));
    GIpV6HopLimit = 64;
    
    GIpV6Initialized = TRUE;
    
    RETURN(SUCCESS);
}
