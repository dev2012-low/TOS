#pragma once

#include <Network/IpV4.h>

/*
 * ============================================================================
 * ICMP constants
 * ============================================================================
 */

#define ICMP_TYPE_ECHO_REPLY        0
#define ICMP_TYPE_DEST_UNREACH      3
    #define ICMP_CODE_NET_UNREACH       0
    #define ICMP_CODE_HOST_UNREACH      1
    #define ICMP_CODE_PROT_UNREACH      2
    #define ICMP_CODE_PORT_UNREACH      3
    #define ICMP_CODE_FRAG_NEEDED       4
#define ICMP_TYPE_SOURCE_QUENCH     4
#define ICMP_TYPE_REDIRECT          5
#define ICMP_TYPE_ECHO_REQUEST      8
#define ICMP_TYPE_TIME_EXCEEDED     11
    #define ICMP_CODE_TTL_EXCEEDED      0
    #define ICMP_CODE_REASSEMBLY        1
#define ICMP_TYPE_PARAM_PROBLEM     12
#define ICMP_TYPE_TIMESTAMP         13
#define ICMP_TYPE_TIMESTAMP_REPLY   14

#define ICMP_DATA_SIZE              56  /* 64 - 8 = 56 bytes of data */

/*
 * ============================================================================
 * ICMP headers
 * ============================================================================
 */

typedef struct ATTRIBUTE(packed) ICmpHeader {
    UINT8 Type;
    UINT8 Code;
    UINT16 Checksum;
    UINT16 Id;
    UINT16 Sequence;
} ICmpHeader;

typedef struct ATTRIBUTE(packed) ICmpUnreachHeader {
    UINT8 Type;
    UINT8 Code;
    UINT16 Checksum;
    UINT16 Unused;
    UINT16 Mtu;
} ICmpUnreachHeader;

/*
 * ============================================================================
 * Ping statistics
 * ============================================================================
 */

typedef struct PingStats {
    UINT32 Sent;
    UINT32 Received;
    UINT32 Lost;
    UINT64 MinRttUs;
    UINT64 MaxRttUs;
    UINT64 TotalRttUs;
    UINT64 AvgRttUs;
} PingStats;

/*
 * ============================================================================
 * Ping callback
 * ============================================================================
 */

typedef void (*PingCallback)(PingStats *Stats, NOPTR *Arg);

/*
 * ============================================================================
 * API Functions
 * ============================================================================
 */

/* Initialization */
INT ICmpInit(NOPTR);

NOPTR ICmpTimerHandler(NOPTR);

/* Send ICMP packet */
INT ICmpOutput(NetworkBuf *Buf, IpV4Addr Dest, IpV4Addr Src, UINT8 Type, UINT8 Code);

/* Process incoming ICMP */
NOPTR ICmpInput(NetworkBuf *Buf, NetworkDevice *Dev);;

/* Ping (blocking) */
INT ICmpPing(IpV4Addr Dest, UINT32 Count, UINT32 TimeoutMs, PingStats *Stats);

/* Ping async */
INT ICmpPingAsync(IpV4Addr Dest, UINT32 Count, UINT32 TimeoutMs, 
                    PingCallback Callback, NOPTR *Arg);

/* Cancel ongoing ping */
NOPTR ICmpPingCancel(NOPTR);

/* Utility */
UINT16 ICmpChecksum(UINT16 *Data, UINT32 Len);
const CHAR* ICmpTypeString(UINT8 Type);
const CHAR* ICmpCodeString(UINT8 Type, UINT8 Code);
