#include <Network/Network.h>
#include <Network/Ethernet.h>
#include <Network/Arp.h>
#include <Network/ICmp.h>
#include <Network/IpV4.h>
#include <Network/IpV6.h>
#include <Network/Tcp.h>
#include <Network/Udp.h>
#include <Network/Dns.h>
#include <Network/Dhcp.h>
#include <Network/Loopback.h>
#include <Network/VirtioNet.h>
#include <Network/E1000.h>
#include <Network/Rtl8139.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Console.h>
#include <Kernel/Return.h>

/*
 * ============================================================================
 * Global Variables
 * ============================================================================
 */

ListHead GNetworkDevices;
static UINT32 GNetworkDeviceCount = 0;
static BOOL GNetworkInitialized = FALSE;

/*
 * ============================================================================
 * Buffer Management
 * ============================================================================
 */

NetworkBuf *NetworkAllocBuf(UINT32 Size) {
    NetworkBuf *Buf;
    UINT32 TotalSize;
    
    Buf = (NetworkBuf*)MemoryAllocate(sizeof(NetworkBuf));
    if (!Buf) return NULLPTR;
    
    TotalSize = Size + NETWORK_HEADROOM;
    Buf->Head = (UINT8*)MemoryAllocate(TotalSize);
    if (!Buf->Head) {
        MemoryFree(Buf);
        return NULLPTR;
    }
    
    Buf->Data = Buf->Head + NETWORK_HEADROOM;
    Buf->Tail = Buf->Data;
    Buf->End = Buf->Head + TotalSize;
    Buf->Len = 0;
    Buf->HeadRoom = NETWORK_HEADROOM;
    Buf->TrueSize = TotalSize;
    Buf->Protocol = 0;
    Buf->IfIndex = 0;
    Buf->Dev = NULLPTR;
    Buf->Next = NULLPTR;
    
    return Buf;
}

NOPTR NetworkFreeBuf(NetworkBuf *Buf) {
    if (!Buf) return;
    if (Buf->Head) MemoryFree(Buf->Head);
    MemoryFree(Buf);
}

NOPTR NetworkReserve(NetworkBuf *Buf, UINT32 HeadRoom) {
    if (!Buf || HeadRoom > Buf->HeadRoom) return;
    Buf->Data += HeadRoom;
    Buf->HeadRoom -= HeadRoom;
}

NOPTR NetworkPut(NetworkBuf *Buf, UINT32 Len) {
    if (!Buf || Buf->Tail + Len > Buf->End) return;
    Buf->Tail += Len;
    Buf->Len += Len;
}

NOPTR NetworkPull(NetworkBuf *Buf, UINT32 Len) {
    if (!Buf || Len > Buf->Len) return;
    Buf->Data += Len;
    Buf->Len -= Len;
}

/*
 * ============================================================================
 * Device Management
 * ============================================================================
 */

INT NetworkRegisterDevice(NetworkDevice *Dev) {
    CHAR MacStr[18];
    
    if (!Dev) RETURN(NO_OBJECT);
    
    Dev->Index = GNetworkDeviceCount++;
    ListAddTail(&GNetworkDevices, &Dev->Node);

    RETURN(SUCCESS);
}

INT NetworkUnregisterDevice(NetworkDevice *Dev) {
    if (!Dev) RETURN(NO_OBJECT);
    
    ListDel(&Dev->Node);
    GNetworkDeviceCount--;
    
    RETURN(SUCCESS);
}

INT NetworkRx(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (!Buf || !Dev) RETURN(NO_OBJECT);
    
    Buf->Dev = Dev;
    Dev->Stats.RxPackets++;
    Dev->Stats.RxBytes += Buf->Len;
    EthernetInput(Buf, Dev);
    
    RETURN(SUCCESS);
}

INT NetworkTx(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (!Buf || !Dev) RETURN(NO_OBJECT);
    if (!Dev->Xmit) RETURN(NOT_IMPLEMENTED);
    
    Dev->Stats.TxPackets++;
    Dev->Stats.TxBytes += Buf->Len;
    return Dev->Xmit(Buf, Dev);
}

NOPTR NetworkPollDevices(NOPTR) {
    ListHead *Pos;

    if (GNetworkDevices.Next == NULLPTR || GNetworkDevices.Prev == NULLPTR) {
        return;
    }
    
    // Проверка, что список не пуст
    if (ListEmpty(&GNetworkDevices)) {
        return;
    }
    
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Dev = ListEntry(Pos, NetworkDevice, Node);
        if (Dev->Up && Dev->Running && Dev->Poll) {
            Dev->Poll(Dev);
        }
    }
}

NetworkDevice *NetworkGetDevice(UINT32 Index) {
    ListHead *Pos;
    
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Dev = ListEntry(Pos, NetworkDevice, Node);
        if (Dev->Index == Index) return Dev;
    }
    
    return NULLPTR;
}

NetworkDevice *NetworkGetDeviceByName(const CHAR *Name) {
    ListHead *Pos;
    
    if (!Name) return NULLPTR;
    
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Dev = ListEntry(Pos, NetworkDevice, Node);
        if (StrCmp(Dev->Name, Name) == 0) return Dev;
    }
    
    return NULLPTR;
}

NetworkDevice *NetworkGetFirstDevice(NOPTR) {
    if (ListEmpty(&GNetworkDevices)) return NULLPTR;
    return ListEntry(GNetworkDevices.Next, NetworkDevice, Node);
}

NetworkDevice *NetworkGetFirstActive(NOPTR) {
    ListHead *Pos;
    
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Dev = ListEntry(Pos, NetworkDevice, Node);
        if (Dev->Up && Dev->Running) {
            return Dev;
        }
    }
    
    return NULLPTR;
}

INT NetworkGetDevicesByType(NetworkDeviceType Type, NetworkDevice **Devices, INT Max) {
    ListHead *Pos;
    INT Count = 0;
    
    if (!Devices || Max <= 0) RETURN(INCORRECT_VALUE);
    
    ListForEach(Pos, &GNetworkDevices) {
        NetworkDevice *Dev = ListEntry(Pos, NetworkDevice, Node);
        if (Dev->Type == Type) {
            Devices[Count++] = Dev;
            if (Count >= Max) break;
        }
    }
    
    RETURN(Count);
}

NetworkDevice *NetworkGetNext(NetworkDevice *Dev) {
    if (!Dev) return NULLPTR;
    
    if (Dev->Node.Next == &GNetworkDevices) {
        return NULLPTR;
    }
    
    return ListEntry(Dev->Node.Next, NetworkDevice, Node);
}

/*
 * ============================================================================
 * Protocol Registration
 * ============================================================================
 */

INT NetworkRegisterProtocol(UINT16 EthType, NetworkRxHandler Handler) {
    /* Ethernet layer handles this via EthernetRegisterProtocol */
    return EthernetRegisterProtocol(EthType, Handler);
}

NOPTR NetworkUnregisterProtocol(UINT16 EthType) {
    EthernetUnregisterProtocol(EthType);
}

/*
 * ============================================================================
 * Network Initialization
 * ============================================================================
 */

INT NetworkInit(NOPTR) {
    if (GNetworkInitialized) {
        RETURN(SUCCESS);
    }
    
    ListInit(&GNetworkDevices);
    GNetworkDeviceCount = 0;
    
    EthernetInit();
    ArpInit();
    IpV4Init();
    IpV6Init();
    ICmpInit();
    TcpInit();
    UdpInit();
    DnsInit();
    DhcpSubsystemInit();
    LoopbackInit();
    GNetworkInitialized = TRUE;
    RETURN(SUCCESS);
}

INT NetworkLateInit(NOPTR) {
    VirtioNetInit();
    E1000Init();
    Rtl8139Init();
    RETURN(SUCCESS);
}
