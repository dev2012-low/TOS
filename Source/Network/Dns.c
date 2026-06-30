#include <Network/Dns.h>
#include <Network/Udp.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Memory/Allocator.h>
#include <Crypto/Rng.h>
#include <Kernel/Return.h>
#include <Network/IpV4.h>

/*
 * ============================================================================
 * Global variables
 * ============================================================================
 */

static IpV4Addr GNameserver = { .Addr = 0x08080808 };  /* 8.8.8.8 (Google) */
static DnsCacheEntry *GCache = NULLPTR;
static DnsQuery *GPendingQueries = NULLPTR;
static UINT16 GNextQueryId = 0;

static INT ParseIpV4(const CHAR *Str, UINT32 *Ip) {
    UINT32 Octets[4] = {0, 0, 0, 0};
    UINT32 *Current = &Octets[0];
    UINT32 Num = 0;
    BOOL HaveDigit = FALSE;
    
    while (*Str) {
        if (*Str >= '0' && *Str <= '9') {
            Num = Num * 10 + (*Str - '0');
            HaveDigit = TRUE;
        } else if (*Str == '.') {
            if (!HaveDigit || Current >= &Octets[3]) return -1;
            *Current++ = Num;
            Num = 0;
            HaveDigit = FALSE;
        } else {
            return -1;
        }
        Str++;
    }
    
    if (!HaveDigit || Current != &Octets[3]) return -1;
    *Current = Num;
    
    if (Octets[0] > 255 || Octets[1] > 255 || Octets[2] > 255 || Octets[3] > 255) {
        return -1;
    }
    
    *Ip = (UINT32)(Octets[0] << 0) | (Octets[1] << 8) | (Octets[2] << 16) | (Octets[3] << 24);
    return 0;
}

/*
 * ============================================================================
 * Domain name encoding (RFC 1035)
 * ============================================================================
 */

NOPTR DnsDomainToDns(const CHAR *Domain, UINT8 *Out) {
    UINT32 I = 0;
    UINT32 J = 0;
    
    while (Domain[I] != '\0') {
        UINT32 LabelStart = I;
        while (Domain[I] != '.' && Domain[I] != '\0') {
            I++;
        }
        
        UINT32 LabelLen = I - LabelStart;
        Out[J++] = (UINT8)LabelLen;
        
        for (UINT32 K = 0; K < LabelLen; K++) {
            Out[J++] = (UINT8)Domain[LabelStart + K];
        }
        
        if (Domain[I] == '.') {
            I++;
        }
    }
    
    Out[J] = 0;  /* Root label */
}

NOPTR DnsDnsToDomain(const UINT8 *Packet, UINT32 *Pos, CHAR *Out, UINT32 OutLen) {
    UINT32 I = 0;
    UINT32 LoopDetection = 0;
    const UINT8 *P = Packet + *Pos;
    
    while (*P != 0 && LoopDetection < 1000) {
        if ((*P & 0xC0) == 0xC0) {
            /* Pointer to another location in packet */
            UINT32 Offset = ((*P & 0x3F) << 8) | *(P + 1);
            UINT32 TempPos = Offset;
            CHAR TempBuf[DNS_MAX_NAME];
            
            DnsDnsToDomain(Packet, &TempPos, TempBuf, DNS_MAX_NAME);
            
            if (I + StrLen(TempBuf) + 1 < OutLen) {
                if (I > 0) Out[I++] = '.';
                StrCpy(Out + I, TempBuf);
                I += StrLen(TempBuf);
            }
            
            *Pos += 2;
            return;
        }
        
        UINT32 Len = *P;
        P++;
        
        for (UINT32 J = 0; J < Len; J++) {
            if (I < OutLen - 1) {
                Out[I++] = (CHAR)*P;
            }
            P++;
        }
        
        if (*P != 0) {
            if (I < OutLen - 1) {
                Out[I++] = '.';
            }
        }
        
        LoopDetection++;
    }
    
    Out[I] = '\0';
    *Pos += (P - (Packet + *Pos)) + 1;  /* +1 for root label */
}

/*
 * ============================================================================
 * DNS message building / parsing
 * ============================================================================
 */

static UINT16 DnsBuildQuery(UINT16 Id, const CHAR *Domain, UINT8 *Buf, UINT32 BufLen) {
    DnsHeader *Hdr = (DnsHeader*)Buf;
    UINT32 Pos = sizeof(DnsHeader);
    UINT8 *NamePtr = Buf + Pos;
    
    MemSet(Hdr, 0, sizeof(DnsHeader));
    
    Hdr->Id = Htons(Id);
    Hdr->Flags = Htons(DNS_FLAG_RD);  /* Recursion Desired */
    Hdr->QdCount = Htons(1);
    
    DnsDomainToDns(Domain, NamePtr);
    Pos += StrLen(Domain) + 2;  /* +2 for length bytes and root */
    
    /* QTYPE = A (1), QCLASS = IN (1) */
    *(UINT16*)(Buf + Pos) = Htons(1);
    Pos += 2;
    *(UINT16*)(Buf + Pos) = Htons(1);
    Pos += 2;
    
    return Pos;
}

static INT DnsParseResponse(const UINT8 *Buf, UINT32 Len, IpV4Addr *OutIp) {
    DnsHeader *Hdr = (DnsHeader*)Buf;
    UINT32 Pos = sizeof(DnsHeader);
    
    UINT16 Flags = Ntohs(Hdr->Flags);
    UINT16 RCode = Flags & DNS_FLAG_RCODE_MASK;
    
    if (RCode != DNS_RCODE_NOERROR) {
        return -RCode;
    }
    
    UINT16 QdCount = Ntohs(Hdr->QdCount);
    UINT16 AnCount = Ntohs(Hdr->AnCount);
    
    /* Skip questions */
    for (UINT16 I = 0; I < QdCount; I++) {
        CHAR Name[DNS_MAX_NAME];
        DnsDnsToDomain(Buf, &Pos, Name, DNS_MAX_NAME);
        Pos += 4;  /* QTYPE + QCLASS */
    }
    
    /* Parse answers */
    for (UINT16 I = 0; I < AnCount; I++) {
        CHAR Name[DNS_MAX_NAME];
        DnsDnsToDomain(Buf, &Pos, Name, DNS_MAX_NAME);
        
        UINT16 Type = Ntohs(*(UINT16*)(Buf + Pos));
        Pos += 2;
        UINT16 Class = Ntohs(*(UINT16*)(Buf + Pos));
        Pos += 2;
        UINT32 Ttl = Ntohl(*(UINT32*)(Buf + Pos));
        Pos += 4;
        UINT16 RdLength = Ntohs(*(UINT16*)(Buf + Pos));
        Pos += 2;
        
        (NOPTR)Class;
        (NOPTR)Ttl;
        
        if (Type == 1 && RdLength == 4) {  /* A record */
            OutIp->Addr = *(UINT32*)(Buf + Pos);
            RETURN(SUCCESS);
        }
        
        Pos += RdLength;
    }
    
    RETURN(GENERAL_ERROR);  /* No A record found */
}

/*
 * ============================================================================
 * UDP input handler (async responses)
 * ============================================================================
 */

static NOPTR DnsUdpInput(NetworkBuf *Buf, IpV4Addr Src, IpV4Addr Dst,
                         UINT16 SrcPort, UINT16 DstPort) {
    (NOPTR)Src;
    (NOPTR)Dst;
    (NOPTR)SrcPort;
    (NOPTR)DstPort;
    
    if (Buf->Len < sizeof(DnsHeader)) return;
    
    DnsHeader *Hdr = (DnsHeader*)Buf->Data;
    UINT16 Id = Ntohs(Hdr->Id);
    
    /* Find pending query */
    DnsQuery *Prev = NULLPTR;
    DnsQuery *Q = GPendingQueries;
    
    while (Q) {
        if (Q->Id == Id) {
            IpV4Addr Ip;
            
            if (DnsParseResponse(Buf->Data, Buf->Len, &Ip) == 0) {
                /* Add to cache */
                DnsCacheEntry *Entry = (DnsCacheEntry*)MemoryAllocate(sizeof(DnsCacheEntry));
                if (Entry) {
                    StrCpy(Entry->Domain, Q->Domain);
                    Entry->Ip = Ip;
                    Entry->Expires = TimerTicks() + TimerMsToTicks(DNS_CACHE_TTL * 1000);
                    Entry->Valid = TRUE;
                    Entry->Next = GCache;
                    GCache = Entry;
                }
                
                if (Q->Callback) Q->Callback(Ip, Q->Arg);
            } else {
                /* No answer */
                IpV4Addr Zero = { .Addr = 0 };
                if (Q->Callback) Q->Callback(Zero, Q->Arg);
            }
            
            /* Remove from pending list */
            if (Prev) {
                Prev->Next = Q->Next;
            } else {
                GPendingQueries = Q->Next;
            }
            
            MemoryFree(Q);
            break;
        }
        Prev = Q;
        Q = Q->Next;
    }
}

/*
 * ============================================================================
 * Cache management
 * ============================================================================
 */

static IpV4Addr DnsCacheLookup(const CHAR *Domain) {
    UINT64 Now = TimerTicks();
    DnsCacheEntry *Entry = GCache;
    
    while (Entry) {
        if (Entry->Valid && Now < Entry->Expires && StrCmp(Entry->Domain, Domain) == 0) {
            return Entry->Ip;
        }
        Entry = Entry->Next;
    }
    
    IpV4Addr Zero = { .Addr = 0 };
    return Zero;
}

NOPTR DnsCacheClear(NOPTR) {
    DnsCacheEntry *Entry = GCache;
    
    while (Entry) {
        DnsCacheEntry *Next = Entry->Next;
        MemoryFree(Entry);
        Entry = Next;
    }
    
    GCache = NULLPTR;
}

/*
 * ============================================================================
 * DNS query sending
 * ============================================================================
 */

static INT DnsSendQuery(UINT16 Id, const CHAR *Domain) {
    UINT8 Packet[DNS_MAX_PACKET];
    UINT32 Len = DnsBuildQuery(Id, Domain, Packet, DNS_MAX_PACKET);
    
    NetworkBuf *Buf = NetworkAllocBuf(Len);
    if (!Buf) RETURN(NO_OBJECT);
    
    MemCpy(Buf->Data, Packet, Len);
    Buf->Len = Len;
    
    return UdpOutput(Buf, GNameserver, (IpV4Addr){0}, DNS_PORT, 0);
}

/*
 * ============================================================================
 * Synchronous resolution
 * ============================================================================
 */

INT DnsResolve(const CHAR *Domain, IpV4Addr *OutIp) {
    if (!Domain || !OutIp) RETURN(NO_OBJECT);
    
    UINT32 IpAddr;
    if (ParseIpV4(Domain, &IpAddr) == 0) {
        OutIp->Addr = IpAddr;
        RETURN(SUCCESS);
    }
    
    IpV4Addr Cached = DnsCacheLookup(Domain);
    if (Cached.Addr != 0) {
        *OutIp = Cached;
        RETURN(SUCCESS);
    }
    
    DnsQuery *Q = (DnsQuery *)MemoryAllocate(sizeof(DnsQuery));
    if (!Q) RETURN(NO_OBJECT);
    
    MemSet(Q, 0, sizeof(DnsQuery));
    Q->Id = GNextQueryId++;
    StrnCpy(Q->Domain, Domain, DNS_MAX_NAME - 1);
    Q->StartTime = TimerTicks();
    Q->Active = TRUE;
    Q->Next = GPendingQueries;
    GPendingQueries = Q;
    
    if (DnsSendQuery(Q->Id, Domain) != 0) {
        GPendingQueries = Q->Next;
        MemoryFree(Q);
        RETURN(INCORRECT_VALUE);
    }
    
    UINT64 Start = TimerTicks();
    UINT64 TimeoutTicks = TimerMsToTicks(DNS_TIMEOUT_MS);
    UINT32 Retry = 0;
    
    while (TimerTicks() - Start < TimeoutTicks) {
        Cached = DnsCacheLookup(Domain);
        if (Cached.Addr != 0) {
            *OutIp = Cached;
            RETURN(SUCCESS);
        }
        
        DnsQuery *Check = GPendingQueries;
        BOOL StillPending = FALSE;
        while (Check) {
            if (Check == Q) {
                StillPending = TRUE;
                break;
            }
            Check = Check->Next;
        }
        if (!StillPending) {
            break;
        }
        
        if (TimerTicks() - Start > TimerMsToTicks(DNS_TIMEOUT_MS / (DNS_MAX_RETRIES + 1)) * (Retry + 1) &&
            Retry < DNS_MAX_RETRIES) {
            Retry++;
            Q->Retries = Retry;
            DnsSendQuery(Q->Id, Domain);
        }
        
        TimerSleep(10);
    }
    
    if (Q == GPendingQueries) {
        GPendingQueries = Q->Next;
    } else {
        DnsQuery *Prev = GPendingQueries;
        while (Prev && Prev->Next != Q) {
            Prev = Prev->Next;
        }
        if (Prev) {
            Prev->Next = Q->Next;
        }
    }
    MemoryFree(Q);
    
    Cached = DnsCacheLookup(Domain);
    if (Cached.Addr != 0) {
        *OutIp = Cached;
        RETURN(SUCCESS);
    }
    
    RETURN(GENERAL_ERROR);
}

NOPTR DnsTimerHandler(NOPTR) {
    UINT64 Now = TimerTicks();
    DnsQuery *Prev = NULLPTR;
    DnsQuery *Q = GPendingQueries;
    UINT64 TimeoutTicks = TimerMsToTicks(DNS_TIMEOUT_MS);
    
    while (Q) {
        DnsQuery *Next = Q->Next;
        
        if (Now - Q->StartTime >= TimeoutTicks) {
            IpV4Addr Zero = { .Addr = 0 };
            if (Q->Callback) {
                Q->Callback(Zero, Q->Arg);
            }
            if (Prev) {
                Prev->Next = Next;
            } else {
                GPendingQueries = Next;
            }
            MemoryFree(Q);
        } else if (Q->Active && Q->Retries < DNS_MAX_RETRIES &&
                   Now - Q->StartTime > TimeoutTicks * (Q->Retries + 1) / (DNS_MAX_RETRIES + 1)) {
            Q->Retries++;
            DnsSendQuery(Q->Id, Q->Domain);
            Prev = Q;
        } else {
            Prev = Q;
        }
        
        Q = Next;
    }
}

/*
 * ============================================================================
 * Asynchronous resolution
 * ============================================================================
 */

INT DnsResolveAsync(const CHAR *Domain, NOPTR (*Callback)(IpV4Addr Ip, NOPTR *Arg), NOPTR *Arg) {
    if (!Domain || !Callback) RETURN(NO_OBJECT);
    
    /* Check cache */
    IpV4Addr Cached = DnsCacheLookup(Domain);
    if (Cached.Addr != 0) {
        Callback(Cached, Arg);
        RETURN(SUCCESS);
    }
    
    /* Create pending query */
    DnsQuery *Q = (DnsQuery*)MemoryAllocate(sizeof(DnsQuery));
    if (!Q) RETURN(NO_OBJECT);
    
    MemSet(Q, 0, sizeof(DnsQuery));
    Q->Id = GNextQueryId++;
    StrnCpy(Q->Domain, Domain, DNS_MAX_NAME - 1);
    Q->Callback = Callback;
    Q->Arg = Arg;
    Q->StartTime = TimerTicks();
    Q->Active = TRUE;
    
    Q->Next = GPendingQueries;
    GPendingQueries = Q;
    
    if (DnsSendQuery(Q->Id, Domain) != 0) {
        /* Failed to send */
        GPendingQueries = Q->Next;
        MemoryFree(Q);
        RETURN(INCORRECT_VALUE);
    }
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

NOPTR DnsSetNameserver(IpV4Addr Ns) {
    GNameserver = Ns;
    CHAR IpStr[16];
    IpV4NTop(Ns, IpStr, 16);
}

const CHAR *DnsRCodeString(UINT16 RCode) {
    switch (RCode) {
        case DNS_RCODE_NOERROR:  return "No error";
        case DNS_RCODE_FORMERR:  return "Format error";
        case DNS_RCODE_SERVFAIL: return "Server failure";
        case DNS_RCODE_NXDOMAIN: return "Non-existent domain";
        case DNS_RCODE_NOTIMP:   return "Not implemented";
        case DNS_RCODE_REFUSED:  return "Refused";
        default:                 return "Unknown";
    }
}

NOPTR DnsInit(NOPTR) {
    GCache = NULLPTR;
    GPendingQueries = NULLPTR;
    GNextQueryId = (UINT16)RngGetRandomBytes((UINT8*)&GNextQueryId, 2);
    
    /* Register UDP handler */
    UdpRegisterHandler(DNS_PORT, DnsUdpInput);
    
    /* Use Google DNS by default */
    GNameserver.Addr = 0x08080808;  /* 8.8.8.8 */
}
