#pragma once

#include <Network/Network.h>

/*
 * ============================================================================== IPv6 constants ==================================================================================
 */

#define IPV6_ADDR_LEN       16
#define IPV6_HEADER_SIZE    40

/*
 * Next Header meanings
 */
#define IPV6_NEXT_HOP_TCP   6
#define IPV6_NEXT_HOP_UDP   17
#define IPV6_NEXT_HOP_ICMP  58
#define IPV6_NEXT_HOP_HOP   0
#define IPV6_NEXT_HOP_ROUTE 43
#define IPV6_NEXT_HOP_FRAG  44
#define IPV6_NEXT_HOP_ESP   50
#define IPV6_NEXT_HOP_AH    51
#define IPV6_NEXT_HOP_NONE  59
#define IPV6_NEXT_HOP_DEST  60

/*
 * ============================================================================== IPv6 header ==================================================================================
 */

typedef struct ATTRIBUTE(packed) IpV6Header {
    UINT32 Version:4;
    UINT32 TrafficClass:8;
    UINT32 FlowLabel:20;
    UINT16 PayloadLen;
    UINT8  NextHeader;
    UINT8  HopLimit;
    UINT8  SrcAddr[16];
    UINT8  DstAddr[16];
} IpV6Header;

/*
 * =============================================================================== IPv6 address ======================================================================================
 */

typedef struct {
    union {
        UINT8 Octets[16];
        UINT16 Words[8];
        UINT32 Dwords[4];
    };
} IpV6Addr;

/*
 * Constant addresses
 */
extern const IpV6Addr IPV6_ADDR_UNSPECIFIED;    /*
 * ::
 */
extern const IpV6Addr IPV6_ADDR_LOOPBACK;       /*
 * ::1
 */
extern const IpV6Addr IPV6_ADDR_ALL_NODES;      /*
 * ff02::1
 */
extern const IpV6Addr IPV6_ADDR_ALL_ROUTERS;    /*
 * ff02::2
 */

/*
 * Conversion
 */
IpV6Addr IpV6PTon(const CHAR *Str);
NOPTR IpV6NTop(IpV6Addr Addr, CHAR *Buf, UINT32 Size);

/*
 * Checks
 */
BOOL IpV6AddrIsMulticast(IpV6Addr Addr);
BOOL IpV6AddrIsLinkLocal(IpV6Addr Addr);
BOOL IpV6AddrIsLoopback(IpV6Addr Addr);
BOOL IpV6AddrIsUnspecified(IpV6Addr Addr);

/*
 * ============================================================================== Main functions ======================================================================================
 */

/*
 * IPv6 initialization
 */
INT IpV6Init(NOPTR);

/*
 * Sending an IPv6 packet
 */
INT IpV6Output(NetworkBuf *Buf, IpV6Addr *Dest, IpV6Addr *Src, UINT8 NextHeader);

/*
 * Processing an incoming IPv6 packet
 */
NOPTR IpV6Input(NetworkBuf *Buf, NetworkDevice *Dev);

/*
 * Registering Upper Level Protocols
 */
INT IpV6RegisterProtocol(UINT8 NextHeader, NetworkRxHandler Handler);
NOPTR IpV6UnregisterProtocol(UINT8 NextHeader);