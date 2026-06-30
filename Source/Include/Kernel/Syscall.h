#pragma once

#include <Kernel/Types.h>

#define KERNEL_CODE_SEL  0x08
#define KERNEL_DATA_SEL  0x20
#define USER_CODE_SEL    0x18
#define USER_DATA_SEL    0x10

#define MSR_EFER             0xC0000080
#define MSR_STAR             0xC0000081
#define MSR_LSTAR            0xC0000082
#define MSR_SFMASK           0xC0000084
#define MSR_GS_BASE          0xC0000101
#define MSR_KERNEL_GS_BASE   0xC0000102

#define EFER_SCE             (1ULL << 0)
#define RFLAGS_IF            (1ULL << 9)

#define SYSCALL_STAR \
    (((UINT64)(USER_CODE_SEL - 16) << 48) | ((UINT64)KERNEL_CODE_SEL << 32))

#define SYS_EXIT     0
#define SYS_WRITE    1
#define SYS_GETPID   2
#define SYS_YIELD    3
#define SYS_READ     4
#define SYS_GETPPID  5
#define SYS_SLEEP    6
#define SYS_UNAME    7
#define SYS_OPEN     8
#define SYS_CLOSE    9
#define SYS_LSEEK    10
#define SYS_STAT     11
#define SYS_GETCWD   12
#define SYS_CHDIR    13
#define SYS_MKDIR    14
#define SYS_UNLINK   15
#define SYS_EXEC     16
#define SYS_WAIT     17
#define SYS_KILL     18
#define SYS_GETUID   19
#define SYS_GETGID   20
#define SYS_GETEUID  21
#define SYS_GETEGID  22
#define SYS_SOCKET       23
#define SYS_CONNECT      24
#define SYS_BIND         25
#define SYS_LISTEN       26
#define SYS_ACCEPT       27
#define SYS_SEND         28
#define SYS_RECV         29
#define SYS_CLOSE_SOCKET 30
#define SYS_SETSOCKOPT   31
#define SYS_GETSOCKOPT   32
#define SYS_GETADDRINFO  33
#define SYS_HTTP_GET     34   /* Опционально: HTTP GET */
#define SYS_HTTP_POST    35   /* Опционально: HTTP POST */
#define SYS_MAX          36



#define TOS_UNAME_LEN 32

typedef struct SyscallFrame {
    UINT64 R15;
    UINT64 R14;
    UINT64 R13;
    UINT64 R12;
    UINT64 R11;
    UINT64 R10;
    UINT64 R9;
    UINT64 R8;
    UINT64 Rbp;
    UINT64 Rdi;
    UINT64 Rsi;
    UINT64 Rdx;
    UINT64 Rcx;
    UINT64 Rbx;
    UINT64 Rax;
} SyscallFrame;

typedef struct SyscallCpuData {
    UINT64 UserRsp;
    UINT64 KernelRsp;
} SyscallCpuData;

extern SyscallCpuData GSyscallCpu;

NOPTR SyscallInit(NOPTR);
NOPTR SyscallSetKernelRsp(UINT64 KernelRsp);
INT64 SyscallDispatch(SyscallFrame *Frame);

/* Address families */
#define TOS_AF_INET        1
#define TOS_AF_INET6       2

/* Socket types */
#define TOS_SOCK_STREAM    1
#define TOS_SOCK_DGRAM     2

/* Protocol families */
#define TOS_IPPROTO_TCP    6
#define TOS_IPPROTO_UDP    17

/* Socket address structure (for userspace) */
typedef struct {
    UINT16 SinFamily;
    UINT16 SinPort;
    UINT32 SinAddr;
    UINT8  SinZero[8];
} ATTRIBUTE(packed) TosSockAddrIn;

/* Internal socket structure (kernel-only) */
typedef struct {
    INT     Fd;
    UINT8   Type;
    UINT8   Protocol;
    BOOL    Connected;
    BOOL    Bound;
    BOOL    Listening;
    NOPTR   *Priv;          /* Pointer to TcpSocket or UdpSocket */
    struct {
        BOOL    ReuseAddr;
        UINT32  RcvTimeout;
        UINT32  SndTimeout;
    } Options;
    BOOL    Used;
} TosSocketFd;

/* Socket syscalls (kernel implementation) */
INT SyscallSocket(UINT64 Domain, UINT64 Type, UINT64 Protocol);
INT SyscallConnect(INT SockFd, UINT64 Addr, UINT64 AddrLen);
INT SyscallBind(INT SockFd, UINT64 Addr, UINT64 AddrLen);
INT SyscallListen(INT SockFd, INT Backlog);
INT SyscallAccept(INT SockFd, UINT64 Addr, UINT64 AddrLen);
INT SyscallSend(INT SockFd, UINT64 Buf, UINT64 Len, UINT64 Flags);
INT SyscallRecv(INT SockFd, UINT64 Buf, UINT64 Len, UINT64 Flags);
INT SyscallCloseSocket(INT SockFd);
INT SyscallGetAddrInfo(UINT64 Host, UINT64 Port, UINT64 AddrOut);
NOPTR SyscallSocketInit(NOPTR);