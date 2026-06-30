#pragma once

#include <Network/Udp.h>
#include <Network/IpV4.h>

/*
 * =============================================================================== DHCP constants =================================================================================
 */

#define DHCP_SERVER_PORT        67
#define DHCP_CLIENT_PORT        68

#define DHCP_MAGIC_COOKIE       0x63825363

/*
 * DHCP Options
 */
#define DHCP_OPT_PAD            0
#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS_SERVER     6
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_DOMAIN_NAME    15
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_LIST     55
#define DHCP_OPT_MAX_MSG_SIZE   57
#define DHCP_OPT_VENDOR_ID      60
#define DHCP_OPT_CLIENT_ID      61
#define DHCP_OPT_END            255

/*
 * DHCP messages
 */
#define DHCP_DISCOVER           1
#define DHCP_OFFER              2
#define DHCP_REQUEST            3
#define DHCP_DECLINE            4
#define DHCP_ACK                5
#define DHCP_NAK                6
#define DHCP_RELEASE            7
#define DHCP_INFORM             8

/*
 * =============================================================================== DHCP header ================================================================================
 */

typedef struct ATTRIBUTE(packed) DhcpHeader {
    UINT8 Op;
    UINT8 HType;
    UINT8 HLen;
    UINT8 Hops;
    UINT32 Xid;
    UINT16 Secs;
    UINT16 Flags;
    UINT32 CIAddr;
    UINT32 YIAddr;
    UINT32 SIAddr;
    UINT32 GIAddr;
    UINT8 ChAddr[16];
    UINT8 SName[64];
    UINT8 File[128];
    UINT32 Magic;
    UINT8 Options[];
} DhcpHeader;

/*
 * ============================================================================== Preliminary declaration of the dhcp_client structure =================================================================================
 */

struct DhcpClient;

/*
 * Type of callback function (after preliminary declaration)
 */
typedef NOPTR (*DhcpCallback)(struct DhcpClient *Client, BOOL Success);

/*
 * ============================================================================== DHCP client (full definition) ===============================================================================
 */

typedef struct DhcpClient {
    NetworkDevice *Dev;
    BOOL Running;
    BOOL Configured;
    
    UINT32 Xid;
    UINT32 YIAddr;
    UINT32 ServerIp;
    UINT32 SubnetMask;
    UINT32 Gateway;
    UINT32 DnsServer;
    UINT32 LeaseTime;
    UINT64 LeaseExpires;
    UINT64 RenewTime;
    UINT64 RebindTime;
    DhcpCallback Callback;
} DhcpClient;

/*
 * ============================================================================================== Functions ====================================================================================
 */

/*
 * Initializing the DHCP client
 */
INT DhcpInit(DhcpClient *Client, NetworkDevice *Dev);

/*
 * Starting DHCP (Discover -> Request -> Ack)
 */
INT DhcpStart(DhcpClient *Client);

/*
 * Requesting a lease (renew)
 */
INT DhcpRenew(DhcpClient *Client);

/*
 * IP release
 */
INT DhcpRelease(DhcpClient *Client);

/*
 * Processing an incoming DHCP packet
 */
NOPTR DhcpInput(NetworkBuf *Buf, IpV4Addr Src, IpV4Addr Dst,
                UINT16 SrcPort, UINT16 DstPort);

/*
 * Initializing the DHCP subsystem
 */
NOPTR DhcpSubsystemInit(NOPTR);

/*
 * Run DHCP on all interfaces
 */
NOPTR DhcpStartOnAllInterfaces(DhcpCallback Callback);

/*
 * Periodic lease renewal handler
 */
NOPTR DhcpTimerHandler(NOPTR);