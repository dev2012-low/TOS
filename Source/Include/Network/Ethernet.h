#pragma once

#include <Network/Network.h>

/*
 * ============================================================================== Ethernet constants ======================================================================================
 */

#define ETH_HLEN            14
#define ETH_FRAME_LEN       1514
#define ETH_MIN_FRAME       64

/*
 * EtherType
 */
#define ETH_P_IP            0x0800
#define ETH_P_ARP           0x0806
#define ETH_P_IPV6          0x86DD

/*
 * ============================================================================== Ethernet header =======================================================================================
 */

typedef struct ATTRIBUTE(packed) EthernetHeader {
    UINT8 DstMac[NETWORK_ETH_ALEN];
    UINT8 SrcMac[NETWORK_ETH_ALEN];
    UINT16 Type;
} EthernetHeader;

/*
 * ============================================================================================== Functions ====================================================================================
 */

/*
 * Ethernet initialization
 */
NOPTR EthernetInit(NOPTR);

/*
 * Sending an Ethernet frame
 */
INT EthernetOutput(NetworkBuf *Buf, NetworkDevice *Dev, UINT8 *DstMac, UINT16 Type);

/*
 * Processing of incoming Ethernet frame
 */
NOPTR EthernetInput(NetworkBuf *Buf, NetworkDevice *Dev);

INT EthernetRegisterProtocol(UINT16 Type, NetworkRxHandler Handler);
NOPTR EthernetUnregisterProtocol(UINT16 Type);

/*
 * MAC address to string
 */
NOPTR EthernetMacNTop(UINT8 *Mac, CHAR *Buf, UINT32 Size);
INT EthernetMacPTon(const CHAR *Str, UINT8 *Mac);

/*
 * Comparison of MAC addresses
 */
BOOL EthernetMacEqual(UINT8 *Mac1, UINT8 *Mac2);

/*
 * Broadcast MAC
 */
extern const UINT8 ETH_MAC_BROADCAST[NETWORK_ETH_ALEN];