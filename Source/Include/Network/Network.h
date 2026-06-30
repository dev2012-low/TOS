#pragma once

#include <Kernel/Types.h>
#include <Kernel/List.h>
#include <Time/Timer.h>

/*
 * =============================================================================== Constants =====================================================================================
 */

#define NETWORK_MTU             1500
#define NETWORK_HEADROOM        64
#define NETWORK_BUFFER_SIZE     (NETWORK_MTU + NETWORK_HEADROOM)

#define NETWORK_ETH_ALEN            6
#define NETWORK_IP_ALEN             4
#define NETWORK_IP6_ALEN            16

/*
 * Protocols
 */
#define NETWORK_ETH_P_IP            0x0800
#define NETWORK_ETH_P_ARP           0x0806
#define NETWORK_ETH_P_IPV6          0x86DD

/*
 * IP protocols
 */
#define NETWORK_IP_PROTO_ICMP       1
#define NETWORK_IP_PROTO_TCP        6
#define NETWORK_IP_PROTO_UDP        17

static inline UINT16 Ntohs(UINT16 N) {
    return (N >> 8) | (N << 8);
}

static inline UINT16 Htons(UINT16 N) {
    return (N >> 8) | (N << 8);
}

static inline UINT32 Ntohl(UINT32 N) {
    return ((N & 0xFF) << 24) |
           ((N & 0xFF00) << 8) |
           ((N & 0xFF0000) >> 8) |
           ((N & 0xFF000000) >> 24);
}

static inline UINT32 Htonl(UINT32 N) {
    return ((N & 0xFF) << 24) |
           ((N & 0xFF00) << 8) |
           ((N & 0xFF0000) >> 8) |
           ((N & 0xFF000000) >> 24);
}

/*
 * ============================================================================== Structures ========================================================================================
 */

typedef enum {
    NETWORK_TYPE_NONE = 0,
    NETWORK_TYPE_ETHERNET = 1,
    NETWORK_TYPE_WIFI = 2,
    NETWORK_TYPE_LOOPBACK = 3,
    NETWORK_TYPE_PPP = 4,
    NETWORK_TYPE_BRIDGE = 5,
} NetworkDeviceType;

/*
 * Network buffer (sk_buff-like)
 */
typedef struct NetworkBuf {
    UINT8 *Data;
    UINT8 *Head;
    UINT8 *Tail;
    UINT8 *End;
    UINT32 Len;
    UINT32 HeadRoom;
    UINT32 TrueSize; 
    UINT16 Protocol;
    UINT32 IfIndex;
    UINT32 IpSrc;
    UINT32 IpDst;
    struct NetworkDevice *Dev;
    struct NetworkBuf *Next;
} NetworkBuf;

/*
 * Statistics
 */
typedef struct NetworkStats {
    UINT64 RxPackets;
    UINT64 RxBytes;
    UINT64 RxErrors;
    UINT64 RxDropped;
    
    UINT64 TxPackets;
    UINT64 TxBytes;
    UINT64 TxErrors;
    UINT64 TxDropped;
} NetworkStats;

/*
 * Network device
 */
typedef struct NetworkDevice {
    CHAR Name[16];
    UINT32 Index;
    NetworkDeviceType Type;
    
    UINT8 MacAddr[NETWORK_ETH_ALEN];
    UINT8 IpAddr[NETWORK_IP_ALEN];
    UINT8 Netmask[NETWORK_IP_ALEN];
    UINT8 Gateway[NETWORK_IP_ALEN];

    BOOL Up;
    BOOL Running;
    
    UINT32 Mtu;
    
    INT (*Open)(struct NetworkDevice *Dev);
    INT (*Stop)(struct NetworkDevice *Dev);
    INT (*Xmit)(struct NetworkBuf *Buf, struct NetworkDevice *Dev);
    INT (*Poll)(struct NetworkDevice *Dev);
    
    NetworkStats Stats;
    NOPTR *Priv;
    
    ListHead Node;
} NetworkDevice;

/*
 * Packet queue for RX/TX
 */
typedef struct NetworkQueue {
    NetworkBuf *Head;
    NetworkBuf *Tail;
    UINT32 Count;
    UINT32 MaxLen;
} NetworkQueue;

/*
 * Initializing the network stack
 */
INT NetworkInit(NOPTR);
INT NetworkLateInit(NOPTR);
NOPTR NetworkPollDevices(NOPTR);

/*
 * Registering a Network Device
 */
INT NetworkRegisterDevice(NetworkDevice *Dev);
INT NetworkUnregisterDevice(NetworkDevice *Dev);

/*
 * Sending/receiving packages
 */
INT NetworkRx(NetworkBuf *Buf, NetworkDevice *Dev);
INT NetworkTx(NetworkBuf *Buf, NetworkDevice *Dev);

/*
 * Buffer management
 */
NetworkBuf *NetworkAllocBuf(UINT32 Size);
NOPTR NetworkFreeBuf(NetworkBuf *Buf);
NOPTR NetworkReserve(NetworkBuf *Buf, UINT32 HeadRoom);
NOPTR NetworkPut(NetworkBuf *Buf, UINT32 Len);
NOPTR NetworkPull(NetworkBuf *Buf, UINT32 Len);

/*
 * Protocol registrations
 */
typedef NOPTR (*NetworkRxHandler)(NetworkBuf *Buf, NetworkDevice *Dev);

INT NetworkRegisterProtocol(UINT16 EthType, NetworkRxHandler Handler);
void NetworkUnregisterProtocol(UINT16 EthType);

/*
 * Getting a device by index/name
 */
NetworkDevice *NetworkGetDevice(UINT32 Index);
NetworkDevice *NetworkGetDeviceByName(const CHAR *Name);

/*
 * Get the first active network interface
 */
NetworkDevice *NetworkGetFirstActive(NOPTR);

/*
 * Get any network interface (first in the list)
 */
NetworkDevice *NetworkGetFirstDevice(NOPTR);

INT NetworkGetDevicesByType(NetworkDeviceType Type, NetworkDevice **Devices, INT Max);

/* Get next device in list */
NetworkDevice *NetworkGetNext(NetworkDevice *Dev);