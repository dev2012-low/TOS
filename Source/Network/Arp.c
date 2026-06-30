#include <Network/Arp.h>
#include <Network/Network.h>
#include <Lib/String.h>
#include <Time/Timer.h>

static ArpCacheEntry ArpCache[ARP_CACHE_SIZE];
static BOOL ArpPendingRequests[ARP_CACHE_SIZE];

/*
 * ============================================================================== ARP cache functions =================================================================================
 */

NOPTR ArpCacheAdd(IpV4Addr Ip, UINT8 *Mac) {
    INT Oldest = -1;
    UINT64 OldestTime = ~0ULL;
    
    for (INT I = 0; I < ARP_CACHE_SIZE; I++) {
        if (ArpCache[I].Valid && ArpCache[I].Ip.Addr == Ip.Addr) {
            MemCpy(ArpCache[I].Mac, Mac, NETWORK_ETH_ALEN);
            ArpCache[I].Timestamp = TimerTicks();
            return;
        }
        if (!ArpCache[I].Valid) {
            Oldest = I;
            break;
        }
        if (ArpCache[I].Timestamp < OldestTime) {
            OldestTime = ArpCache[I].Timestamp;
            Oldest = I;
        }
    }
    
    if (Oldest >= 0) {
        ArpCache[Oldest].Ip = Ip;
        MemCpy(ArpCache[Oldest].Mac, Mac, NETWORK_ETH_ALEN);
        ArpCache[Oldest].Timestamp = TimerTicks();
        ArpCache[Oldest].Valid = TRUE;
    }
}

NOPTR ArpCacheDel(IpV4Addr Ip) {
    for (INT I = 0; I < ARP_CACHE_SIZE; I++) {
        if (ArpCache[I].Valid && ArpCache[I].Ip.Addr == Ip.Addr) {
            ArpCache[I].Valid = FALSE;
            return;
        }
    }
}

NOPTR ArpCacheClear(NOPTR) {
    MemSet(ArpCache, 0, sizeof(ArpCache));
}

NOPTR ArpCachePurgeExpired(NOPTR) {
    UINT64 Now = TimerTicks();
    UINT64 TimeoutTicks = TimerMsToTicks(ARP_CACHE_TIMEOUT);
    
    for (INT I = 0; I < ARP_CACHE_SIZE; I++) {
        if (ArpCache[I].Valid) {
            if (Now - ArpCache[I].Timestamp > TimeoutTicks) {
                ArpCache[I].Valid = FALSE;
            }
        }
    }
}

BOOL ArpCacheLookup(IpV4Addr Ip, UINT8 *Mac) {
    ArpCachePurgeExpired();
    
    for (INT I = 0; I < ARP_CACHE_SIZE; I++) {
        if (ArpCache[I].Valid && ArpCache[I].Ip.Addr == Ip.Addr) {
            if (Mac) MemCpy(Mac, ArpCache[I].Mac, NETWORK_ETH_ALEN);
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * ============================================================================== ARP packets ===================================================================================
 */

INT ArpRequest(IpV4Addr Ip, NetworkDevice *Dev) {
    NetworkBuf *Buf = NetworkAllocBuf(sizeof(ArpHeader));
    if (!Buf) return -1;
    
    ArpHeader *Arp = (ArpHeader*)Buf->Data;
    MemSet(Arp, 0, sizeof(ArpHeader));
    
    Arp->Hrd = Htons(ARP_HRD_ETHERNET);
    Arp->Pro = Htons(ARP_PRO_IP);
    Arp->Hln = ARP_HLEN_ETHERNET;
    Arp->Pln = ARP_PLEN_IP;
    Arp->Op = Htons(ARP_OP_REQUEST);
    
    MemCpy(Arp->Sha, Dev->MacAddr, NETWORK_ETH_ALEN);
    Arp->Spa = ((IpV4Addr*)Dev->IpAddr)->Addr;
    
    MemSet(Arp->Tha, 0, NETWORK_ETH_ALEN);
    Arp->Tpa = Ip.Addr;
    
    Buf->Len = sizeof(ArpHeader);
    
    return EthernetOutput(Buf, Dev, (UINT8*)ETH_MAC_BROADCAST, ETH_P_ARP);
}

NOPTR ArpInput(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (Buf->Len < sizeof(ArpHeader)) return;
    
    ArpHeader *Arp = (ArpHeader*)Buf->Data;
    
    if (Ntohs(Arp->Hrd) != ARP_HRD_ETHERNET) return;
    if (Ntohs(Arp->Pro) != ARP_PRO_IP) return;
    if (Arp->Hln != ARP_HLEN_ETHERNET || Arp->Pln != ARP_PLEN_IP) return;
    
    IpV4Addr SenderIp = { .Addr = Arp->Spa };
    IpV4Addr TargetIp = { .Addr = Arp->Tpa };
    IpV4Addr MyIp = { .Addr = ((IpV4Addr*)Dev->IpAddr)->Addr };
    
    if (SenderIp.Addr != 0) {
        ArpCacheAdd(SenderIp, Arp->Sha);
    }
    
    UINT16 Op = Ntohs(Arp->Op);
    
    switch (Op) {
        case ARP_OP_REQUEST:
            if (TargetIp.Addr == MyIp.Addr) {
                NetworkBuf *Reply = NetworkAllocBuf(sizeof(ArpHeader));
                if (!Reply) break;
                
                ArpHeader *ReplyArp = (ArpHeader*)Reply->Data;
                MemCpy(ReplyArp, Arp, sizeof(ArpHeader));
                
                ReplyArp->Op = Htons(ARP_OP_REPLY);
                MemCpy(ReplyArp->Tha, Arp->Sha, NETWORK_ETH_ALEN);
                ReplyArp->Tpa = Arp->Spa;
                MemCpy(ReplyArp->Sha, Dev->MacAddr, NETWORK_ETH_ALEN);
                ReplyArp->Spa = MyIp.Addr;
                
                Reply->Len = sizeof(ArpHeader);
                EthernetOutput(Reply, Dev, Arp->Sha, ETH_P_ARP);
            }
            break;
            
        case ARP_OP_REPLY:
            break;
    }
    
    (NOPTR)Dev;
    return;
}

/*
 * =============================================================================== Resolution with timeout ================================================================================
 */

INT ArpResolve(NetworkDevice *Dev, IpV4Addr Ip, UINT8 *Mac, UINT32 TimeoutMs) {
    UINT64 Start;
    UINT64 TimeoutTicks;
    
    if (ArpCacheLookup(Ip, Mac)) {
        return 0;
    }
    
    ArpRequest(Ip, Dev);
    
    Start = TimerTicks();
    TimeoutTicks = TimerMsToTicks(TimeoutMs);
    
    while ((TimerTicks() - Start) < TimeoutTicks) {
        NetworkPollDevices();
        if (ArpCacheLookup(Ip, Mac)) {
            return 0;
        }
        TimerSleep(1);
    }
    
    if (ArpCacheLookup(Ip, Mac)) {
        return 0;
    }
    
    return -1;
}

/*
 * =============================================================================== Initialization ======================================================================================
 */

NOPTR ArpInit(NOPTR) {
    MemSet(ArpCache, 0, sizeof(ArpCache));
    MemSet(ArpPendingRequests, 0, sizeof(ArpPendingRequests));
}
