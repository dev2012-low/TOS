#include <Network/Tcp.h>
#include <Network/IpV4.h>
#include <Lib/String.h>
#include <Lib/Math.h>
#include <Memory/Allocator.h>
#include <Time/Timer.h>
#include <Kernel/Return.h>
#include <Console.h>

/*
 * ============================================================================
 * Global variables
 * ============================================================================
 */

static ListHead GTcpSockets;
static ListHead GTcpListenSockets;
static UINT32 GTcpIss = 0x12345678;
static UINT16 GTcpEphemeralPort = 49152;
static BOOL GTcpInitialized = FALSE;
static BOOL GTcpReady = FALSE;
BOOL TcpReady = FALSE;

/*
 * ============================================================================
 * TCP Checksum (RFC 793)
 * ============================================================================
 */

static UINT16 TcpChecksumInternal(TcpHeader *Tcp, UINT32 Len, UINT32 Src, UINT32 Dst) {
    struct {
        UINT32 Src;
        UINT32 Dst;
        UINT8 Zero;
        UINT8 Proto;
        UINT16 TcpLen;
    } ATTRIBUTE(packed) Pseudo = {
        .Src = Src,
        .Dst = Dst,
        .Zero = 0,
        .Proto = IPV4_PROTO_TCP,
        .TcpLen = Htons((UINT16)Len),
    };
    
    UINT32 Sum = 0;
    UINT16 *Ptr;
    UINT32 I;
    
    /* Pseudo header */
    Ptr = (UINT16*)&Pseudo;
    for (I = 0; I < sizeof(Pseudo) / 2; I++) {
        Sum += Ptr[I];
    }
    
    /* TCP header + data */
    Ptr = (UINT16*)Tcp;
    for (I = 0; I < Len / 2; I++) {
        Sum += Ptr[I];
    }
    if (Len & 1) {
        Sum += *(UINT8*)((UINT8*)Tcp + Len - 1);
    }
    
    while (Sum >> 16) {
        Sum = (Sum & 0xFFFF) + (Sum >> 16);
    }
    
    return ~Sum;
}

UINT16 TcpChecksum(TcpHeader *Tcp, UINT32 Len, IpV4Addr Src, IpV4Addr Dst) {
    return TcpChecksumInternal(Tcp, Len, Src.Addr, Dst.Addr);
}

/*
 * ============================================================================
 * Socket management
 * ============================================================================
 */

static UINT16 TcpGetEphemeralPort(NOPTR) {
    UINT16 Port = GTcpEphemeralPort++;
    if (GTcpEphemeralPort > 65535) {
        GTcpEphemeralPort = 49152;
    }
    return Port;
}

static BOOL TcpPortInUse(UINT16 Port, IpV4Addr Addr) {
    ListHead *Pos;
    
    ListForEach(Pos, &GTcpSockets) {
        TcpSocket *Sock = ListEntry(Pos, TcpSocket, Node);
        if (Sock->LocalPort == Port && Sock->LocalAddr.Addr == Addr.Addr) {
            return TRUE;
        }
    }
    
    return FALSE;
}

TcpSocket *TcpSocketCreate(NOPTR) {
    TcpSocket *Sock;
    
    Sock = (TcpSocket*)MemoryAllocate(sizeof(TcpSocket));
    if (!Sock) return NULLPTR;
    
    MemSet(Sock, 0, sizeof(TcpSocket));
    
    Sock->State = TCP_STATE_CLOSED;
    Sock->RcvWnd = TCP_WINDOW_DEFAULT;
    Sock->SndWnd = TCP_WINDOW_DEFAULT;
    Sock->MssSend = TCP_MSS_DEFAULT;
    Sock->MssRecv = TCP_MSS_CALCULATED;
    Sock->Rto = TCP_RTO_INITIAL;
    Sock->Srtt = 0;
    Sock->Rttvar = 750;
    Sock->Cwnd = TCP_MSS_DEFAULT;
    Sock->Ssthresh = 65535;
    
    ListInit(&Sock->SendQueue);
    ListInit(&Sock->RetransQueue);
    ListInit(&Sock->RecvQueue);
    ListInit(&Sock->AcceptQueue);
    
    return Sock;
}

static NOPTR TcpSocketUnlink(TcpSocket *Sock) {
    if (Sock->Node.Next != NULLPTR || Sock->Node.Prev != NULLPTR) {
        ListDel(&Sock->Node);
    }
}

static NOPTR TcpSocketLink(TcpSocket *Sock) {
    if (Sock->Node.Next == NULLPTR && Sock->Node.Prev == NULLPTR) {
        ListAddTail(&GTcpSockets, &Sock->Node);
    }
}

NOPTR TcpSocketDestroy(TcpSocket *Sock) {
    ListHead *Pos;
    ListHead *Tmp;
    TcpSegment *Seg;
    TcpSocket *Accepted;
    
    if (!Sock) return;
    
    TcpSocketUnlink(Sock);
    
    /* Free send_queue */
    ListForEachSafe(Pos, Tmp, &Sock->SendQueue) {
        Seg = ListEntry(Pos, TcpSegment, Node);
        ListDel(&Seg->Node);
        if (Seg->Data) MemoryFree(Seg->Data);
        MemoryFree(Seg);
    }
    
    /* Free retrans_queue */
    ListForEachSafe(Pos, Tmp, &Sock->RetransQueue) {
        Seg = ListEntry(Pos, TcpSegment, Node);
        ListDel(&Seg->Node);
        if (Seg->Data) MemoryFree(Seg->Data);
        MemoryFree(Seg);
    }
    
    /* Free recv_queue */
    ListForEachSafe(Pos, Tmp, &Sock->RecvQueue) {
        Seg = ListEntry(Pos, TcpSegment, Node);
        ListDel(&Seg->Node);
        if (Seg->Data) MemoryFree(Seg->Data);
        MemoryFree(Seg);
    }
    
    /* Free accept_queue */
    ListForEachSafe(Pos, Tmp, &Sock->AcceptQueue) {
        Accepted = ListEntry(Pos, TcpSocket, Node);
        ListDel(&Accepted->Node);
        TcpSocketDestroy(Accepted);
    }
    
    MemoryFree(Sock);
}

/*
 * ============================================================================
 * TCP Segment sending
 * ============================================================================
 */

static INT TcpSendSegment(TcpSocket *Sock, UINT32 Seq, UINT32 Ack, UINT16 Flags,
                          const UINT8 *Data, UINT32 DataLen, BOOL Push) {
    NetworkBuf *Buf;
    TcpHeader *Tcp;
    UINT32 HeaderLen = 20;
    UINT32 TotalLen = HeaderLen + DataLen;
    UINT8 *Payload;
    
    (NOPTR)Push;
    
    Buf = NetworkAllocBuf(TotalLen + IPV4_HEADER_MIN + NETWORK_HEADROOM);
    if (!Buf) RETURN(NO_MEMORY);
    
    NetworkReserve(Buf, IPV4_HEADER_MIN + NETWORK_HEADROOM);
    
    Tcp = (TcpHeader*)Buf->Data;
    MemSet(Tcp, 0, HeaderLen);
    
    Tcp->SrcPort = Htons(Sock->LocalPort);
    Tcp->DstPort = Htons(Sock->RemotePort);
    Tcp->SeqNum = Htonl(Seq);
    Tcp->AckNum = Htonl(Ack);
    TCP_SET_DATA_OFFSET(Tcp, 5);
    TCP_SET_FLAGS(Tcp, Flags);
    Tcp->Window = Htons((UINT16)Sock->RcvWnd);
    
    if (Data && DataLen > 0) {
        Payload = (UINT8*)Tcp + HeaderLen;
        MemCpy(Payload, Data, DataLen);
    }
    
    Buf->Len = TotalLen;
    
    Tcp->Checksum = 0;
    Tcp->Checksum = TcpChecksum(Tcp, TotalLen, Sock->LocalAddr, Sock->RemoteAddr);
    
    return IpV4Output(Buf, Sock->RemoteAddr, Sock->LocalAddr, IPV4_PROTO_TCP);
}

/*
 * ============================================================================
 * Queue management
 * ============================================================================
 */

static NOPTR TcpQueueData(TcpSocket *Sock, const UINT8 *Data, UINT32 Len) {
    TcpSegment *Seg;
    
    Seg = (TcpSegment*)MemoryAllocate(sizeof(TcpSegment));
    if (!Seg) return;
    
    MemSet(Seg, 0, sizeof(TcpSegment));
    Seg->Seq = Sock->SndNxt;
    Seg->Len = Len;
    Seg->Data = (UINT8*)MemoryAllocate(Len);
    if (Seg->Data) {
        MemCpy(Seg->Data, Data, Len);
    }
    
    ListAddTail(&Sock->SendQueue, &Seg->Node);
    Sock->SendQueueLen += Len;
    Sock->SndNxt += Len;
}

static NOPTR TcpSendFromQueue(TcpSocket *Sock) {
    ListHead *Pos;
    ListHead *Tmp;
    TcpSegment *Seg;
    UINT32 Window;
    
    Window = (Sock->SndUna + Sock->SndWnd) - Sock->SndNxt;
    if (Window < Sock->MssSend) return;
    
    ListForEachSafe(Pos, Tmp, &Sock->SendQueue) {
        Seg = ListEntry(Pos, TcpSegment, Node);
        
        if (Seg->Len <= Window) {
            ListDel(&Seg->Node);
            Sock->SendQueueLen -= Seg->Len;
            
            Seg->SentTime = TimerTicks();
            Seg->RetransCount = 0;
            
            TcpSendSegment(Sock, Seg->Seq, Sock->RcvNxt,
                          TCP_FLAG_ACK | TCP_FLAG_PSH,
                          Seg->Data, Seg->Len, TRUE);
            
            ListAddTail(&Sock->RetransQueue, &Seg->Node);
            Sock->RetransQueueLen += Seg->Len;
            Window -= Seg->Len;
            
            Sock->LastSent = TimerTicks();
            
            if (!Sock->TimerActive) {
                Sock->TimerExpires = Sock->LastSent + TimerMsToTicks(Sock->Rto);
                Sock->TimerActive = TRUE;
            }
        } else {
            break;
        }
    }
}

/*
 * ============================================================================
 * RTT and Retransmission
 * ============================================================================
 */

static NOPTR TcpUpdateRtt(TcpSocket *Sock, UINT32 AckSeq) {
    ListHead *Pos;
    TcpSegment *Seg;
    UINT64 Now;
    UINT64 ElapsedUs;
    UINT32 RttMs;
    
    ListForEach(Pos, &Sock->RetransQueue) {
        Seg = ListEntry(Pos, TcpSegment, Node);
        if (Seg->Seq + Seg->Len == AckSeq) {
            Now = TimerTicks();
            ElapsedUs = (Now - Seg->SentTime) * 1000000ULL / TimerFreq();
            RttMs = (UINT32)(ElapsedUs / 1000);
            
            if (RttMs == 0) RttMs = 1;
            
            if (Sock->Srtt == 0) {
                Sock->Srtt = RttMs;
                Sock->Rttvar = RttMs / 2;
            } else {
                Sock->Rttvar = (3 * Sock->Rttvar + Abs((INT)Sock->Srtt - (INT)RttMs)) / 4;
                Sock->Srtt = (7 * Sock->Srtt + RttMs) / 8;
            }
            
            Sock->Rto = Sock->Srtt + MAX(4 * Sock->Rttvar, TCP_RTO_MIN);
            Sock->Rto = MIN(Sock->Rto, TCP_RTO_MAX);
            break;
        }
    }
}

static NOPTR TcpRemoveAcked(TcpSocket *Sock, UINT32 AckSeq) {
    ListHead *Pos;
    ListHead *Tmp;
    TcpSegment *Seg;
    BOOL AckedSomething = FALSE;
    
    ListForEachSafe(Pos, Tmp, &Sock->RetransQueue) {
        Seg = ListEntry(Pos, TcpSegment, Node);
        if (Seg->Seq + Seg->Len <= AckSeq) {
            ListDel(&Seg->Node);
            Sock->RetransQueueLen -= Seg->Len;
            MemoryFree(Seg->Data);
            MemoryFree(Seg);
            AckedSomething = TRUE;
        } else {
            break;
        }
    }
    
    if (AckedSomething) {
        Sock->RetransCount = 0;
        Sock->RxtShift = 0;
        Sock->TimerActive = FALSE;
    }
}

static NOPTR TcpRetransmit(TcpSocket *Sock) {
    ListHead *Pos;
    TcpSegment *Seg;
    
    if (ListEmpty(&Sock->RetransQueue)) {
        Sock->TimerActive = FALSE;
        return;
    }
    
    Pos = Sock->RetransQueue.Next;
    Seg = ListEntry(Pos, TcpSegment, Node);
    
    Sock->Rto = MIN(Sock->Rto * 2, TCP_RTO_MAX);
    Sock->RetransCount++;
    Sock->RxtShift++;
    
    TcpSendSegment(Sock, Seg->Seq, Sock->RcvNxt,
                  Seg->Flags, Seg->Data, Seg->Len, TRUE);
    
    Seg->SentTime = TimerTicks();
    Seg->RetransCount++;
    
    Sock->TimerExpires = Seg->SentTime + TimerMsToTicks(Sock->Rto);
    Sock->TimerActive = TRUE;
    
    if (Sock->RetransCount >= TCP_MAX_RETRANSMIT) {
        ConsolePrint("[TCP] Max retransmissions reached, aborting connection\n");
        if (Sock->ErrorCb) Sock->ErrorCb(Sock, TCP_ERROR_TIMEOUT);
        TcpAbort(Sock);
    }
}

/*
 * ============================================================================
 * TCP state machine helpers
 * ============================================================================
 */

static NOPTR TcpSendSyn(TcpSocket *Sock) {
    Sock->Iss = GTcpIss;
    GTcpIss += 1000000;
    Sock->SndUna = Sock->Iss;
    Sock->SndNxt = Sock->Iss + 1;
    
    TcpSendSegment(Sock, Sock->Iss, 0, TCP_FLAG_SYN, NULLPTR, 0, FALSE);
    
    Sock->LastSent = TimerTicks();
    Sock->TimerExpires = Sock->LastSent + TimerMsToTicks(TCP_SYN_RTO);
    Sock->TimerActive = TRUE;
}

static NOPTR TcpSendSynAck(TcpSocket *Sock) {
    Sock->Iss = GTcpIss;
    GTcpIss += 1000000;
    Sock->SndUna = Sock->Iss;
    Sock->SndNxt = Sock->Iss + 1;
    
    TcpSendSegment(Sock, Sock->Iss, Sock->RcvNxt,
                  TCP_FLAG_SYN | TCP_FLAG_ACK, NULLPTR, 0, FALSE);
    
    Sock->TimerExpires = TimerTicks() + TimerMsToTicks(TCP_SYN_RTO);
    Sock->TimerActive = TRUE;
}

static NOPTR TcpSendFin(TcpSocket *Sock) {
    TcpSendSegment(Sock, Sock->SndNxt, Sock->RcvNxt,
                  TCP_FLAG_FIN | TCP_FLAG_ACK, NULLPTR, 0, FALSE);
    Sock->SndNxt++;
    Sock->LastSent = TimerTicks();
    Sock->TimerExpires = Sock->LastSent + TimerMsToTicks(Sock->Rto);
    Sock->TimerActive = TRUE;
}

static NOPTR TcpSendRst(TcpSocket *Sock) {
    TcpSendSegment(Sock, Sock->SndNxt, Sock->RcvNxt,
                  TCP_FLAG_RST | TCP_FLAG_ACK, NULLPTR, 0, FALSE);
}

/*
 * ============================================================================
 * API Implementation
 * ============================================================================
 */

INT TcpBind(TcpSocket *Sock, IpV4Addr Addr, UINT16 Port) {
    if (!Sock) RETURN(NO_OBJECT);
    if (Sock->State != TCP_STATE_CLOSED) RETURN(INCORRECT_VALUE);
    if (TcpPortInUse(Port, Addr)) RETURN(ALREADY_EXISTS);
    
    Sock->LocalAddr = Addr;
    Sock->LocalPort = Port;
    
    RETURN(SUCCESS);
}

INT TcpListen(TcpSocket *Sock, INT Backlog) {
    if (!Sock) RETURN(NO_OBJECT);
    if (Sock->State != TCP_STATE_CLOSED) RETURN(INCORRECT_VALUE);
    if (Sock->LocalPort == 0) RETURN(INCORRECT_VALUE);
    
    Sock->State = TCP_STATE_LISTEN;
    Sock->BackLog = (UINT32)Backlog;
    
    ListAddTail(&GTcpListenSockets, &Sock->Node);
    
    RETURN(SUCCESS);
}

INT TcpConnect(TcpSocket *Sock, IpV4Addr Addr, UINT16 Port) {
    if (!Sock) RETURN(NO_OBJECT);
    if (Sock->State != TCP_STATE_CLOSED) RETURN(INCORRECT_VALUE);
    
    Sock->RemoteAddr = Addr;
    Sock->RemotePort = Port;
    
    if (Sock->LocalPort == 0) {
        Sock->LocalPort = TcpGetEphemeralPort();
    }
    
    Sock->State = TCP_STATE_SYN_SENT;
    
    TcpSocketLink(Sock);
    TcpSendSyn(Sock);
    
    RETURN(SUCCESS);
}

TcpSocket *TcpAccept(TcpSocket *ListenSock) {
    TcpSocket *NewSock;
    
    if (!ListenSock || ListenSock->State != TCP_STATE_LISTEN) return NULLPTR;
    if (ListEmpty(&ListenSock->AcceptQueue)) return NULLPTR;
    
    NewSock = ListEntry(ListenSock->AcceptQueue.Next, TcpSocket, Node);
    ListDel(&NewSock->Node);
    ListenSock->AcceptQueueLen--;
    
    return NewSock;
}

INT TcpSend(TcpSocket *Sock, const UINT8 *Data, UINT32 Len) {
    UINT32 Remaining;
    UINT32 Offset;
    UINT32 Chunk;
    
    if (!Sock || Len == 0) RETURN(NO_OBJECT);
    if (Sock->State != TCP_STATE_ESTABLISHED) RETURN(INCORRECT_VALUE);
    
    Remaining = Len;
    Offset = 0;
    
    while (Remaining > 0) {
        Chunk = Remaining;
        if (Chunk > Sock->MssSend) Chunk = Sock->MssSend;
        
        TcpQueueData(Sock, Data + Offset, Chunk);
        
        Offset += Chunk;
        Remaining -= Chunk;
    }
    
    TcpSendFromQueue(Sock);
    
    RETURN((INT)Len);
}

INT TcpRecv(TcpSocket *Sock, UINT8 *Buf, UINT32 Len) {
    TcpSegment *Seg;
    UINT32 Copied = 0;
    UINT32 ToCopy;
    UINT8 *NewData;
    
    if (!Sock || !Buf) RETURN(NO_OBJECT);
    if (Sock->State == TCP_STATE_CLOSED) RETURN(INCORRECT_VALUE);
    
    while (Copied < Len && !ListEmpty(&Sock->RecvQueue)) {
        Seg = ListEntry(Sock->RecvQueue.Next, TcpSegment, Node);
        
        ToCopy = Seg->Len;
        if (ToCopy > Len - Copied) ToCopy = Len - Copied;
        
        MemCpy(Buf + Copied, Seg->Data, ToCopy);
        Copied += ToCopy;
        
        if (ToCopy == Seg->Len) {
            ListDel(&Seg->Node);
            Sock->RecvQueueLen -= Seg->Len;
            MemoryFree(Seg->Data);
            MemoryFree(Seg);
        } else {
            NewData = (UINT8*)MemoryAllocate(Seg->Len - ToCopy);
            MemCpy(NewData, Seg->Data + ToCopy, Seg->Len - ToCopy);
            MemoryFree(Seg->Data);
            Seg->Data = NewData;
            Seg->Len -= ToCopy;
            Seg->Seq += ToCopy;
        }
        
        Sock->RcvNxt += ToCopy;
        Sock->RcvWnd += ToCopy;
    }
    
    RETURN((INT)Copied);
}

INT TcpClose(TcpSocket *Sock) {
    if (!Sock) RETURN(NO_OBJECT);
    
    switch (Sock->State) {
        case TCP_STATE_ESTABLISHED:
            Sock->State = TCP_STATE_FIN_WAIT_1;
            TcpSendFin(Sock);
            break;
            
        case TCP_STATE_CLOSE_WAIT:
            Sock->State = TCP_STATE_LAST_ACK;
            TcpSendFin(Sock);
            break;
            
        default:
            RETURN(INCORRECT_VALUE);
    }
    
    RETURN(SUCCESS);
}

INT TcpAbort(TcpSocket *Sock) {
    if (!Sock) RETURN(NO_OBJECT);
    
    TcpSendRst(Sock);
    TcpSocketDestroy(Sock);
    
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Timer handling
 * ============================================================================
 */

NOPTR TcpTimerHandler(NOPTR) {
    ListHead *Pos;
    ListHead *Tmp;
    TcpSocket *Sock;
    UINT64 Now = TimerTicks();
    
    ListForEachSafe(Pos, Tmp, &GTcpSockets) {
        Sock = ListEntry(Pos, TcpSocket, Node);
        if (!Sock->TimerActive) continue;
        
        if (Now >= Sock->TimerExpires) {
            switch (Sock->State) {
                case TCP_STATE_SYN_SENT:
                case TCP_STATE_SYN_RCVD:
                    if (Sock->RetransCount >= TCP_MAX_RETRANSMIT) {
                        if (Sock->ErrorCb) Sock->ErrorCb(Sock, TCP_ERROR_TIMEOUT);
                        TcpAbort(Sock);
                    } else {
                        TcpSendSyn(Sock);
                    }
                    break;
                    
                case TCP_STATE_ESTABLISHED:
                case TCP_STATE_FIN_WAIT_1:
                case TCP_STATE_LAST_ACK:
                    TcpRetransmit(Sock);
                    break;
                    
                case TCP_STATE_TIME_WAIT:
                    if (Now >= Sock->StateExpires) {
                        TcpSocketDestroy(Sock);
                    }
                    break;
                    
                default:
                    Sock->TimerActive = FALSE;
                    break;
            }
        }
    }
}

/*
 * ============================================================================
 * Input processing
 * ============================================================================
 */

static TcpSocket *TcpFindSocket(UINT16 LocalPort, UINT16 RemotePort,
                                  IpV4Addr LocalAddr, IpV4Addr RemoteAddr) {
    ListHead *Pos;
    TcpSocket *Sock;
    
    /* Exact match */
    ListForEach(Pos, &GTcpSockets) {
        Sock = ListEntry(Pos, TcpSocket, Node);
        if (Sock->LocalPort == LocalPort &&
            Sock->RemotePort == RemotePort &&
            Sock->LocalAddr.Addr == LocalAddr.Addr &&
            Sock->RemoteAddr.Addr == RemoteAddr.Addr) {
            return Sock;
        }
    }
    
    /* Bound socket (listening) */
    ListForEach(Pos, &GTcpListenSockets) {
        Sock = ListEntry(Pos, TcpSocket, Node);
        if (Sock->LocalPort == LocalPort &&
            (Sock->LocalAddr.Addr == 0 || Sock->LocalAddr.Addr == LocalAddr.Addr)) {
            return Sock;
        }
    }
    
    return NULLPTR;
}

static NOPTR TcpHandleSegment(TcpSocket *Sock, TcpHeader *Tcp,
                               UINT32 Seq, UINT32 Ack, UINT16 Flags, UINT32 DataLen,
                               IpV4Addr Src, IpV4Addr Dst) {
    UINT32 ExpectedSeq;
    TcpSegment *Seg;
    TcpSocket *NewSock;
    UINT8 *Payload;
    
    (NOPTR)Src;
    (NOPTR)Dst;
    
    Sock->LastRecv = TimerTicks();
    Sock->SndWnd = Ntohs(Tcp->Window);
    
    switch (Sock->State) {
        case TCP_STATE_LISTEN:
            if (Flags & TCP_FLAG_SYN) {
                NewSock = TcpSocketCreate();
                if (!NewSock) break;
                
                NewSock->LocalAddr = Sock->LocalAddr;
                NewSock->LocalPort = Sock->LocalPort;
                NewSock->RemoteAddr = Src;
                NewSock->RemotePort = Ntohs(Tcp->SrcPort);
                
                NewSock->Irs = Seq;
                NewSock->RcvNxt = Seq + 1;
                NewSock->Iss = GTcpIss;
                GTcpIss += 1000000;
                NewSock->SndUna = NewSock->Iss;
                NewSock->SndNxt = NewSock->Iss + 1;
                
                NewSock->State = TCP_STATE_SYN_RCVD;
                
                TcpSocketLink(NewSock);
                TcpSendSynAck(NewSock);
                
                if (Sock->AcceptQueueLen < Sock->BackLog) {
                    ListAddTail(&Sock->AcceptQueue, &NewSock->Node);
                    Sock->AcceptQueueLen++;
                } else {
                    TcpAbort(NewSock);
                }
            }
            break;
            
        case TCP_STATE_SYN_SENT:
            if (Flags & TCP_FLAG_SYN && Flags & TCP_FLAG_ACK) {
                Sock->RcvNxt = Seq + 1;
                Sock->Irs = Seq;
                Sock->SndUna = Ack;
                
                TcpSendSegment(Sock, Sock->SndNxt, Sock->RcvNxt,
                              TCP_FLAG_ACK, NULLPTR, 0, FALSE);
                
                Sock->State = TCP_STATE_ESTABLISHED;
                Sock->TimerActive = FALSE;
                
                if (Sock->ConnectCb) Sock->ConnectCb(Sock, TRUE);
            } else if (Flags & TCP_FLAG_SYN) {
                Sock->RcvNxt = Seq + 1;
                Sock->Irs = Seq;
                
                TcpSendSegment(Sock, Sock->SndNxt, Sock->RcvNxt,
                              TCP_FLAG_SYN | TCP_FLAG_ACK, NULLPTR, 0, FALSE);
                
                Sock->State = TCP_STATE_SYN_RCVD;
            } else if (Flags & TCP_FLAG_RST) {
                if (Sock->ConnectCb) Sock->ConnectCb(Sock, FALSE);
                TcpSocketDestroy(Sock);
            }
            break;
            
        case TCP_STATE_SYN_RCVD:
            if (Flags & TCP_FLAG_ACK) {
                Sock->State = TCP_STATE_ESTABLISHED;
                Sock->TimerActive = FALSE;
                if (Sock->ConnectCb) Sock->ConnectCb(Sock, TRUE);
                TcpSendFromQueue(Sock);
            }
            break;
            
        case TCP_STATE_ESTABLISHED:
            if (Flags & TCP_FLAG_ACK) {
                if (Ack > Sock->SndUna) {
                    TcpUpdateRtt(Sock, Ack);
                    TcpRemoveAcked(Sock, Ack);
                    TcpSendFromQueue(Sock);
                }
            }
            
            if (Flags & TCP_FLAG_FIN) {
                Sock->RcvNxt++;
                Sock->State = TCP_STATE_CLOSE_WAIT;
                TcpSendSegment(Sock, Sock->SndNxt, Sock->RcvNxt,
                              TCP_FLAG_ACK, NULLPTR, 0, FALSE);
                if (Sock->CloseCb) Sock->CloseCb(Sock);
            }
            
            if (DataLen > 0) {
                Seg = (TcpSegment*)MemoryAllocate(sizeof(TcpSegment));
                if (Seg) {
                    MemSet(Seg, 0, sizeof(TcpSegment));
                    Seg->Seq = Seq;
                    Seg->Len = DataLen;
                    Seg->Data = (UINT8*)MemoryAllocate(DataLen);
                    Payload = (UINT8*)Tcp + TCP_GET_DATA_OFFSET(Tcp) * 4;
                    if (Seg->Data) {
                        MemCpy(Seg->Data, Payload, DataLen);
                    }
                    
                    ListAddTail(&Sock->RecvQueue, &Seg->Node);
                    Sock->RecvQueueLen += DataLen;
                    Sock->RcvWnd -= DataLen;
                    Sock->RcvNxt += DataLen;
                    
                    TcpSendSegment(Sock, Sock->SndNxt, Sock->RcvNxt,
                                  TCP_FLAG_ACK, NULLPTR, 0, FALSE);
                    
                    if (Sock->RecvCb) Sock->RecvCb(Sock);
                }
            }
            break;
            
        case TCP_STATE_FIN_WAIT_1:
            if (Flags & TCP_FLAG_FIN && Flags & TCP_FLAG_ACK) {
                Sock->State = TCP_STATE_TIME_WAIT;
                Sock->StateExpires = TimerTicks() + TimerMsToTicks(TCP_2MSL);
                Sock->TimerActive = TRUE;
                TcpSendSegment(Sock, Sock->SndNxt, Sock->RcvNxt,
                              TCP_FLAG_ACK, NULLPTR, 0, FALSE);
            } else if (Flags & TCP_FLAG_ACK) {
                Sock->State = TCP_STATE_FIN_WAIT_2;
            }
            break;
            
        case TCP_STATE_FIN_WAIT_2:
            if (Flags & TCP_FLAG_FIN) {
                Sock->State = TCP_STATE_TIME_WAIT;
                Sock->StateExpires = TimerTicks() + TimerMsToTicks(TCP_2MSL);
                Sock->TimerActive = TRUE;
                TcpSendSegment(Sock, Sock->SndNxt, Sock->RcvNxt,
                              TCP_FLAG_ACK, NULLPTR, 0, FALSE);
            }
            break;
            
        case TCP_STATE_CLOSE_WAIT:
            break;
            
        case TCP_STATE_LAST_ACK:
            if (Flags & TCP_FLAG_ACK) {
                TcpSocketDestroy(Sock);
            }
            break;
            
        case TCP_STATE_TIME_WAIT:
            if (Flags & TCP_FLAG_FIN) {
                TcpSendSegment(Sock, Sock->SndNxt, Sock->RcvNxt,
                              TCP_FLAG_ACK, NULLPTR, 0, FALSE);
            }
            break;
            
        default:
            break;
    }
}

INT TcpInput(NetworkBuf *Buf, IpV4Addr Src, IpV4Addr Dst,
             UINT16 SrcPort, UINT16 DstPort) {
    TcpHeader *Tcp;
    UINT32 HeaderLen;
    UINT32 DataLen;
    UINT32 Seq;
    UINT32 Ack;
    UINT16 Flags;
    UINT16 ReceivedCsum;
    TcpSocket *Sock;
    TcpHeader Rst;
    NetworkBuf *RstBuf;
    UINT32 RstSeq;
    UINT32 RstAck;
    
    Tcp = (TcpHeader*)Buf->Data;
    HeaderLen = TCP_GET_DATA_OFFSET(Tcp) * 4;
    DataLen = Buf->Len - HeaderLen;
    Seq = Ntohl(Tcp->SeqNum);
    Ack = Ntohl(Tcp->AckNum);
    Flags = TCP_GET_FLAGS(Tcp);
    
    ReceivedCsum = Tcp->Checksum;
    Tcp->Checksum = 0;
    if (TcpChecksum(Tcp, HeaderLen + DataLen, Src, Dst) != ReceivedCsum) {
        ConsolePrint("Bad checksum\n");
        RETURN(INCORRECT_VALUE);
    }
    Tcp->Checksum = ReceivedCsum;
    
    Sock = TcpFindSocket(DstPort, SrcPort, Dst, Src);
    if (!Sock) {
        if (Flags & TCP_FLAG_ACK) {
            RstSeq = Ack;
            RstAck = Seq + DataLen;
            
            MemSet(&Rst, 0, sizeof(Rst));
            Rst.SrcPort = Htons(DstPort);
            Rst.DstPort = Htons(SrcPort);
            Rst.SeqNum = Htonl(RstSeq);
            Rst.AckNum = Htonl(RstAck);
            TCP_SET_DATA_OFFSET(&Rst, 5);
            TCP_SET_FLAGS(&Rst, TCP_FLAG_RST | TCP_FLAG_ACK);
            Rst.Window = 0;
            Rst.Checksum = 0;
            Rst.Checksum = TcpChecksum(&Rst, 20, Dst, Src);
            
            RstBuf = NetworkAllocBuf(20 + IPV4_HEADER_MIN + NETWORK_HEADROOM);
            if (RstBuf) {
                NetworkReserve(RstBuf, IPV4_HEADER_MIN + NETWORK_HEADROOM);
                MemCpy(RstBuf->Data, &Rst, 20);
                RstBuf->Len = 20;
                IpV4Output(RstBuf, Src, Dst, IPV4_PROTO_TCP);
                NetworkFreeBuf(RstBuf);
            }
        }
        RETURN(NO_OBJECT);
    }
    
    TcpHandleSegment(Sock, Tcp, Seq, Ack, Flags, DataLen, Src, Dst);
    
    RETURN(SUCCESS);
}

static NOPTR TcpInputDispatch(NetworkBuf *Buf, NetworkDevice *Dev) {
    TcpHeader *Tcp;
    UINT32 HeaderLen;
    IpV4Addr Src;
    IpV4Addr Dst;

    (NOPTR)Dev;

    if (Buf->Len < TCP_HEADER_MIN) {
        return;
    }

    Tcp = (TcpHeader *)Buf->Data;
    HeaderLen = TCP_GET_DATA_OFFSET(Tcp) * 4;
    if (Buf->Len < HeaderLen) {
        return;
    }

    Src.Addr = Buf->IpSrc;
    Dst.Addr = Buf->IpDst;
    TcpInput(Buf, Src, Dst, Ntohs(Tcp->SrcPort), Ntohs(Tcp->DstPort));
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */

INT TcpInit(NOPTR) {
    if (GTcpInitialized) {
        RETURN(SUCCESS);
    }
    
    ListInit(&GTcpSockets);
    ListInit(&GTcpListenSockets);
    
    IpV4RegisterProtocol(IPV4_PROTO_TCP, TcpInputDispatch);
    
    GTcpReady = TRUE;
    TcpReady = TRUE;
    GTcpInitialized = TRUE;
    
    RETURN(SUCCESS);
}