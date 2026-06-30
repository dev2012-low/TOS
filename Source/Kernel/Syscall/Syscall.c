#include <Kernel/Syscall.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Task.h>
#include <Kernel/SyscallFd.h>
#include <Console.h>
#include <Lib/String.h>
#include <Asm/Cpu.h>
#include <Kernel/Return.h>
#include <Kernel/UserAccount.h>
#include <Time/Timer.h>
#include <Ps2Keyboard.h>
#include <Elf.h>
#include <Memory/Allocator.h>
#include <Fs/Vfs.h>

#include <Network/Tcp.h>
#include <Network/Udp.h>
#include <Network/Dns.h>
#include <Network/IpV4.h>

EXTERN(CHAR, SyscallEntry[]);
SyscallCpuData GSyscallCpu;

#define USER_ADDR_MAX 0x00007FFFFFFFFFFFULL

/*
 * ============================================================================
 * Socket Table (отдельная от файловых дескрипторов)
 * ============================================================================
 */

#define MAX_SOCKETS 32

static TosSocketFd *SocketTable[MAX_SOCKETS];
static UINT32 SocketCount = 0;
static SpinLock SocketLock;

/* ============================================================================
 * Forward declarations
 * ============================================================================ */

INT SyscallSocket(UINT64 Domain, UINT64 Type, UINT64 Protocol);
INT SyscallConnect(INT SockFd, UINT64 Addr, UINT64 AddrLen);
INT SyscallBind(INT SockFd, UINT64 Addr, UINT64 AddrLen);
INT SyscallListen(INT SockFd, INT Backlog);
INT SyscallAccept(INT SockFd, UINT64 Addr, UINT64 AddrLen);
INT SyscallSend(INT SockFd, UINT64 Buf, UINT64 Len, UINT64 Flags);
INT SyscallRecv(INT SockFd, UINT64 Buf, UINT64 Len, UINT64 Flags);
INT SyscallCloseSocket(INT SockFd);
INT SyscallGetAddrInfo(UINT64 Host, UINT64 Port, UINT64 AddrOut);

/* ============================================================================
 * Socket initialization
 * ============================================================================ */

NOPTR SyscallSocketInit(NOPTR) {
    MemSet(SocketTable, 0, sizeof(SocketTable));
    SocketCount = 0;
    SpinLockInit(&SocketLock);
}

static INT SocketAllocFd(NOPTR) {
    for (INT I = 0; I < MAX_SOCKETS; I++) {
        if (!SocketTable[I]) {
            return I;
        }
    }
    return -1;
}

/* ============================================================================
 * socket() - Create a new socket
 * ============================================================================ */

INT SyscallSocket(UINT64 Domain, UINT64 Type, UINT64 Protocol) {
    KTask *Task = SchedulerGetCurrent();
    TosSocketFd *Sock;
    INT Fd;
    
    if (!Task) {
        return -NO_OBJECT;
    }
    
    /* Проверка параметров */
    if (Domain != TOS_AF_INET) {
        return -NOT_SUPPORTED;
    }
    
    if (Type != TOS_SOCK_STREAM && Type != TOS_SOCK_DGRAM) {
        return -NOT_SUPPORTED;
    }
    
    SpinLockAcquire(&SocketLock);
    
    Fd = SocketAllocFd();
    if (Fd < 0) {
        SpinLockRelease(&SocketLock);
        return -NO_MEMORY;
    }
    
    Sock = (TosSocketFd*)MemoryAllocate(sizeof(TosSocketFd));
    if (!Sock) {
        SpinLockRelease(&SocketLock);
        return -NO_MEMORY;
    }
    
    MemSet(Sock, 0, sizeof(TosSocketFd));
    Sock->Fd = Fd;
    Sock->Type = (UINT8)Type;
    Sock->Protocol = (UINT8)Protocol;
    Sock->Connected = FALSE;
    Sock->Bound = FALSE;
    Sock->Listening = FALSE;
    Sock->Used = TRUE;
    Sock->Options.ReuseAddr = TRUE;
    Sock->Options.RcvTimeout = 5000;
    Sock->Options.SndTimeout = 5000;
    
    if (Type == TOS_SOCK_STREAM) {
        Sock->Priv = TcpSocketCreate();
        if (!Sock->Priv) {
            MemoryFree(Sock);
            SpinLockRelease(&SocketLock);
            return -NO_MEMORY;
        }
    } else {
        /* UDP support coming soon */
        MemoryFree(Sock);
        SpinLockRelease(&SocketLock);
        return -NOT_IMPLEMENTED;
    }
    
    SocketTable[Fd] = Sock;
    SocketCount++;
    
    SpinLockRelease(&SocketLock);
    return Fd;
}

/* ============================================================================
 * connect() - Connect to remote host
 * ============================================================================ */

INT SyscallConnect(INT SockFd, UINT64 Addr, UINT64 AddrLen) {
    TosSocketFd *Sock;
    TosSockAddrIn *AddrIn;
    INT Result;
    IpV4Addr Ip;
    UINT16 Port;
    
    if (SockFd < 0 || SockFd >= MAX_SOCKETS) {
        return -INCORRECT_VALUE;
    }
    
    SpinLockAcquire(&SocketLock);
    
    Sock = SocketTable[SockFd];
    if (!Sock || !Sock->Used) {
        SpinLockRelease(&SocketLock);
        return -NO_OBJECT;
    }
    
    if (Sock->Type != TOS_SOCK_STREAM) {
        SpinLockRelease(&SocketLock);
        return -NOT_SUPPORTED;
    }
    
    if (AddrLen < sizeof(TosSockAddrIn)) {
        SpinLockRelease(&SocketLock);
        return -INCORRECT_VALUE;
    }
    
    AddrIn = (TosSockAddrIn*)(UINTPTR)Addr;
    Ip.Addr = AddrIn->SinAddr;
    Port = Ntohs(AddrIn->SinPort);
    
    if (Sock->Priv) {
        Result = TcpConnect((TcpSocket*)Sock->Priv, Ip, Port);
        if (Result == SUCCESS) {
            Sock->Connected = TRUE;
        }
        SpinLockRelease(&SocketLock);
        return Result;
    }
    
    SpinLockRelease(&SocketLock);
    return -GENERAL_ERROR;
}

/* ============================================================================
 * bind() - Bind socket to address
 * ============================================================================ */

INT SyscallBind(INT SockFd, UINT64 Addr, UINT64 AddrLen) {
    TosSocketFd *Sock;
    TosSockAddrIn *AddrIn;
    IpV4Addr Ip;
    UINT16 Port;
    INT Result;
    
    if (SockFd < 0 || SockFd >= MAX_SOCKETS) {
        return -INCORRECT_VALUE;
    }
    
    SpinLockAcquire(&SocketLock);
    
    Sock = SocketTable[SockFd];
    if (!Sock || !Sock->Used) {
        SpinLockRelease(&SocketLock);
        return -NO_OBJECT;
    }
    
    if (AddrLen < sizeof(TosSockAddrIn)) {
        SpinLockRelease(&SocketLock);
        return -INCORRECT_VALUE;
    }
    
    AddrIn = (TosSockAddrIn*)(UINTPTR)Addr;
    Ip.Addr = AddrIn->SinAddr;
    Port = Ntohs(AddrIn->SinPort);
    
    if (Sock->Type == TOS_SOCK_STREAM && Sock->Priv) {
        Result = TcpBind((TcpSocket*)Sock->Priv, Ip, Port);
        if (Result == SUCCESS) {
            Sock->Bound = TRUE;
        }
        SpinLockRelease(&SocketLock);
        return Result;
    }
    
    SpinLockRelease(&SocketLock);
    return -NOT_SUPPORTED;
}

/* ============================================================================
 * listen() - Start listening for connections
 * ============================================================================ */

INT SyscallListen(INT SockFd, INT Backlog) {
    TosSocketFd *Sock;
    
    if (SockFd < 0 || SockFd >= MAX_SOCKETS) {
        return -INCORRECT_VALUE;
    }
    
    SpinLockAcquire(&SocketLock);
    
    Sock = SocketTable[SockFd];
    if (!Sock || !Sock->Used) {
        SpinLockRelease(&SocketLock);
        return -NO_OBJECT;
    }
    
    if (Sock->Type != TOS_SOCK_STREAM) {
        SpinLockRelease(&SocketLock);
        return -NOT_SUPPORTED;
    }
    if (!Sock->Bound) {
        SpinLockRelease(&SocketLock);
        return -INCORRECT_VALUE;
    }
    
    if (Sock->Priv) {
        INT Result = TcpListen((TcpSocket*)Sock->Priv, Backlog);
        if (Result == SUCCESS) {
            Sock->Listening = TRUE;
        }
        SpinLockRelease(&SocketLock);
        return Result;
    }
    
    SpinLockRelease(&SocketLock);
    return -GENERAL_ERROR;
}

/* ============================================================================
 * accept() - Accept incoming connection
 * ============================================================================ */

INT SyscallAccept(INT SockFd, UINT64 Addr, UINT64 AddrLen) {
    TosSocketFd *Sock;
    TosSocketFd *NewSock;
    TcpSocket *ClientSock;
    TosSockAddrIn *AddrIn;
    UINT32 *AddrLenPtr;
    INT Fd;
    
    if (SockFd < 0 || SockFd >= MAX_SOCKETS) {
        return -INCORRECT_VALUE;
    }
    
    SpinLockAcquire(&SocketLock);
    
    Sock = SocketTable[SockFd];
    if (!Sock || !Sock->Used) {
        SpinLockRelease(&SocketLock);
        return -NO_OBJECT;
    }
    
    if (Sock->Type != TOS_SOCK_STREAM) {
        SpinLockRelease(&SocketLock);
        return -NOT_SUPPORTED;
    }
    if (!Sock->Listening || !Sock->Priv) {
        SpinLockRelease(&SocketLock);
        return -INCORRECT_VALUE;
    }
    
    ClientSock = TcpAccept((TcpSocket*)Sock->Priv);
    if (!ClientSock) {
        SpinLockRelease(&SocketLock);
        return -BUSY;
    }
    
    Fd = SocketAllocFd();
    if (Fd < 0) {
        TcpSocketDestroy(ClientSock);
        SpinLockRelease(&SocketLock);
        return -NO_MEMORY;
    }
    
    NewSock = (TosSocketFd*)MemoryAllocate(sizeof(TosSocketFd));
    if (!NewSock) {
        TcpSocketDestroy(ClientSock);
        SpinLockRelease(&SocketLock);
        return -NO_MEMORY;
    }
    
    MemSet(NewSock, 0, sizeof(TosSocketFd));
    NewSock->Fd = Fd;
    NewSock->Type = Sock->Type;
    NewSock->Protocol = Sock->Protocol;
    NewSock->Connected = TRUE;
    NewSock->Used = TRUE;
    NewSock->Priv = ClientSock;
    NewSock->Options.ReuseAddr = TRUE;
    
    SocketTable[Fd] = NewSock;
    SocketCount++;
    
    /* Fill address (optional) */
    if (Addr && AddrLen) {
        AddrLenPtr = (UINT32*)(UINTPTR)AddrLen;
        if (*AddrLenPtr >= sizeof(TosSockAddrIn)) {
            AddrIn = (TosSockAddrIn*)(UINTPTR)Addr;
            AddrIn->SinFamily = TOS_AF_INET;
            AddrIn->SinAddr = ClientSock->RemoteAddr.Addr;
            AddrIn->SinPort = Htons(ClientSock->RemotePort);
            *AddrLenPtr = sizeof(TosSockAddrIn);
        }
    }
    
    SpinLockRelease(&SocketLock);
    return Fd;
}

/* ============================================================================
 * send() - Send data
 * ============================================================================ */

INT SyscallSend(INT SockFd, UINT64 Buf, UINT64 Len, UINT64 Flags) {
    TosSocketFd *Sock;
    const UINT8 *Data;
    INT Result;
    
    (NOPTR)Flags;
    
    if (SockFd < 0 || SockFd >= MAX_SOCKETS) {
        return -INCORRECT_VALUE;
    }
    
    SpinLockAcquire(&SocketLock);
    
    Sock = SocketTable[SockFd];
    if (!Sock || !Sock->Used) {
        SpinLockRelease(&SocketLock);
        return -NO_OBJECT;
    }
    
    if (!Sock->Connected) {
        SpinLockRelease(&SocketLock);
        return -INCORRECT_VALUE;
    }
    
    if (!Buf || Len == 0 || !SyscallIsUserRange(Buf, Len)) {
        SpinLockRelease(&SocketLock);
        return -INCORRECT_VALUE;
    }
    
    Data = (const UINT8*)(UINTPTR)Buf;
    
    if (Sock->Type == TOS_SOCK_STREAM && Sock->Priv) {
        Result = TcpSend((TcpSocket*)Sock->Priv, Data, (UINT32)Len);
        SpinLockRelease(&SocketLock);
        return Result;
    }
    
    SpinLockRelease(&SocketLock);
    return -NOT_SUPPORTED;
}

/* ============================================================================
 * recv() - Receive data
 * ============================================================================ */

INT SyscallRecv(INT SockFd, UINT64 Buf, UINT64 Len, UINT64 Flags) {
    TosSocketFd *Sock;
    UINT8 *Data;
    INT Result;
    
    (NOPTR)Flags;
    
    if (SockFd < 0 || SockFd >= MAX_SOCKETS) {
        return -INCORRECT_VALUE;
    }
    
    SpinLockAcquire(&SocketLock);
    
    Sock = SocketTable[SockFd];
    if (!Sock || !Sock->Used) {
        SpinLockRelease(&SocketLock);
        return -NO_OBJECT;
    }
    
    if (!Sock->Connected) {
        SpinLockRelease(&SocketLock);
        return -INCORRECT_VALUE;
    }
    
    if (!Buf || Len == 0 || !SyscallIsUserRange(Buf, Len)) {
        SpinLockRelease(&SocketLock);
        return -INCORRECT_VALUE;
    }
    
    Data = (UINT8*)(UINTPTR)Buf;
    
    if (Sock->Type == TOS_SOCK_STREAM && Sock->Priv) {
        Result = TcpRecv((TcpSocket*)Sock->Priv, Data, (UINT32)Len);
        SpinLockRelease(&SocketLock);
        return Result;
    }
    
    SpinLockRelease(&SocketLock);
    return -NOT_SUPPORTED;
}

/* ============================================================================
 * close_socket() - Close socket
 * ============================================================================ */

INT SyscallCloseSocket(INT SockFd) {
    TosSocketFd *Sock;
    
    if (SockFd < 0 || SockFd >= MAX_SOCKETS) {
        return -INCORRECT_VALUE;
    }
    
    SpinLockAcquire(&SocketLock);
    
    Sock = SocketTable[SockFd];
    if (!Sock || !Sock->Used) {
        SpinLockRelease(&SocketLock);
        return -NO_OBJECT;
    }
    
    if (Sock->Type == TOS_SOCK_STREAM && Sock->Priv) {
        if (Sock->Connected) {
            TcpClose((TcpSocket*)Sock->Priv);
        }
        TcpSocketDestroy((TcpSocket*)Sock->Priv);
    }
    
    MemoryFree(Sock);
    SocketTable[SockFd] = NULLPTR;
    SocketCount--;
    
    SpinLockRelease(&SocketLock);
    return SUCCESS;
}

/* ============================================================================
 * getaddrinfo() - DNS resolution
 * ============================================================================ */

INT SyscallGetAddrInfo(UINT64 Host, UINT64 Port, UINT64 AddrOut) {
    const CHAR *HostName;
    UINT16 PortNum;
    TosSockAddrIn *AddrIn;
    IpV4Addr Ip;
    INT Result;
    
    if (!Host || !AddrOut || !SyscallIsUserRange(Host, 1)) {
        return -INCORRECT_VALUE;
    }
    
    HostName = (const CHAR*)(UINTPTR)Host;
    PortNum = (UINT16)Port;
    
    Result = DnsResolve(HostName, &Ip);
    if (Result != SUCCESS) {
        return -NOT_FOUND;
    }
    
    if (SyscallIsUserRange(AddrOut, sizeof(TosSockAddrIn))) {
        AddrIn = (TosSockAddrIn*)(UINTPTR)AddrOut;
        AddrIn->SinFamily = TOS_AF_INET;
        AddrIn->SinAddr = Ip.Addr;
        AddrIn->SinPort = Htons(PortNum);
        MemSet(AddrIn->SinZero, 0, 8);
    }
    
    RETURN(SUCCESS);
}

/* ============================================================================
 * SyscallSetKernelRsp - Set kernel stack for syscalls
 * ============================================================================ */

NOPTR SyscallSetKernelRsp(UINT64 KernelRsp) {
    GSyscallCpu.KernelRsp = KernelRsp;
}


/* ============================================================================
 * SyscallExit - Terminate current task
 * ============================================================================ */

static INT64 SyscallExit(UINT64 Code) {
    KTask *Task = SchedulerGetCurrent();
    if (Task) {
        Task->ExitCode = (INT32)Code;
    }
    SyscallFdCloseAll(Task);
    SchedulerTerminate();
    return 0;
}

/* ============================================================================
 * SyscallWrite - Write to file descriptor
 * ============================================================================ */

static INT64 SyscallWrite(INT64 Fd, UINT64 Buf, UINT64 Count) {
    const CHAR *Src;
    CHAR Tmp[256];
    UINT64 Written = 0;

    if (Count == 0) {
        return 0;
    }

    if (!Buf || !SyscallIsUserRange(Buf, Count)) {
        return -INCORRECT_VALUE;
    }

    if (Fd == FD_STDOUT || Fd == FD_STDERR) {
        Src = (const CHAR *)(UINTPTR)Buf;
        while (Written < Count) {
            UINT64 Chunk = Count - Written;
            if (Chunk > sizeof(Tmp) - 1) {
                Chunk = sizeof(Tmp) - 1;
            }
	    asm volatile("stac" ::: "cc");
            MemCpy(Tmp, Src + Written, (USIZE)Chunk);
	    asm volatile("clac" ::: "cc");
            Tmp[Chunk] = '\0';
            ConsolePrint("%s", Tmp);
            Written += Chunk;
        }
        return (INT64)Written;
    }

    if (Fd >= FD_FIRST) {
        return SyscallFdWrite((INT)Fd, Buf, Count);
    }
    return -INCORRECT_VALUE;
}

/* ============================================================================
 * SyscallRead - Read from file descriptor
 * ============================================================================ */

static INT64 SyscallRead(INT64 Fd, UINT64 Buf, UINT64 Count) {
    UINT64 Written = 0;
    CHAR *Dst;

    if (Count == 0) {
        return 0;
    }
    if (!Buf || !SyscallIsUserRange(Buf, Count)) {
        return -INCORRECT_VALUE;
    }
    if (Fd == FD_STDIN) {
        Dst = (CHAR*)(UINTPTR)Buf;
        while (Written < Count) {
            INT Ch = ConsoleReadChar();
            if (Ch < 0) {
                if (Written > 0) {
                    break;
                }
                return -INCORRECT_VALUE;
            }

            if (Ch == 3) {
                return -2;
            }

            Dst[Written++] = (CHAR)Ch;
            if (Ch == '\n') {
               break;
            }
        }
        return (INT64)Written;
    }

    if (Fd >= FD_FIRST) {
        return SyscallFdRead((INT)Fd, Buf, Count);
    }
    return -INCORRECT_VALUE;

}

/* ============================================================================
 * SyscallGetPid - Get current process ID
 * ============================================================================ */

static INT64 SyscallGetPid(NOPTR) {
    return (INT64)SchedulerGetPid();
}

static INT64 SyscallGetPpid(NOPTR) {
    return (INT64)SchedulerGetParentPid();
}

static INT64 SyscallYield(NOPTR) {
    SchedulerYield();
    return SUCCESS;
}

static INT64 SyscallSleep(UINT64 Ms) {
    if (Ms == 0) {
        SchedulerYield();
        return SUCCESS;
    }
    TimerMdelay((UINT32)(Ms > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (UINT32)Ms));
    return SUCCESS;
}

static INT64 SyscallUname(UINT64 Buf, UINT64 BufSize) {
    static const CHAR Name[] = "TOS 0.04";
    USIZE Len;
    CHAR *Dst;

    if (!Buf || BufSize == 0) {
        return -INCORRECT_VALUE;
    }
    if (!SyscallIsUserRange(Buf, BufSize)) {
        return -INCORRECT_VALUE;
    }

    Len = StrLen(Name);
    if (BufSize < Len + 1) {
        return -INCORRECT_VALUE;
    }

    Dst = (CHAR*)(UINTPTR)Buf;
    MemCpy(Dst, Name, Len + 1);
    return (INT64)Len;
}

static INT64 SyscallOpen(UINT64 Path, UINT64 Flags) {
    if (!Path || !SyscallIsUserRange(Path, 1)) {
        return -INCORRECT_VALUE;
    }
    return SyscallFdOpen((const CHAR *)(UINTPTR)Path, (UINT32)Flags);
}

static INT64 SyscallClose(INT64 Fd) {
    return SyscallFdClose((INT)Fd);
}

static INT64 SyscallLseek(INT64 Fd, INT64 Offset, INT64 Whence) {
    return SyscallFdSeek((INT)Fd, Offset, (INT)Whence);
}

static INT64 SyscallStat(UINT64 Path, UINT64 Buf) {
    if (!Path || !Buf || !SyscallIsUserRange(Path, 1) ||
        !SyscallIsUserRange(Buf, sizeof(TosStat))) {
        return -INCORRECT_VALUE;
    }
    return SyscallFdStatPath((const CHAR *)(UINTPTR)Path, (TosStat *)(UINTPTR)Buf);
}

static INT64 SyscallGetcwd(UINT64 Buf, UINT64 Size) {
    if (!Buf || Size == 0 || !SyscallIsUserRange(Buf, Size)) {
        return -INCORRECT_VALUE;
    }
    return SyscallFdGetCwd((CHAR *)(UINTPTR)Buf, (USIZE)Size);
}

static INT64 SyscallChdir(UINT64 Path) {
    if (!Path || !SyscallIsUserRange(Path, 1)) {
        return -INCORRECT_VALUE;
    }
    return SyscallFdChdir((const CHAR *)(UINTPTR)Path);
}

static INT64 SyscallMkdir(UINT64 Path) {
    if (!Path || !SyscallIsUserRange(Path, 1)) {
        return -INCORRECT_VALUE;
    }
    return SyscallFdMkdir((const CHAR *)(UINTPTR)Path);
}

static INT64 SyscallUnlink(UINT64 Path) {
    if (!Path || !SyscallIsUserRange(Path, 1)) {
        return -INCORRECT_VALUE;
    }
    return SyscallFdUnlink((const CHAR *)(UINTPTR)Path);
}

static INT64 SyscallExec(UINT64 Path, UINT64 ArgStr) {
    CHAR KPath[PATH_MAX];
    CHAR KArgs[512];
    CHAR *Argv[16];
    INT Argc = 0;
    CHAR *Save;
    CHAR *Token;
    VfsFile *File;
    UINT8 *FileData;
    UINT64 FileSize;
    UINT32 Read;
    ElfLoadedImage Image;
    ElfLoadResult Result;
    const CHAR *UserPath = (const CHAR *)(UINTPTR)Path;

    if (!UserPath || !SyscallIsUserRange(Path, 1)) {
        return -INCORRECT_VALUE;
    }
    {
        USIZE I;
        for (I = 0; I < sizeof(KPath) - 1; I++) {
            KPath[I] = UserPath[I];
            if (UserPath[I] == '\0') {
                break;
            }
        }
        if (I >= sizeof(KPath) - 1) {
            return -INCORRECT_VALUE;
        }
    }

    KArgs[0] = '\0';
    if (ArgStr && SyscallIsUserRange(ArgStr, 1)) {
        const CHAR *UserArgs = (const CHAR *)(UINTPTR)ArgStr;
        USIZE I;
        for (I = 0; I < sizeof(KArgs) - 1; I++) {
            KArgs[I] = UserArgs[I];
            if (UserArgs[I] == '\0') break;
        }
    }

    if (VfsOpen(CurrentDir, KPath, O_READ, &File) != SUCCESS) {
        return -NOT_FOUND;
    }

    FileSize = File->FInode->ISize;
    if (FileSize == 0 || FileSize > 16 * 1024 * 1024) {
        VfsClose(File);
        return -INCORRECT_VALUE;
    }

    FileData = (UINT8*)MemoryAllocate((USIZE)FileSize);
    if (!FileData) {
        VfsClose(File);
        return -NO_MEMORY;
    }

    if (VfsRead(File, FileData, (UINT32)FileSize, &Read) != SUCCESS || Read != FileSize) {
        MemoryFree(FileData);
        VfsClose(File);
        return -IO_ERROR;
    }
    VfsClose(File);

    Result = ElfLoad(FileData, (USIZE)FileSize, &Image);
    MemoryFree(FileData);
    if (Result != ELF_LOAD_SUCCESS) {
        return -IO_ERROR;
    }

    Argv[Argc++] = KPath;
    Token = StrTokR(KArgs, " ", &Save);
    while (Token && Argc < 15) {
        Argv[Argc++] = Token;
        Token = StrTokR(NULLPTR, " ", &Save);
    }
    Argv[Argc] = NULLPTR;

    Image.ProgramName = MemoryAllocate(StrLen(KPath) + 1);
    if (Image.ProgramName) {
        StrCpy((CHAR*)Image.ProgramName, KPath);
    }

    Result = ElfExecute(&Image, Argc, Argv);
    ElfUnload(&Image);
    if (Result != ELF_LOAD_SUCCESS) {
        return -IO_ERROR;
    }
    return SUCCESS;
}

static INT64 SyscallWait(INT64 WaitPid, UINT64 StatusPtr) {
    INT32 Status = 0;
    INT Result = SchedulerWaitChild((UINT32)WaitPid, &Status);

    if (Result < 0) {
        return Result;
    }
    if (StatusPtr && SyscallIsUserRange(StatusPtr, sizeof(INT32))) {
        *(INT32 *)(UINTPTR)StatusPtr = Status;
    }
    return Result;
}

static INT64 SyscallKill(UINT64 Pid) {
    KTask *Self = SchedulerGetCurrent();
    KTask *Target;

    if (Pid == 0 || Pid == SchedulerGetPid()) {
        return -INCORRECT_VALUE;
    }

    if (!Self || !Self->Authenticated) {
        return -PERMISSION_DENIED;
    }

    Target = SchedulerFindTaskByPid((UINT32)Pid);
    if (!Target) {
        return -NOT_FOUND;
    }

    if (Self->Role != UserRoleAdmin) {
        if (Target->Uid != Self->Uid && Target->Euid != Self->Euid) {
            return -PERMISSION_DENIED;
        }
    }

    SchedulerKillTask((UINT32)Pid);
    return SUCCESS;
}

static INT64 SyscallGetUid(NOPTR) {
    KTask *Task = SchedulerGetCurrent();
    if (!Task || !Task->Authenticated) {
        return UID_NOBODY;
    }
    return (INT64)Task->Uid;
}

static INT64 SyscallGetGid(NOPTR) {
    KTask *Task = SchedulerGetCurrent();
    if (!Task || !Task->Authenticated) {
        return GID_NOBODY;
    }
    return (INT64)Task->Gid;
}

static INT64 SyscallGetEuid(NOPTR) {
    KTask *Task = SchedulerGetCurrent();
    if (!Task || !Task->Authenticated) {
        return UID_NOBODY;
    }
    return (INT64)Task->Euid;
}

static INT64 SyscallGetEgid(NOPTR) {
    KTask *Task = SchedulerGetCurrent();
    if (!Task || !Task->Authenticated) {
        return GID_NOBODY;
    }
    return (INT64)Task->Egid;
}

/* ============================================================================
 * SyscallDispatch - Main syscall dispatcher
 * ============================================================================ */

INT64 SyscallDispatch(SyscallFrame *Frame) {
    if (!Frame) {
        return -INCORRECT_VALUE;
    }

    switch (Frame->Rax) {
    case SYS_EXIT:
        return SyscallExit(Frame->Rdi);
    case SYS_WRITE:
        return SyscallWrite((INT64)Frame->Rdi, Frame->Rsi, Frame->Rdx);
    case SYS_GETPID:
        return SyscallGetPid();
    case SYS_YIELD:
        return SyscallYield();
    case SYS_READ:
        return SyscallRead((INT64)Frame->Rdi, Frame->Rsi, Frame->Rdx);
    case SYS_GETPPID:
        return SyscallGetPpid();
    case SYS_SLEEP:
        return SyscallSleep(Frame->Rdi);
    case SYS_UNAME:
        return SyscallUname(Frame->Rdi, Frame->Rsi);
    case SYS_OPEN:
        return SyscallOpen(Frame->Rdi, Frame->Rsi);
    case SYS_CLOSE:
        return SyscallClose((INT64)Frame->Rdi);
    case SYS_LSEEK:
        return SyscallLseek((INT64)Frame->Rdi, (INT64)Frame->Rsi, (INT64)Frame->Rdx);
    case SYS_STAT:
        return SyscallStat(Frame->Rdi, Frame->Rsi);
    case SYS_GETCWD:
        return SyscallGetcwd(Frame->Rdi, Frame->Rsi);
    case SYS_CHDIR:
        return SyscallChdir(Frame->Rdi);
    case SYS_MKDIR:
        return SyscallMkdir(Frame->Rdi);
    case SYS_UNLINK:
        return SyscallUnlink(Frame->Rdi);
    case SYS_EXEC:
        return SyscallExec(Frame->Rdi, Frame->Rsi);
    case SYS_WAIT:
        return SyscallWait((INT64)Frame->Rdi, Frame->Rsi);
    case SYS_KILL:
        return SyscallKill(Frame->Rdi);
    case SYS_GETUID:
        return SyscallGetUid();
    case SYS_GETGID:
        return SyscallGetGid();
    case SYS_GETEUID:
        return SyscallGetEuid();
    case SYS_GETEGID:
        return SyscallGetEgid();

    /* Network syscalls */
    case SYS_SOCKET:
        return SyscallSocket(Frame->Rdi, Frame->Rsi, Frame->Rdx);
    case SYS_CONNECT:
        return SyscallConnect((INT)Frame->Rdi, Frame->Rsi, Frame->Rdx);
    case SYS_BIND:
        return SyscallBind((INT)Frame->Rdi, Frame->Rsi, Frame->Rdx);
    case SYS_LISTEN:
        return SyscallListen((INT)Frame->Rdi, (INT)Frame->Rsi);
    case SYS_ACCEPT:
        return SyscallAccept((INT)Frame->Rdi, Frame->Rsi, Frame->Rdx);
    case SYS_SEND:
        return SyscallSend((INT)Frame->Rdi, Frame->Rsi, Frame->Rdx, 0);
    case SYS_RECV:
        return SyscallRecv((INT)Frame->Rdi, Frame->Rsi, Frame->Rdx, 0);
    case SYS_CLOSE_SOCKET:
        return SyscallCloseSocket((INT)Frame->Rdi);
    case SYS_GETADDRINFO:
        return SyscallGetAddrInfo(Frame->Rdi, Frame->Rsi, Frame->Rdx);

    default:
        return -NOT_IMPLEMENTED;
    }
}

/* ============================================================================
 * SyscallInit - Initialize syscall subsystem
 * ============================================================================ */

NOPTR SyscallInit(NOPTR) {
    UINT64 Sp;

    __asm__ volatile("mov %%rsp, %0" : "=r"(Sp));

    UINT64 Efer = ReadMSR(MSR_EFER);
    Efer |= EFER_SCE;
    WriteMSR(MSR_EFER, Efer);

    WriteMSR(MSR_STAR, SYSCALL_STAR);
    WriteMSR(MSR_LSTAR, (UINT64)(UINTPTR)SyscallEntry);
    WriteMSR(MSR_SFMASK, RFLAGS_IF);

    WriteMSR(MSR_KERNEL_GS_BASE, (UINT64)(UINTPTR)&GSyscallCpu);
    WriteMSR(MSR_GS_BASE, 0);

    GSyscallCpu.UserRsp = 0;
    GSyscallCpu.KernelRsp = Sp;

    SyscallSocketInit();
}