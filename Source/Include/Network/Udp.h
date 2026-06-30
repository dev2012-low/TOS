#pragma once

#include <Network/Network.h>
#include <Network/IpV4.h>

#define UDP_HEADER_SIZE 8
#define UDP_MAX_PAYLOAD 65507

typedef struct ATTRIBUTE(packed) UdpHeader {
    UINT16 SrcPort;
    UINT16 DstPort;
    UINT16 Length;
    UINT16 Checksum;
} UdpHeader;

/*
 * UDP handler type
 */
typedef NOPTR (*UdpHandler)(NetworkBuf *Buf, IpV4Addr Src, IpV4Addr Dst,
                               UINT16 SrcPort, UINT16 DstPort);

/*
 * Registering a handler for a port
 */
INT UdpRegisterHandler(UINT16 Port, UdpHandler Handler);
NOPTR UdpUnregisterHandler(UINT16 Port);

/*
 * Basic functions
 */
INT UdpInit(NOPTR);
INT UdpInput(NetworkBuf *Buf, IpV4Addr Src, IpV4Addr Dst);
INT UdpOutput(NetworkBuf *Buf, IpV4Addr Dest, IpV4Addr Src,
              UINT16 DestPort, UINT16 SrcPort);
