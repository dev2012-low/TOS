#include <Kernel/SyscallFd.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Task.h>
#include <Kernel/Return.h>
#include <Lib/String.h>
#include <Fs/Vfs.h>

#define USER_ADDR_MAX 0x00007FFFFFFFFFFFULL

BOOL SyscallIsUserRange(UINT64 Ptr, UINT64 Size) {
    UINT64 End;

    if (Size == 0) {
        return TRUE;
    }
    if (Ptr == 0) {
        return FALSE;
    }
    if (Ptr > USER_ADDR_MAX) {
        return FALSE;
    }
    End = Ptr + Size - 1;
    if (End < Ptr || End > USER_ADDR_MAX) {
        return FALSE;
    }
    return TRUE;
}

static INT CopyUserString(const CHAR *UserPath, CHAR *KernelBuf, USIZE Size) {
    USIZE I;

    if (!UserPath || !KernelBuf || Size == 0) {
        return INCORRECT_VALUE;
    }

    for (I = 0; I < Size - 1; I++) {
        KernelBuf[I] = UserPath[I];
        if (UserPath[I] == '\0') {
            return SUCCESS;
        }
    }
    return INCORRECT_VALUE;
}

static INT CopyUserStruct(const NOPTR *UserBuf, NOPTR *KernelBuf, USIZE Size) {
    USIZE I;
    const UINT8 *Src = (const UINT8 *)(UINTPTR)UserBuf;
    UINT8 *Dst = (UINT8 *)(UINTPTR)KernelBuf;

    if (!UserBuf || !KernelBuf || Size == 0) {
        return INCORRECT_VALUE;
    }

    for (I = 0; I < Size; I++) {
        Dst[I] = Src[I];
    }
    return SUCCESS;
}

static INT WriteUserStruct(NOPTR *UserBuf, const NOPTR *KernelBuf, USIZE Size) {
    USIZE I;
    UINT8 *Dst = (UINT8 *)(UINTPTR)UserBuf;
    const UINT8 *Src = (const UINT8 *)(UINTPTR)KernelBuf;

    if (!UserBuf || !KernelBuf || Size == 0) {
        return INCORRECT_VALUE;
    }

    for (I = 0; I < Size; I++) {
        Dst[I] = Src[I];
    }
    return SUCCESS;
}

static KTask *SyscallCurrentTask(NOPTR) {
    return SchedulerGetCurrent();
}

NOPTR SyscallFdInitTask(KTask *Task) {
    INT I;

    if (!Task) {
        return;
    }
    for (I = 0; I < TASK_MAX_FD; I++) {
        Task->OpenFiles[I] = NULLPTR;
    }
}

NOPTR SyscallFdCloseAll(KTask *Task) {
    INT I;

    if (!Task) {
        return;
    }
    for (I = FD_FIRST; I < TASK_MAX_FD; I++) {
        if (Task->OpenFiles[I]) {
            VfsClose(Task->OpenFiles[I]);
            Task->OpenFiles[I] = NULLPTR;
        }
    }
}

static INT SyscallFdAlloc(KTask *Task, VfsFile *File) {
    INT I;

    for (I = FD_FIRST; I < TASK_MAX_FD; I++) {
        if (!Task->OpenFiles[I]) {
            Task->OpenFiles[I] = File;
            return I;
        }
    }
    return -BUSY;
}

INT SyscallFdOpen(const CHAR *Path, UINT32 Flags) {
    KTask *Task = SyscallCurrentTask();
    CHAR KPath[PATH_MAX];
    VfsFile *File;
    INT Fd;

    if (!Task || !Path) {
        return INCORRECT_VALUE;
    }
    if (CopyUserString(Path, KPath, sizeof(KPath)) != SUCCESS) {
        return INCORRECT_VALUE;
    }
    if (VfsOpen(CurrentDir, KPath, Flags, &File) != SUCCESS) {
        return NOT_FOUND;
    }
    Fd = SyscallFdAlloc(Task, File);
    if (Fd < 0) {
        VfsClose(File);
        return Fd;
    }
    return Fd;
}

INT SyscallFdClose(INT Fd) {
    KTask *Task = SyscallCurrentTask();

    if (!Task || Fd < FD_FIRST || Fd >= TASK_MAX_FD) {
        return INCORRECT_VALUE;
    }
    if (!Task->OpenFiles[Fd]) {
        return NO_OBJECT;
    }
    VfsClose(Task->OpenFiles[Fd]);
    Task->OpenFiles[Fd] = NULLPTR;
    return SUCCESS;
}

INT SyscallFdRead(INT Fd, UINT64 Buf, UINT64 Count) {
    KTask *Task = SyscallCurrentTask();
    VfsFile *File;
    UINT32 Read;
    CHAR Tmp[512];
    UINT64 Done = 0;
    INT Result;

    if (!Task || !Buf || Count == 0) {
        return 0;
    }

    if (Fd < FD_FIRST || Fd >= TASK_MAX_FD) {
        return INCORRECT_VALUE;
    }
    File = Task->OpenFiles[Fd];
    if (!File) {
        return NO_OBJECT;
    }

    while (Done < Count) {
        UINT32 Chunk = (UINT32)((Count - Done) > sizeof(Tmp) ? sizeof(Tmp) : (Count - Done));
        Result = VfsRead(File, Tmp, Chunk, &Read);
        if (Result != SUCCESS || Read == 0) {
            break;
        }
        if (WriteUserStruct((NOPTR *)(UINTPTR)(Buf + Done), (NOPTR *)Tmp, Read) != SUCCESS) {
            return INCORRECT_VALUE;
        }
        Done += Read;
    }
    return (INT)Done;
}

INT SyscallFdWrite(INT Fd, UINT64 Buf, UINT64 Count) {
    KTask *Task = SyscallCurrentTask();
    VfsFile *File;
    UINT32 Written;
    CHAR Tmp[512];
    UINT64 Done = 0;
    INT Result;

    if (!Task || !Buf || Count == 0) {
        return 0;
    }

    if (Fd < FD_FIRST || Fd >= TASK_MAX_FD) {
        return INCORRECT_VALUE;
    }
    File = Task->OpenFiles[Fd];
    if (!File) {
        return NO_OBJECT;
    }

    while (Done < Count) {
        UINT32 Chunk = (UINT32)((Count - Done) > sizeof(Tmp) ? sizeof(Tmp) : (Count - Done));
        if (CopyUserStruct((NOPTR *)(UINTPTR)(Buf + Done), (NOPTR *)Tmp, Chunk) != SUCCESS) {
            return INCORRECT_VALUE;
        }
        Result = VfsWrite(File, Tmp, Chunk, &Written);
        if (Result != SUCCESS) {
            return Result;
        }
        Done += Written;
        if (Written < Chunk) {
            break;
        }
    }
    return (INT)Done;
}

INT SyscallFdSeek(INT Fd, INT64 Offset, INT Whence) {
    KTask *Task = SyscallCurrentTask();
    VfsFile *File;

    if (!Task || Fd < FD_FIRST || Fd >= TASK_MAX_FD) {
        return INCORRECT_VALUE;
    }
    File = Task->OpenFiles[Fd];
    if (!File) {
        return NO_OBJECT;
    }
    return VfsSeek(File, (UINT64)Offset, Whence);
}

static INT VfsStatToTos(const VfsStatS *Vs, TosStat *Out) {
    if (!Vs || !Out) {
        return INCORRECT_VALUE;
    }
    Out->StMode = Vs->StMode;
    Out->StUid = Vs->StUid;
    Out->StGid = Vs->StGid;
    Out->StSize = Vs->StSize;
    Out->StMtime = Vs->StMtime;
    Out->StIno = Vs->StIno;
    return SUCCESS;
}

INT SyscallFdStatPath(const CHAR *Path, TosStat *Out) {
    CHAR KPath[PATH_MAX];
    VfsInode *Node;
    VfsStatS Vs;
    TosStat Ts;

    if (!Path || !Out) {
        return INCORRECT_VALUE;
    }
    if (CopyUserString(Path, KPath, sizeof(KPath)) != SUCCESS) {
        return INCORRECT_VALUE;
    }
    if (VfsWalk(CurrentDir, KPath, &Node) != SUCCESS) {
        return NOT_FOUND;
    }
    if (VfsStat(Node, &Vs) != SUCCESS) {
        VfsInodeUnref(Node);
        return IO_ERROR;
    }
    VfsInodeUnref(Node);
    VfsStatToTos(&Vs, &Ts);
    if (WriteUserStruct((NOPTR *)Out, (NOPTR *)&Ts, sizeof(Ts)) != SUCCESS) {
        return INCORRECT_VALUE;
    }
    return SUCCESS;
}

INT SyscallFdStatFd(INT Fd, TosStat *Out) {
    KTask *Task = SyscallCurrentTask();
    VfsFile *File;
    VfsStatS Vs;
    TosStat Ts;

    if (!Task || !Out) {
        return INCORRECT_VALUE;
    }
    if (Fd < FD_FIRST || Fd >= TASK_MAX_FD) {
        return INCORRECT_VALUE;
    }
    File = Task->OpenFiles[Fd];
    if (!File || !File->FInode) {
        return NO_OBJECT;
    }
    if (VfsStat(File->FInode, &Vs) != SUCCESS) {
        return IO_ERROR;
    }
    VfsStatToTos(&Vs, &Ts);
    if (WriteUserStruct((NOPTR *)Out, (NOPTR *)&Ts, sizeof(Ts)) != SUCCESS) {
        return INCORRECT_VALUE;
    }
    return SUCCESS;
}

INT SyscallFdGetCwd(CHAR *Buf, USIZE Size) {
    CHAR Path[PATH_MAX];

    if (!Buf || Size == 0) {
        return INCORRECT_VALUE;
    }
    if (VfsBuildPath(CurrentDir, Path, PATH_MAX) != SUCCESS) {
        StrnCpy(Path, CurrentPath, PATH_MAX - 1);
    }
    if (StrLen(Path) + 1 > Size) {
        return INCORRECT_VALUE;
    }
    if (WriteUserStruct((NOPTR *)Buf, (NOPTR *)Path, StrLen(Path) + 1) != SUCCESS) {
        return INCORRECT_VALUE;
    }
    return (INT)StrLen(Path);
}

INT SyscallFdChdir(const CHAR *Path) {
    CHAR KPath[PATH_MAX];
    VfsInode *NewDir;

    if (!Path) {
        return INCORRECT_VALUE;
    }
    if (CopyUserString(Path, KPath, sizeof(KPath)) != SUCCESS) {
        return INCORRECT_VALUE;
    }
    NewDir = CurrentDir;
    if (VfsCd(&NewDir, KPath) != SUCCESS) {
        return NOT_FOUND;
    }
    return SUCCESS;
}

INT SyscallFdMkdir(const CHAR *Path) {
    CHAR KPath[PATH_MAX];
    VfsInode *Result;

    if (!Path) {
        return INCORRECT_VALUE;
    }
    if (CopyUserString(Path, KPath, sizeof(KPath)) != SUCCESS) {
        return INCORRECT_VALUE;
    }
    if (VfsMkdir(CurrentDir, KPath, FT_DIR, &Result) != SUCCESS) {
        return IO_ERROR;
    }
    if (Result) {
        VfsInodeUnref(Result);
    }
    return SUCCESS;
}

INT SyscallFdUnlink(const CHAR *Path) {
    CHAR KPath[PATH_MAX];

    if (!Path) {
        return INCORRECT_VALUE;
    }
    if (CopyUserString(Path, KPath, sizeof(KPath)) != SUCCESS) {
        return INCORRECT_VALUE;
    }
    return VfsUnlink(CurrentDir, KPath);
}
