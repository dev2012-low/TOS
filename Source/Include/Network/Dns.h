#pragma once

#include <Network/IpV4.h>
#include <Network/Udp.h>

/*
 * ============================================================================
 * DNS Constants
 * ============================================================================
 */

#define DNS_PORT                53
#define DNS_MAX_NAME            256
#define DNS_MAX_PACKET          512
#define DNS_TIMEOUT_MS          3000
#define DNS_MAX_RETRIES         2
#define DNS_CACHE_SIZE          32
#define DNS_CACHE_TTL           300   /* 5 minutes */

/*
 * ============================================================================
 * DNS Header (packed for wire)
 * ============================================================================
 */

typedef struct ATTRIBUTE(packed) {
    UINT16 Id;
    UINT16 Flags;
    UINT16 QdCount;
    UINT16 AnCount;
    UINT16 NsCount;
    UINT16 ArCount;
} DnsHeader;

/* DNS header flags bits */
#define DNS_FLAG_QR             0x8000  /* Query (0) / Response (1) */
#define DNS_FLAG_OPCODE_MASK    0x7800
#define DNS_FLAG_AA             0x0400  /* Authoritative Answer */
#define DNS_FLAG_TC             0x0200  /* Truncated */
#define DNS_FLAG_RD             0x0100  /* Recursion Desired */
#define DNS_FLAG_RA             0x0080  /* Recursion Available */
#define DNS_FLAG_Z              0x0040  /* Reserved */
#define DNS_FLAG_RCODE_MASK     0x000F  /* Response code */

/* Response codes */
#define DNS_RCODE_NOERROR       0
#define DNS_RCODE_FORMERR       1
#define DNS_RCODE_SERVFAIL      2
#define DNS_RCODE_NXDOMAIN      3
#define DNS_RCODE_NOTIMP        4
#define DNS_RCODE_REFUSED       5

/*
 * ============================================================================
 * DNS Question / Answer
 * ============================================================================
 */

typedef struct {
    CHAR Name[DNS_MAX_NAME];
    UINT16 QType;
    UINT16 QClass;
} DnsQuestion;

typedef struct {
    CHAR Name[DNS_MAX_NAME];
    UINT16 Type;
    UINT16 Class;
    UINT32 Ttl;
    UINT16 RdLength;
    UINT8 *RData;
} DnsAnswer;

/*
 * ============================================================================
 * DNS Cache Entry
 * ============================================================================
 */

typedef struct DnsCacheEntry {
    CHAR Domain[DNS_MAX_NAME];
    IpV4Addr Ip;
    UINT64 Expires;            /* Timestamp when cache expires */
    BOOL Valid;
    struct DnsCacheEntry *Next;
} DnsCacheEntry;

/*
 * ============================================================================
 * DNS Context (for async queries)
 * ============================================================================
 */

typedef struct DnsQuery {
    UINT16 Id;
    CHAR Domain[DNS_MAX_NAME];
    NOPTR (*Callback)(IpV4Addr Ip, NOPTR *Arg);
    NOPTR *Arg;
    UINT64 StartTime;
    UINT32 Retries;
    BOOL Active;
    struct DnsQuery *Next;
} DnsQuery;

/*
 * ============================================================================
 * API Functions
 * ============================================================================
 */

/* Initialization */
NOPTR DnsInit(NOPTR);
NOPTR DnsSetNameserver(IpV4Addr Ns);

/* Synchronous resolution (blocks) */
INT DnsResolve(const CHAR *Domain, IpV4Addr *OutIp);

/* Asynchronous resolution */
INT DnsResolveAsync(const CHAR *Domain, NOPTR (*Callback)(IpV4Addr Ip, NOPTR *Arg), NOPTR *Arg);

/* Cache management */
NOPTR DnsCacheClear(NOPTR);

/* Periodic timeout/retry handler */
NOPTR DnsTimerHandler(NOPTR);

/* Utility */
const CHAR *DnsRCodeString(UINT16 RCode);
NOPTR DnsDomainToDns(const CHAR *Domain, UINT8 *Out);
NOPTR DnsDnsToDomain(const UINT8 *Packet, UINT32 *Pos, CHAR *Out, UINT32 OutLen);