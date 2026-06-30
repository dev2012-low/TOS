#pragma once

#include <Network/Network.h>
#include <Network/IpV4.h>
#include <Kernel/List.h>
#include <Time/Timer.h>

/*
 * ============================================================================
 * TCP Constants
 * ============================================================================
 */

#define TCP_HEADER_MIN          20
#define TCP_HEADER_MAX          60
#define TCP_MSS_DEFAULT         536
#define TCP_MSS_CALCULATED      1460
#define TCP_WINDOW_DEFAULT      65535
#define TCP_WINDOW_MAX          65535

#define TCP_RTO_MIN             200     /* 200ms */
#define TCP_RTO_MAX             120000  /* 120s */
#define TCP_RTO_INITIAL         3000    /* 3s */
#define TCP_SYN_RTO             3000    /* 3s for SYN */
#define TCP_MSL                 30000   /* 30s Maximum Segment Lifetime */
#define TCP_2MSL                (2 * TCP_MSL)

#define TCP_MAX_RETRANSMIT      12      /* Максимум ретрансмиссий */

/*
 * ============================================================================
 * TCP Flags
 * ============================================================================
 */

#define TCP_FLAG_FIN            0x01
#define TCP_FLAG_SYN            0x02
#define TCP_FLAG_RST            0x04
#define TCP_FLAG_PSH            0x08
#define TCP_FLAG_ACK            0x10
#define TCP_FLAG_URG            0x20
#define TCP_FLAG_ECE            0x40
#define TCP_FLAG_CWR            0x80
#define TCP_FLAG_NS             0x100

#ifndef ETH_HLEN
#define ETH_HLEN 14
#endif

/*
 * ============================================================================
 * TCP States
 * ============================================================================
 */

typedef enum {
    TCP_STATE_CLOSED           = 0,
    TCP_STATE_LISTEN           = 1,
    TCP_STATE_SYN_SENT         = 2,
    TCP_STATE_SYN_RCVD         = 3,
    TCP_STATE_ESTABLISHED      = 4,
    TCP_STATE_FIN_WAIT_1       = 5,
    TCP_STATE_FIN_WAIT_2       = 6,
    TCP_STATE_CLOSE_WAIT       = 7,
    TCP_STATE_CLOSING          = 8,
    TCP_STATE_LAST_ACK         = 9,
    TCP_STATE_TIME_WAIT        = 10,
} TcpState;

static inline const CHAR* TcpStateString(TcpState State) {
    switch (State) {
        case TCP_STATE_CLOSED:      return "CLOSED";
        case TCP_STATE_LISTEN:      return "LISTEN";
        case TCP_STATE_SYN_SENT:    return "SYN_SENT";
        case TCP_STATE_SYN_RCVD:    return "SYN_RCVD";
        case TCP_STATE_ESTABLISHED: return "ESTABLISHED";
        case TCP_STATE_FIN_WAIT_1:  return "FIN_WAIT_1";
        case TCP_STATE_FIN_WAIT_2:  return "FIN_WAIT_2";
        case TCP_STATE_CLOSE_WAIT:  return "CLOSE_WAIT";
        case TCP_STATE_CLOSING:     return "CLOSING";
        case TCP_STATE_LAST_ACK:    return "LAST_ACK";
        case TCP_STATE_TIME_WAIT:   return "TIME_WAIT";
        default:                    return "UNKNOWN";
    }
}

/*
 * ============================================================================
 * TCP Header (packed for wire)
 * ============================================================================
 */

typedef struct ATTRIBUTE(packed) {
    UINT16 SrcPort;
    UINT16 DstPort;
    UINT32 SeqNum;
    UINT32 AckNum;
    UINT16 OffsetReservedFlags;
    UINT16 Window;
    UINT16 Checksum;
    UINT16 Urgent;
} TcpHeader;

/* Helper macros */
#define TCP_GET_DATA_OFFSET(Hdr) (((Hdr)->OffsetReservedFlags >> 12) & 0xF)
#define TCP_GET_FLAGS(Hdr) ((Hdr)->OffsetReservedFlags & 0x1FF)
#define TCP_SET_FLAGS(Hdr, Flags) \
    ((Hdr)->OffsetReservedFlags = ((Hdr)->OffsetReservedFlags & ~0x1FF) | ((Flags) & 0x1FF))
#define TCP_SET_DATA_OFFSET(Hdr, Offset) \
    ((Hdr)->OffsetReservedFlags = ((Hdr)->OffsetReservedFlags & ~0xF000) | (((Offset) & 0xF) << 12))

/*
 * ============================================================================
 * TCP Segment (for queuing)
 * ============================================================================
 */

typedef struct TcpSegment {
    struct ListHead Node;
    UINT32 Seq;
    UINT32 Len;
    UINT32 Flags;
    UINT8 *Data;
    UINT64 SentTime;
    UINT32 RetransCount;
    BOOL Acked;
} TcpSegment;

/*
 * ============================================================================
 * TCP Socket
 * ============================================================================
 */

typedef struct TcpSocket {
    /* 5-tuple */
    IpV4Addr LocalAddr;
    IpV4Addr RemoteAddr;
    UINT16 LocalPort;
    UINT16 RemotePort;
    
    /* State */
    TcpState State;
    UINT64 StateExpires;      /* For TIME_WAIT, SYN_RCVD timers (tick units) */
    
    /* Sequence numbers */
    UINT32 SndUna;            /* Sent but not acked */
    UINT32 SndNxt;            /* Next to send */
    UINT32 SndWnd;            /* Send window (from peer) */
    UINT32 SndWl1;            /* Window update segment seq number */
    UINT32 SndWl2;            /* Window update segment ack number */
    UINT32 Iss;                /* Initial send sequence number */
    
    UINT32 RcvNxt;            /* Next expected to receive */
    UINT32 RcvWnd;            /* Receive window (my buffer) */
    UINT32 RcvUp;             /* Urgent pointer */
    UINT32 Irs;                /* Initial receive sequence number */
    
    /* Congestion control (simplified) */
    UINT32 Cwnd;               /* Congestion window */
    UINT32 Ssthresh;           /* Slow start threshold */
    
    /* RTT estimation (RFC 6298) */
    UINT32 Srtt;               /* Smoothed RTT (ms) */
    UINT32 Rttvar;             /* RTT variance (ms) */
    UINT32 Rto;                /* Retransmission timeout (ms) */
    UINT32 RxtShift;          /* Exponential backoff shift */
    UINT32 RetransCount;      /* Current retransmission count */
    
    /* Timers */
    UINT64 TimerExpires;      /* When current timer fires */
    bool TimerActive;
    UINT64 LastSent;          /* Last packet send time */
    UINT64 LastRecv;          /* Last packet receive time */
    
    /* Queues */
    struct ListHead SendQueue;    /* Data waiting to be sent */
    struct ListHead RetransQueue; /* Data sent but not ACKed */
    struct ListHead RecvQueue;    /* Received data ready for app */
    
    UINT32 SendQueueLen;
    UINT32 RetransQueueLen;
    UINT32 RecvQueueLen;
    
    /* MSS */
    UINT32 MssSend;           /* MSS to use when sending */
    UINT32 MssRecv;           /* MSS advertised to peer */
    
    /* Options */
    BOOL WScaleEnabled;
    UINT32 SendScale;
    UINT32 RecvScale;
    BOOL TimestampEnabled;
    
    /* Callbacks */
    NOPTR (*ConnectCb)(struct TcpSocket *Sock, BOOL Success);
    NOPTR (*RecvCb)(struct TcpSocket *Sock);
    NOPTR (*CloseCb)(struct TcpSocket *Sock);
    NOPTR (*ErrorCb)(struct TcpSocket *Sock, INT Error);
    
    /* Backlog for listening sockets */
    struct ListHead AcceptQueue;
    UINT32 AcceptQueueLen;
    UINT32 BackLog;
    
    /* Private data for user */
    NOPTR *Priv;
    
    /* Linked list node */
    struct ListHead Node;
} TcpSocket;

/*
 * ============================================================================
 * TCP Callbacks
 * ============================================================================
 */

#define TCP_ERROR_CONNECTION_RESET   1
#define TCP_ERROR_TIMEOUT            2
#define TCP_ERROR_REFUSED            3

/*
 * ============================================================================
 * API Functions
 * ============================================================================
 */

/* Initialization */
INT TcpInit(NOPTR);
NOPTR TcpTimerHandler(NOPTR);  /* Called periodically (every 100ms) */

/* Socket management */
TcpSocket *TcpSocketCreate(NOPTR);
NOPTR TcpSocketDestroy(TcpSocket *Sock);

/* Socket operations */
INT TcpBind(TcpSocket *Sock, IpV4Addr Addr, UINT16 Port);
INT TcpListen(TcpSocket *Sock, INT BackLog);
INT TcpConnect(TcpSocket *Sock, IpV4Addr Addr, UINT16 Port);
TcpSocket *TcpAccept(TcpSocket *ListenSock);

/* Data transfer */
INT TcpSend(TcpSocket *Sock, const UINT8 *Data, UINT32 Len);
INT TcpRecv(TcpSocket *Sock, UINT8 *Buf, UINT32 Len);

/* Closing */
INT TcpClose(TcpSocket *Sock);
INT TcpAbort(TcpSocket *Sock);

/* Input processing (called from IPv4) */
INT TcpInput(NetworkBuf *Buf, IpV4Addr Src, IpV4Addr Dst,
             UINT16 SrcPort, UINT16 DstPort);

/* Utility */
UINT16 TcpChecksum(TcpHeader *Tcp, UINT32 Len, IpV4Addr Src, IpV4Addr Dst);