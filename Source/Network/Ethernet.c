#include <Network/Ethernet.h>
#include <Network/Arp.h>
#include <Network/IpV4.h>
#include <Network/IpV6.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Kernel/Return.h>

const UINT8 ETH_MAC_BROADCAST[NETWORK_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static NetworkRxHandler EthProtocols[65536];

/*
 * ============================================================================================== MAC address of the function =================================================================================
 */

NOPTR EthernetMacNTop(UINT8 *Mac, CHAR *Buf, UINT32 Size) {
    SnPrintf(Buf, Size, "%02x:%02x:%02x:%02x:%02x:%02x",
             Mac[0], Mac[1], Mac[2], Mac[3], Mac[4], Mac[5]);
}

INT EthernetMacPTon(const CHAR *Str, UINT8 *Mac) {
    INT I;
    UINT32 Val;
    
    for (I = 0; I < 6; I++) {
        /*
 * Manual hex parsing instead of sscanf
 */
        Val = 0;
        if (*Str >= '0' && *Str <= '9') Val = (*Str - '0') * 16;
        else if (*Str >= 'a' && *Str <= 'f') Val = (*Str - 'a' + 10) * 16;
        else if (*Str >= 'A' && *Str <= 'F') Val = (*Str - 'A' + 10) * 16;
        else RETURN(GENERAL_ERROR);
        Str++;
        
        if (*Str >= '0' && *Str <= '9') Val += (*Str - '0');
        else if (*Str >= 'a' && *Str <= 'f') Val += (*Str - 'a' + 10);
        else if (*Str >= 'A' && *Str <= 'F') Val += (*Str - 'A' + 10);
        else RETURN(GENERAL_ERROR);
        Str++;
        
        Mac[I] = Val & 0xFF;
        
        if (I < 5 && *Str == ':') Str++;
    }
    
    RETURN(SUCCESS);
}

BOOL EthernetMacEqual(UINT8 *Mac1, UINT8 *Mac2) {
    return MemCmp(Mac1, Mac2, NETWORK_ETH_ALEN) == 0;
}

/*
 * ============================================================================== Sending/receiving =================================================================================
 */

INT EthernetOutput(NetworkBuf *Buf, NetworkDevice *Dev, UINT8 *DstMac, UINT16 Type) {
    if (!Buf || !Dev || !DstMac) RETURN(NO_OBJECT);
    
    NetworkReserve(Buf, ETH_HLEN);
    
    EthernetHeader *Eth = (EthernetHeader*)Buf->Data;
    MemCpy(Eth->DstMac, DstMac, NETWORK_ETH_ALEN);
    MemCpy(Eth->SrcMac, Dev->MacAddr, NETWORK_ETH_ALEN);
    Eth->Type = Htons(Type);
    
    /*
 * Add padding to the minimum frame size
 */
    if (Buf->Len < ETH_MIN_FRAME - ETH_HLEN) {
        UINT32 Pad = ETH_MIN_FRAME - ETH_HLEN - Buf->Len;
        MemSet(Buf->Data + Buf->Len, 0, Pad);
        Buf->Len += Pad;
    }
    
    return Dev->Xmit(Buf, Dev);
}

NOPTR EthernetInput(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (Buf->Len < ETH_HLEN) return;
    
    EthernetHeader *Eth = (EthernetHeader*)Buf->Data;
    UINT16 Type = Ntohs(Eth->Type);
    
    /*
 * Removing the Ethernet header
 */
    NetworkPull(Buf, ETH_HLEN);
    
    /*
 * We filter not our MAC or broadcast
 */
    if (!EthernetMacEqual(Eth->DstMac, Dev->MacAddr) &&
        !EthernetMacEqual(Eth->DstMac, (UINT8*)ETH_MAC_BROADCAST)) {
        return;
    }
    
    /*
 * We pass it to the upper level protocol
 */
    if (Type < 65536 && EthProtocols[Type]) {
        EthProtocols[Type](Buf, Dev);
    }
}

/*
 * ============================================================================== Registering protocols ====================================================================================
 */

INT EthernetRegisterProtocol(UINT16 Type, NetworkRxHandler Handler) {
    if (Type >= 65536) RETURN(INCORRECT_VALUE);
    if (EthProtocols[Type]) RETURN(GENERAL_ERROR);
    
    EthProtocols[Type] = Handler;
    RETURN(SUCCESS);
}

NOPTR EthernetUnregisterProtocol(UINT16 Type) {
    if (Type < 65536) {
        EthProtocols[Type] = NULLPTR;
    }
}

/*
 * =============================================================================== Initialization ======================================================================================
 */

NOPTR EthernetInit(NOPTR) {
    MemSet(EthProtocols, 0, sizeof(EthProtocols));
    
    /*
 * Registering IP protocols (wrappers for type conversion)
 */
    EthernetRegisterProtocol(ETH_P_IP, (NetworkRxHandler)IpV4Input);
    EthernetRegisterProtocol(ETH_P_IPV6, (NetworkRxHandler)IpV6Input);
    EthernetRegisterProtocol(ETH_P_ARP, (NetworkRxHandler)ArpInput);
}