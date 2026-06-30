#pragma once

#include <Network/Ethernet.h>
#include <Network/IpV4.h>
#include <Network/Network.h>

/*
 * ============================================================================== ARP constants =================================================================================
 */

#define ARP_HRD_ETHERNET    0x0001
#define ARP_PRO_IP          0x0800
#define ARP_HLEN_ETHERNET   6
#define ARP_PLEN_IP         4

#define ARP_OP_REQUEST      0x0001
#define ARP_OP_REPLY        0x0002

/*
 * ============================================================================== ARP header =================================================================================
 */

typedef struct ATTRIBUTE(packed) ArpHeader {
    UINT16 Hrd;
    UINT16 Pro;
    UINT8  Hln;
    UINT8  Pln;
    UINT16 Op;
    UINT8  Sha[NETWORK_ETH_ALEN];
    UINT32 Spa;
    UINT8  Tha[NETWORK_ETH_ALEN];
    UINT32 Tpa;
} ArpHeader;

/*
 * ============================================================================== ARP cache ====================================================================================
 */

#define ARP_CACHE_SIZE      32
#define ARP_CACHE_TIMEOUT   300000   /*
 * 5 minutes in milliseconds
 */

typedef struct ArpCacheEntry {
    IpV4Addr Ip;
    UINT8 Mac[NETWORK_ETH_ALEN];
    UINT64 Timestamp;
    BOOL Valid;
} ArpCacheEntry;

/*
 * ============================================================================================== Functions ====================================================================================
 */

/*
 * ARP initialization
 */
NOPTR ArpInit(NOPTR);

/*
 * Sending an ARP request
 */
INT ArpRequest(IpV4Addr Ip, NetworkDevice *Dev);

/*
 * Processing an incoming ARP packet
 */
NOPTR ArpInput(NetworkBuf *Buf, NetworkDevice *Dev);

/*
 * IP → MAC resolution (blocking with timeout)
 */
INT ArpResolve(NetworkDevice *Dev, IpV4Addr Ip, UINT8 *Mac, UINT32 TimeoutMs);

/*
 * Adding/removing a cache entry
 */
NOPTR ArpCacheAdd(IpV4Addr Ip, UINT8 *Mac);
NOPTR ArpCacheDel(IpV4Addr Ip);
NOPTR ArpCacheClear(NOPTR);
NOPTR ArpCachePurgeExpired(NOPTR);

/*
 * Cache search
 */
BOOL ArpCacheLookup(IpV4Addr Ip, UINT8 *Mac);