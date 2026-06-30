#include <Network/ICmp.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Memory/Allocator.h>
#include <Kernel/Return.h>
#include <Console.h>

BOOL ICmpReady = FALSE;

/*
 * ============================================================================
 * Global variables for ping
 * ============================================================================
 */

typedef struct PingRequest {
    UINT16 Id;
    UINT16 Seq;
    UINT64 SentTimeUs;
    UINT32 Remaining;
    UINT32 Total;
    UINT32 TimeoutMs;
    PingStats *Stats;
    PingCallback Callback;
    NOPTR *Arg;
    BOOL Active;
    struct PingRequest *Next;
} PingRequest;

static PingRequest *GPingRequests = NULLPTR;
static UINT16 GNextPingId = 0;
static BOOL GICmpInitialized = FALSE;

/*
 * ============================================================================
 * Utility functions
 * ============================================================================
 */

const CHAR* ICmpTypeString(UINT8 Type) {
    switch (Type) {
        case ICMP_TYPE_ECHO_REPLY:      return "Echo Reply";
        case ICMP_TYPE_DEST_UNREACH:    return "Destination Unreachable";
        case ICMP_TYPE_SOURCE_QUENCH:   return "Source Quench";
        case ICMP_TYPE_REDIRECT:        return "Redirect";
        case ICMP_TYPE_ECHO_REQUEST:    return "Echo Request";
        case ICMP_TYPE_TIME_EXCEEDED:   return "Time Exceeded";
        case ICMP_TYPE_PARAM_PROBLEM:   return "Parameter Problem";
        case ICMP_TYPE_TIMESTAMP:       return "Timestamp";
        case ICMP_TYPE_TIMESTAMP_REPLY: return "Timestamp Reply";
        default:                        return "Unknown";
    }
}

const CHAR* ICmpCodeString(UINT8 Type, UINT8 Code) {
    if (Type == ICMP_TYPE_DEST_UNREACH) {
        switch (Code) {
            case ICMP_CODE_NET_UNREACH:   return "Network Unreachable";
            case ICMP_CODE_HOST_UNREACH:  return "Host Unreachable";
            case ICMP_CODE_PROT_UNREACH:  return "Protocol Unreachable";
            case ICMP_CODE_PORT_UNREACH:  return "Port Unreachable";
            case ICMP_CODE_FRAG_NEEDED:   return "Fragmentation Needed";
            default:                      return "Unknown";
        }
    }
    if (Type == ICMP_TYPE_TIME_EXCEEDED) {
        switch (Code) {
            case ICMP_CODE_TTL_EXCEEDED:  return "TTL Exceeded";
            case ICMP_CODE_REASSEMBLY:    return "Fragment Reassembly Time Exceeded";
            default:                      return "Unknown";
        }
    }
    return "";
}

/*
 * ============================================================================
 * ICMP Checksum (RFC 1071)
 * ============================================================================
 */

UINT16 ICmpChecksum(UINT16 *Data, UINT32 Len) {
    UINT32 Sum = 0;
    
    while (Len > 1) {
        Sum += *Data++;
        Len -= 2;
    }
    
    if (Len) {
        Sum += *(UINT8*)Data;
    }
    
    while (Sum >> 16) {
        Sum = (Sum & 0xFFFF) + (Sum >> 16);
    }
    
    return ~Sum;
}

/*
 * ============================================================================
 * Send ICMP Echo Request
 * ============================================================================
 */

static INT ICmpSendEchoRequest(IpV4Addr Dest, IpV4Addr Src, UINT16 Id, UINT16 Seq) {
    NetworkBuf *Buf = NetworkAllocBuf(sizeof(ICmpHeader) + ICMP_DATA_SIZE);
    if (!Buf) RETURN(NO_MEMORY);
    
    ICmpHeader *ICmp = (ICmpHeader*)Buf->Data;
    MemSet(ICmp, 0, sizeof(ICmpHeader));
    	
    ICmp->Type = ICMP_TYPE_ECHO_REQUEST;
    ICmp->Code = 0;
    ICmp->Id = Htons(Id);
    ICmp->Sequence = Htons(Seq);
    
    UINT64 NowUs = TimerTicks() * 1000000ULL / TimerFreq();
    UINT8 *Data = (UINT8*)ICmp + sizeof(ICmpHeader);
    *(UINT64*)Data = NowUs;
    
    Buf->Len = sizeof(ICmpHeader) + ICMP_DATA_SIZE;
    
    ICmp->Checksum = 0;
    ICmp->Checksum = ICmpChecksum((UINT16*)ICmp, Buf->Len);
    
    return IpV4Output(Buf, Dest, Src, IPV4_PROTO_ICMP);
}

/*
 * ============================================================================
 * Handle ping response
 * ============================================================================
 */

static NOPTR ICmpHandleEchoReply(IpV4Addr Src, ICmpHeader *ICmp, UINT32 Len) {
    (NOPTR)Src;
    
    UINT16 Id = Ntohs(ICmp->Id);
    UINT16 Seq = Ntohs(ICmp->Sequence);
    UINT64 NowUs = TimerTicks() * 1000000ULL / TimerFreq();
    
    PingRequest *Req = GPingRequests;
    while (Req) {
        if (Req->Id == Id && Req->Active) {
            UINT64 SentUs = 0;
            if (Len >= sizeof(ICmpHeader) + 8) {
                SentUs = *(UINT64*)((UINT8*)ICmp + sizeof(ICmpHeader));
            }
            
            UINT64 RttUs = NowUs - SentUs;
            
            if (Req->Stats) {
                Req->Stats->Received++;
                if (RttUs < Req->Stats->MinRttUs || Req->Stats->MinRttUs == 0) {
                    Req->Stats->MinRttUs = RttUs;
                }
                if (RttUs > Req->Stats->MaxRttUs) {
                    Req->Stats->MaxRttUs = RttUs;
                }
                Req->Stats->TotalRttUs += RttUs;
            }
            break;
        }
        Req = Req->Next;
    }
}

/*
 * ============================================================================
 * ICMP Input (incoming packets)
 * ============================================================================
 */

NOPTR ICmpInput(NetworkBuf *Buf, NetworkDevice *Dev) {
    if (Buf->Len < sizeof(ICmpHeader)) return;

    IpV4Addr Src = { .Addr = Buf->IpSrc };
    IpV4Addr Dst = { .Addr = Buf->IpDst };
    ICmpHeader *ICmp = (ICmpHeader *)Buf->Data;
    
    UINT16 Checksum = ICmp->Checksum;
    ICmp->Checksum = 0;
    if (ICmpChecksum((UINT16 *)ICmp, Buf->Len) != Checksum) {
        return;
    }
    ICmp->Checksum = Checksum;
    
    switch (ICmp->Type) {
        case ICMP_TYPE_ECHO_REQUEST:
            ICmp->Type = ICMP_TYPE_ECHO_REPLY;
            ICmp->Checksum = 0;
            ICmp->Checksum = ICmpChecksum((UINT16 *)ICmp, Buf->Len);
            IpV4Output(Buf, Src, Dst, IPV4_PROTO_ICMP);
            break;
            
        case ICMP_TYPE_ECHO_REPLY:
            ICmpHandleEchoReply(Src, ICmp, Buf->Len);
            break;
            
        case ICMP_TYPE_DEST_UNREACH:
        case ICMP_TYPE_TIME_EXCEEDED:
            break;
            
        default:
            break;
    }
    
    (NOPTR)Dev;
    return;
}

/*
 * ============================================================================
 * ICMP Output
 * ============================================================================
 */

INT ICmpOutput(NetworkBuf *Buf, IpV4Addr Dest, IpV4Addr Src, UINT8 Type, UINT8 Code) {
    ICmpHeader *ICmp = (ICmpHeader*)Buf->Data;
    ICmp->Type = Type;
    ICmp->Code = Code;
    ICmp->Checksum = 0;
    ICmp->Checksum = ICmpChecksum((UINT16*)ICmp, Buf->Len);
    return IpV4Output(Buf, Dest, Src, IPV4_PROTO_ICMP);
}

/*
 * ============================================================================
 * Ping Timer (called periodically from timer handler)
 * ============================================================================
 */

static NOPTR ICmpPingTimer(NOPTR) {
    UINT64 NowUs = TimerTicks() * 1000000ULL / TimerFreq();
    PingRequest *Req = GPingRequests;
    PingRequest *Prev = NULLPTR;
    
    while (Req) {
        if (!Req->Active) {
            Prev = Req;
            Req = Req->Next;
            continue;
        }
        
        if (Req->SentTimeUs > 0 && (NowUs - Req->SentTimeUs) > Req->TimeoutMs * 1000) {
            Req->Stats->Lost++;
            Req->SentTimeUs = 0;
            Req->Remaining--;
            
            if (Req->Remaining > 0) {
                Req->Seq++;
                ICmpSendEchoRequest((IpV4Addr){0}, (IpV4Addr){0}, Req->Id, Req->Seq);
                Req->SentTimeUs = NowUs;
            }
        }
        
        if (Req->Remaining == 0) {
            if (Req->Stats && Req->Stats->Received > 0) {
                Req->Stats->Lost = Req->Total - Req->Stats->Received;
                Req->Stats->AvgRttUs = Req->Stats->TotalRttUs / Req->Stats->Received;
            }
            
            if (Req->Callback) {
                Req->Callback(Req->Stats, Req->Arg);
            }
            
            if (Prev) {
                Prev->Next = Req->Next;
            } else {
                GPingRequests = Req->Next;
            }
            MemoryFree(Req);
            Req = Prev ? Prev->Next : GPingRequests;
            continue;
        }
        
        Prev = Req;
        Req = Req->Next;
    }
}

/*
 * ============================================================================
 * Ping (blocking)
 * ============================================================================
 */

INT ICmpPing(IpV4Addr Dest, UINT32 Count, UINT32 TimeoutMs, PingStats *Stats) {
    CHAR DestStr[16];
    UINT64 MinRttUs;
    UINT64 MaxRttUs;
    UINT64 TotalRttUs;
    UINT32 Sent;
    UINT32 Received;
    UINT32 Lost;
    UINT64 AvgRttUs;
    
    if (!Stats || Count == 0) RETURN(INCORRECT_VALUE);
    
    IpV4NTop(Dest, DestStr, 16);
    
    MemSet(Stats, 0, sizeof(PingStats));
    Stats->MinRttUs = UINT64MAX;
    
    UINT16 Id = GNextPingId++;
    Sent = 0;
    Received = 0;
    TotalRttUs = 0;
    MinRttUs = UINT64MAX;
    MaxRttUs = 0;
    
    for (UINT32 I = 0; I < Count; I++) {
        UINT64 StartUs = TimerTicks() * 1000000ULL / TimerFreq();
        UINT16 Seq = (UINT16)(I + 1);
        
        if (ICmpSendEchoRequest(Dest, (IpV4Addr){0}, Id, Seq) == SUCCESS) {
            Sent++;
        } else {
            continue;
        }
        
        BOOL ReceivedResponse = FALSE;
        UINT64 WaitStart = TimerTicks() * 1000000ULL / TimerFreq();
        UINT64 TimeoutUs = (UINT64)TimeoutMs * 1000;
        
        while (!ReceivedResponse && (TimerTicks() * 1000000ULL / TimerFreq() - WaitStart) < TimeoutUs) {
            PingRequest *Req = GPingRequests;
            while (Req) {
                if (Req->Id == Id && Req->Seq == Seq && Req->Stats->Received > Received) {
                    ReceivedResponse = TRUE;
                    break;
                }
                Req = Req->Next;
            }
            TimerMdelay(1);
        }
        
        if (ReceivedResponse) {
            Received++;
            UINT64 RttUs = (TimerTicks() * 1000000ULL / TimerFreq()) - StartUs;
            if (RttUs < MinRttUs) MinRttUs = RttUs;
            if (RttUs > MaxRttUs) MaxRttUs = RttUs;
            TotalRttUs += RttUs;
            
            UINT32 RttMs = (UINT32)(RttUs / 1000);
            ConsolePrint("%d bytes from %s: icmp_seq=%u time=%u.%03u ms\n",
                        ICMP_DATA_SIZE + 8, DestStr, Seq,
                        RttMs / 1000, RttMs % 1000);
        } else {
            ConsolePrint("Request timeout for icmp_seq=%u\n", Seq);
        }
    }
    
    Lost = Sent - Received;
    AvgRttUs = (Received > 0) ? TotalRttUs / Received : 0;
    
    Stats->Sent = Sent;
    Stats->Received = Received;
    Stats->Lost = Lost;
    Stats->MinRttUs = (MinRttUs == UINT64MAX) ? 0 : MinRttUs;
    Stats->MaxRttUs = MaxRttUs;
    Stats->TotalRttUs = TotalRttUs;
    Stats->AvgRttUs = AvgRttUs;
    
    ConsolePrint("\n--- %s ping statistics ---\n", DestStr);
    ConsolePrint("%u packets transmitted, %u received, %u%% packet loss\n",
                Sent, Received, (Sent > 0) ? (Lost * 100 / Sent) : 0);
    if (Received > 0) {
        ConsolePrint("rtt min/avg/max = %llu.%03llu/%llu.%03llu/%llu.%03llu ms\n",
                    MinRttUs / 1000, MinRttUs % 1000,
                    AvgRttUs / 1000, AvgRttUs % 1000,
                    MaxRttUs / 1000, MaxRttUs % 1000);
    }
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Ping (async)
 * ============================================================================
 */

INT ICmpPingAsync(IpV4Addr Dest, UINT32 Count, UINT32 TimeoutMs,
                  PingCallback Callback, NOPTR *Arg) {
    PingStats *Stats;
    PingRequest *Req;
    
    if (!Callback || Count == 0) RETURN(INCORRECT_VALUE);
    
    Stats = (PingStats*)MemoryAllocate(sizeof(PingStats));
    if (!Stats) RETURN(NO_MEMORY);
    MemSet(Stats, 0, sizeof(PingStats));
    Stats->MinRttUs = UINT64MAX;
    
    Req = (PingRequest*)MemoryAllocate(sizeof(PingRequest));
    if (!Req) {
        MemoryFree(Stats);
        RETURN(NO_MEMORY);
    }
    
    UINT16 Id = GNextPingId++;
    
    Req->Id = Id;
    Req->Seq = 1;
    Req->Remaining = Count;
    Req->Total = Count;
    Req->TimeoutMs = TimeoutMs;
    Req->Stats = Stats;
    Req->Callback = Callback;
    Req->Arg = Arg;
    Req->Active = TRUE;
    Req->Next = GPingRequests;
    Req->SentTimeUs = TimerTicks() * 1000000ULL / TimerFreq();
    
    GPingRequests = Req;
    
    ICmpSendEchoRequest(Dest, (IpV4Addr){0}, Id, 1);
    
    RETURN(SUCCESS);
}

NOPTR ICmpPingCancel(NOPTR) {
    PingRequest *Req = GPingRequests;
    while (Req) {
        Req->Active = FALSE;
        Req = Req->Next;
    }
}

/*
 * ============================================================================
 * Timer handler (called from system timer)
 * ============================================================================
 */

NOPTR ICmpTimerHandler(NOPTR) {
    if (!GICmpInitialized) return;
    ICmpPingTimer();
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

INT ICmpInit(NOPTR) {
    GPingRequests = NULLPTR;
    GNextPingId = 0x1234;
    GICmpInitialized = TRUE;
    
    IpV4RegisterProtocol(IPV4_PROTO_ICMP, ICmpInput);
    
    ICmpReady = TRUE;
    
    RETURN(SUCCESS);
}