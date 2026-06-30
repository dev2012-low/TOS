#pragma once

#include <Kernel/Types.h>

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

#define TOS_UNAME_LEN 32
#define TOS_O_RDONLY  1
#define TOS_O_WRONLY  2
#define TOS_O_RDWR    3
#define TOS_O_CREAT   4
#define TOS_O_TRUNC   8
#define TOS_O_APPEND  16

#define TOS_SEEK_SET  0
#define TOS_SEEK_CUR  1
#define TOS_SEEK_END  2

#define TOS_FT_REG    1
#define TOS_FT_DIR    2

typedef struct TosStat {
    UINT32 StMode;
    UINT32 StUid;
    UINT32 StGid;
    UINT64 StSize;
    UINT64 StMtime;
    UINT64 StIno;
} TosStat;

static inline INT64 Syscall0(UINT64 Nr) {
    INT64 Ret;
    __asm__ volatile(
        "syscall"
        : "=a"(Ret)
        : "a"(Nr)
        : "rcx", "r11", "memory");
    return Ret;
}

static inline INT64 Syscall1(UINT64 Nr, UINT64 A0) {
    INT64 Ret;
    __asm__ volatile(
        "syscall"
        : "=a"(Ret)
        : "a"(Nr), "D"(A0)
        : "rcx", "r11", "memory");
    return Ret;
}

static inline INT64 Syscall2(UINT64 Nr, UINT64 A0, UINT64 A1) {
    INT64 Ret;
    __asm__ volatile(
        "syscall"
        : "=a"(Ret)
        : "a"(Nr), "D"(A0), "S"(A1)
        : "rcx", "r11", "memory");
    return Ret;
}

static inline INT64 Syscall3(UINT64 Nr, UINT64 A0, UINT64 A1, UINT64 A2) {
    INT64 Ret;
    __asm__ volatile(
        "syscall"
        : "=a"(Ret)
        : "a"(Nr), "D"(A0), "S"(A1), "d"(A2)
        : "rcx", "r11", "memory");
    return Ret;
}

static inline NOPTR SyscallExit(UINT64 Code) {
    Syscall1(SYS_EXIT, Code);
    for (;;) {
        __asm__ volatile("pause");
    }
}

static inline INT64 SyscallWrite(INT64 Fd, const NOPTR *Buf, USIZE Count) {
    return Syscall3(SYS_WRITE, (UINT64)Fd, (UINT64)(UINTPTR)Buf, (UINT64)Count);
}

static inline INT64 SyscallRead(INT64 Fd, NOPTR *Buf, USIZE Count) {
    return Syscall3(SYS_READ, (UINT64)Fd, (UINT64)(UINTPTR)Buf, (UINT64)Count);
}

static inline INT64 SyscallGetPid(NOPTR) {
    return Syscall0(SYS_GETPID);
}

static inline INT64 SyscallGetPpid(NOPTR) {
    return Syscall0(SYS_GETPPID);
}

static inline INT64 SyscallYield(NOPTR) {
    return Syscall0(SYS_YIELD);
}

static inline INT64 SyscallSleep(UINT64 Ms) {
    return Syscall1(SYS_SLEEP, Ms);
}

static inline INT64 SyscallUname(CHAR *Buf, USIZE BufSize) {
    return Syscall2(SYS_UNAME, (UINT64)(UINTPTR)Buf, (UINT64)BufSize);
}

static inline INT64 SyscallOpen(const CHAR *Path, UINT64 Flags) {
    return Syscall2(SYS_OPEN, (UINT64)(UINTPTR)Path, Flags);
}

static inline INT64 SyscallClose(INT64 Fd) {
    return Syscall1(SYS_CLOSE, (UINT64)Fd);
}

static inline INT64 SyscallLseek(INT64 Fd, INT64 Offset, INT64 Whence) {
    return Syscall3(SYS_LSEEK, (UINT64)Fd, (UINT64)Offset, (UINT64)Whence);
}

static inline INT64 SyscallStat(const CHAR *Path, TosStat *Stat) {
    return Syscall2(SYS_STAT, (UINT64)(UINTPTR)Path, (UINT64)(UINTPTR)Stat);
}

static inline INT64 SyscallGetcwd(CHAR *Buf, USIZE Size) {
    return Syscall2(SYS_GETCWD, (UINT64)(UINTPTR)Buf, (UINT64)Size);
}

static inline INT64 SyscallChdir(const CHAR *Path) {
    return Syscall1(SYS_CHDIR, (UINT64)(UINTPTR)Path);
}

static inline INT64 SyscallMkdir(const CHAR *Path) {
    return Syscall1(SYS_MKDIR, (UINT64)(UINTPTR)Path);
}

static inline INT64 SyscallUnlink(const CHAR *Path) {
    return Syscall1(SYS_UNLINK, (UINT64)(UINTPTR)Path);
}

static inline INT64 SyscallExec(const CHAR *Path, const CHAR *Args) {
    return Syscall2(SYS_EXEC, (UINT64)(UINTPTR)Path, (UINT64)(UINTPTR)Args);
}

static inline INT64 SyscallWait(INT64 Pid, INT32 *Status) {
    return Syscall2(SYS_WAIT, (UINT64)Pid, (UINT64)(UINTPTR)Status);
}

static inline INT64 SyscallKill(UINT64 Pid) {
    return Syscall1(SYS_KILL, Pid);
}

static inline INT64 SyscallGetUid(NOPTR) {
    return Syscall0(SYS_GETUID);
}

static inline INT64 SyscallGetGid(NOPTR) {
    return Syscall0(SYS_GETGID);
}

static inline INT64 SyscallGetEuid(NOPTR) {
    return Syscall0(SYS_GETEUID);
}

static inline INT64 SyscallGetEgid(NOPTR) {
    return Syscall0(SYS_GETEGID);
}
