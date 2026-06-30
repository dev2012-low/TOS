#pragma once

#include <Network/Network.h>

/*
 * ============================================================================== IPv4 constants ==================================================================================
 */

#define IPV4_ADDR_LEN       4
#define IPV4_HEADER_MIN     20
#define IPV4_HEADER_MAX     60

/*
 * Protocols
 */
#define IPV4_PROTO_ICMP     1
#define IPV4_PROTO_TCP      6
#define IPV4_PROTO_UDP      17

/*
 * Fragmentation Flags
 */
#define IPV4_FLAG_MORE_FRAG  0x2000
#define IPV4_FLAG_DONT_FRAG  0x4000
#define IPV4_FLAG_RESERVED   0x8000
#define IPV4_FRAG_OFFSET_MASK 0x1FFF

/*
 * ============================================================================== IPv4 header ==================================================================================
 */

typedef struct ATTRIBUTE(packed) IpV4Header {
    UINT8  Ihl:4;
    UINT8  Version:4;
    UINT8  Tos; // :D
    UINT16 TotalLen;
    UINT16 Id;
    UINT16 FragOff;
    UINT8  Ttl;
    UINT8  Protocol;
    UINT16 Checksum;
    UINT32 SrcAddr;
    UINT32 DstAddr;
} IpV4Header;

/*
 * =============================================================================== IPv4 address =====================================================================================
 */

typedef struct {
    union {
        UINT32 Addr;
        UINT8 Octets[4];
    };
} IpV4Addr;

/*
 * Conversion
 */
IpV4Addr IpV4PTon(const CHAR *Str);
NOPTR IpV4NTop(IpV4Addr Addr, CHAR *Buf, UINT32 Size);

/*
 * Checks
 */
BOOL IpV4AddrIsMulticast(IpV4Addr addr);
BOOL IpV4AddrIsBroadcast(IpV4Addr addr);
BOOL IpV4AddrIsLoopback(IpV4Addr Addr);

/*
 * =============================================================================== Routing ====================================================================================
 */

typedef struct IpV4Route {
    IpV4Addr Dest;
    IpV4Addr Netmask;
    IpV4Addr Gateway;
    IpV4Addr Src;
    NetworkDevice *Dev;
    UINT32 Metric;
    
    ListHead Node;
} IpV4Route;

/*
 * Adding/removing a route
 */
INT IpV4RouteAdd(IpV4Addr Dest, IpV4Addr Netmask,
                   IpV4Addr Gateway, IpV4Addr Src,
                   NetworkDevice *Dev, UINT32 Metric);
INT IpV4RouteDel(IpV4Addr Dest, IpV4Addr Netmask);

/*
 * Finding a route
 */
IpV4Route *IpV4RouteLookup(IpV4Addr Dest);
IpV4Route *IpV4RouteGetDefault(NOPTR);

/*
 * ============================================================================== Main functions ======================================================================================
 */

/*
 * IPv4 initialization
 */
INT IpV4Init(NOPTR);

/*
 * Sending an IPv4 packet
 */
INT IpV4Output(NetworkBuf *Buf, IpV4Addr Dest, IpV4Addr Src, UINT8 Protocol);

/*
 * Processing an incoming IPv4 packet
 */
NOPTR IpV4Input(NetworkBuf *Buf, NetworkDevice *Dev);

/*
 * Fragmentation
 */
INT IpV4Fragment(NetworkBuf *Buf, UINT32 Mtu);

/*
 * Registering Upper Level Protocols
 */
INT IpV4RegisterProtocol(UINT8 Protocol, NetworkRxHandler Handler);
NOPTR IpV4UnregisterProtocol(UINT8 Protocol);

/*
 * Checksum (RFC 1071)
 */
UINT16 IpV4Checksum(UINT16 *Data, UINT32 Len);