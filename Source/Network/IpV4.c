#include <Network/IpV4.h>
#include <Network/Arp.h>
#include <Network/ICmp.h>
#include <Network/Ethernet.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Memory/Allocator.h>
#include <Kernel/Return.h>
#include <Console.h>

/*
 * ============================================================================
 * Global routing table
 * ============================================================================
 */

static ListHead GIpV4Routes;
static NetworkRxHandler GIpV4Protocols[256];
static UINT16 GIpV4IdCounter = 0;
static BOOL GIpV4Initialized = FALSE;

/*
 * ============================================================================
 * IPv4 addressing functions
 * ============================================================================
 */

IpV4Addr IpV4PTon(const CHAR *Str) {
    IpV4Addr Addr = { .Addr = 0 };
    UINT32 Octets[4] = {0, 0, 0, 0};
    INT I = 0;
    UINT32 Val = 0;
    
    while (*Str && I < 4) {
        if (*Str >= '0' && *Str <= '9') {
            Val = Val * 10 + (*Str - '0');
        } else if (*Str == '.') {
            Octets[I++] = Val;
            Val = 0;
        } else {
            break;
        }
        Str++;
    }
    
    if (I == 3 && Val <= 255) {
        Octets[3] = Val;
        Addr.Octets[0] = (UINT8)Octets[0];
        Addr.Octets[1] = (UINT8)Octets[1];
        Addr.Octets[2] = (UINT8)Octets[2];
        Addr.Octets[3] = (UINT8)Octets[3];
    }
    
    return Addr;
}

NOPTR IpV4NTop(IpV4Addr Addr, CHAR *Buf, UINT32 Size) {
    if (!Buf || Size == 0) return;
    
    SnPrintf(Buf, Size, "%u.%u.%u.%u",
             Addr.Octets[0], Addr.Octets[1],
             Addr.Octets[2], Addr.Octets[3]);
}

BOOL IpV4AddrIsMulticast(IpV4Addr Addr) {
    return (Addr.Octets[0] & 0xF0) == 0xE0;  /* 224.0.0.0 - 239.255.255.255 */
}

BOOL IpV4AddrIsBroadcast(IpV4Addr Addr) {
    return Addr.Addr == 0xFFFFFFFF;
}

BOOL IpV4AddrIsLoopback(IpV4Addr Addr) {
    return Addr.Octets[0] == 127;
}

/*
 * ============================================================================
 * IPv4 Checksum (RFC 1071)
 * ============================================================================
 */

UINT16 IpV4Checksum(UINT16 *Data, UINT32 Len) {
    UINT32 Sum = 0;
    
    while (Len > 1) {
        Sum += *Data++;
        Len -= 2;
    }
    
    if (Len) {
        Sum += *(UINT8*)Data;
    }
    
    while (Sum >> 16) {
        Sum = (Sum & 0xFFFF) + (Sum >> 16);
    }
    
    return ~Sum;
}

/*
 * ============================================================================
 * Routing
 * ============================================================================
 */

INT IpV4RouteAdd(IpV4Addr Dest, IpV4Addr Netmask,
                   IpV4Addr Gateway, IpV4Addr Src,
                   NetworkDevice *Dev, UINT32 Metric) {
    IpV4Route *Route;
    CHAR DestStr[16];
    CHAR NetmaskStr[16];
    CHAR GatewayStr[16];
    
    Route = (IpV4Route*)MemoryAllocate(sizeof(IpV4Route));
    if (!Route) RETURN(NO_MEMORY);
    
    Route->Dest = Dest;
    Route->Netmask = Netmask;
    Route->Gateway = Gateway;
    Route->Src = Src;
    Route->Dev = Dev;
    Route->Metric = Metric;
    
    ListAddTail(&GIpV4Routes, &Route->Node);
    
    IpV4NTop(Dest, DestStr, 16);
    IpV4NTop(Netmask, NetmaskStr, 16);
    IpV4NTop(Gateway, GatewayStr, 16);
    
    RETURN(SUCCESS);
}

INT IpV4RouteDel(IpV4Addr Dest, IpV4Addr Netmask) {
    ListHead *Pos;
    ListHead *N;
    
    ListForEachSafe(Pos, N, &GIpV4Routes) {
        IpV4Route *Route = ListEntry(Pos, IpV4Route, Node);
        
        if (Route->Dest.Addr == Dest.Addr && Route->Netmask.Addr == Netmask.Addr) {
            ListDel(&Route->Node);
            MemoryFree(Route);
            RETURN(SUCCESS);
        }
    }
    
    RETURN(NOT_FOUND);
}

IpV4Route *IpV4RouteLookup(IpV4Addr Dest) {
    IpV4Route *Best = NULLPTR;
    ListHead *Pos;
    
    ListForEach(Pos, &GIpV4Routes) {
        IpV4Route *Route = ListEntry(Pos, IpV4Route, Node);
        UINT32 MaskedDest = Dest.Addr & Route->Netmask.Addr;
        
        if (MaskedDest == Route->Dest.Addr) {
            if (!Best || Route->Metric < Best->Metric) {
                Best = Route;
            }
        }
    }
    
    return Best;
}

IpV4Route *IpV4RouteGetDefault(NOPTR) {
    ListHead *Pos;
    
    ListForEach(Pos, &GIpV4Routes) {
        IpV4Route *Route = ListEntry(Pos, IpV4Route, Node);
        if (Route->Dest.Addr == 0 && Route->Netmask.Addr == 0) {
            return Route;
        }
    }
    
    return NULLPTR;
}

/*
 * ============================================================================
 * Sending an IPv4 packet
 * ============================================================================
 */

INT IpV4Output(NetworkBuf *Buf, IpV4Addr Dest, IpV4Addr Src, UINT8 Protocol) {
    IpV4Route *Route;
    IpV4Header *IpH;
    UINT8 DstMac[6];
    CHAR DestStr[16];
    INT Result;
    
    Route = IpV4RouteLookup(Dest);
    if (!Route) {
        Route = IpV4RouteGetDefault();
        if (!Route) RETURN(NO_OBJECT);
    }
    
    NetworkReserve(Buf, IPV4_HEADER_MIN);
    
    IpH = (IpV4Header*)Buf->Data;
    MemSet(IpH, 0, sizeof(IpV4Header));
    
    IpH->Version = 4;
    IpH->Ihl = 5;  /* 5 * 4 = 20 bytes */
    IpH->Tos = 0;
    IpH->TotalLen = Htons((UINT16)Buf->Len);
    IpH->Id = Htons(++GIpV4IdCounter);
    IpH->FragOff = 0;
    IpH->Ttl = 64;
    IpH->Protocol = Protocol;
    IpH->SrcAddr = Src.Addr;
    
    /* If src is not specified, take it from the route or interface */
    if (IpH->SrcAddr == 0) {
        if (Route->Src.Addr != 0) {
            IpH->SrcAddr = Route->Src.Addr;
        } else if (Route->Dev && Route->Dev->IpAddr) {
            IpH->SrcAddr = ((IpV4Addr*)Route->Dev->IpAddr)->Addr;
        } else {
            RETURN(NO_OBJECT);
        }
    }
    
    IpH->DstAddr = Dest.Addr;
    
    IpH->Checksum = 0;
    IpH->Checksum = IpV4Checksum((UINT16*)IpH, IpH->Ihl * 4);
    
    /* Determine next hop */
    IpV4Addr NextHop = Dest;
    if (Route->Gateway.Addr != 0) {
        NextHop = Route->Gateway;
    }
    
    /* Send via ARP (if not loopback) */
    if (!IpV4AddrIsLoopback(Dest)) {
        Result = ArpResolve(Route->Dev, NextHop, DstMac, 1000);
        if (Result == SUCCESS) {
            return EthernetOutput(Buf, Route->Dev, DstMac, ETH_P_IP);
        }
        RETURN(Result);
    } else {
        /* Loopback - send to ourselves */
        return EthernetOutput(Buf, Route->Dev, Route->Dev->MacAddr, ETH_P_IP);
    }
}

/*
 * ============================================================================
 * Processing incoming IPv4 packet
 * ============================================================================
 */

NOPTR IpV4Input(NetworkBuf *Buf, NetworkDevice *Dev) {
    IpV4Header *IpH;
    UINT16 Checksum;
    NetworkRxHandler Handler;
    
    if (Buf->Len < IPV4_HEADER_MIN) {
        ConsolePrint("Packet too short\n");
        return;
    }
    
    IpH = (IpV4Header*)Buf->Data;
    
    /* Version check */
    if (IpH->Version != 4) {
        ConsolePrint("Invalid version %d\n", IpH->Version);
        return;
    }
    
    /* Checksum check */
    Checksum = IpH->Checksum;
    IpH->Checksum = 0;
    if (IpV4Checksum((UINT16*)IpH, IpH->Ihl * 4) != Checksum) {
        ConsolePrint("Bad checksum\n");
        return;
    }
    IpH->Checksum = Checksum;
    
    /* TTL */
    if (IpH->Ttl <= 1) {
        NetworkBuf *IcmpBuf = NetworkAllocBuf(Buf->Len + 8);
        if (IcmpBuf) {
            MemCpy(IcmpBuf->Data, IpH, IpH->Ihl * 4 + 8);
            IcmpBuf->Len = IpH->Ihl * 4 + 8;
            
            ICmpOutput(IcmpBuf,
                       (IpV4Addr){ .Addr = IpH->SrcAddr },
                       (IpV4Addr){ .Addr = IpH->DstAddr },
                       ICMP_TYPE_TIME_EXCEEDED, ICMP_CODE_TTL_EXCEEDED);
            NetworkFreeBuf(IcmpBuf);
        }
        return;
    }
    
    /* Remove the header */
    Buf->IpSrc = IpH->SrcAddr;
    Buf->IpDst = IpH->DstAddr;
    NetworkPull(Buf, IpH->Ihl * 4);
    
    /* Pass to upper level protocol */
    if (IpH->Protocol < 256 && GIpV4Protocols[IpH->Protocol]) {
        GIpV4Protocols[IpH->Protocol](Buf, (NOPTR*)Dev);
    } else {
        ConsolePrint("Unsupported protocol %d\n", IpH->Protocol);
    }
}

/*
 * ============================================================================
 * Registering protocols
 * ============================================================================
 */

INT IpV4RegisterProtocol(UINT8 Protocol, NetworkRxHandler Handler) {
    if (Protocol >= 256) RETURN(INCORRECT_VALUE);
    if (GIpV4Protocols[Protocol]) RETURN(ALREADY_EXISTS);
    
    GIpV4Protocols[Protocol] = Handler;
    RETURN(SUCCESS);
}

NOPTR IpV4UnregisterProtocol(UINT8 Protocol) {
    if (Protocol < 256) {
        GIpV4Protocols[Protocol] = NULLPTR;
    }
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

INT IpV4Init(NOPTR) {
    if (GIpV4Initialized) {
        RETURN(SUCCESS);
    }
    
    ListInit(&GIpV4Routes);
    MemSet(GIpV4Protocols, 0, sizeof(GIpV4Protocols));
    GIpV4IdCounter = 0;
    
    GIpV4Initialized = TRUE;
    
    RETURN(SUCCESS);
}
