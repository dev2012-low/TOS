#pragma once

#include <Kernel/Types.h>
#include <Fs/Vfs.h>
#include <Kernel/Task.h>

#define TOS_SEEK_SET 0
#define TOS_SEEK_CUR 1
#define TOS_SEEK_END 2

typedef struct TosStat {
    UINT32 StMode;
    UINT32 StUid;
    UINT32 StGid;
    UINT64 StSize;
    UINT64 StMtime;
    UINT64 StIno;
} TosStat;

NOPTR SyscallFdInitTask(KTask *Task);
NOPTR SyscallFdCloseAll(KTask *Task);
INT SyscallFdOpen(const CHAR *Path, UINT32 Flags);
INT SyscallFdClose(INT Fd);
INT SyscallFdRead(INT Fd, UINT64 Buf, UINT64 Count);
INT SyscallFdWrite(INT Fd, UINT64 Buf, UINT64 Count);
INT SyscallFdSeek(INT Fd, INT64 Offset, INT Whence);
INT SyscallFdStatPath(const CHAR *Path, TosStat *Out);
INT SyscallFdStatFd(INT Fd, TosStat *Out);
INT SyscallFdGetCwd(CHAR *Buf, USIZE Size);
INT SyscallFdChdir(const CHAR *Path);
INT SyscallFdMkdir(const CHAR *Path);
INT SyscallFdUnlink(const CHAR *Path);
BOOL SyscallIsUserRange(UINT64 Ptr, UINT64 Size);