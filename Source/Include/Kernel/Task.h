#pragma once

#include <Kernel/Types.h>
#include <Kernel/List.h>
#include <Kernel/SpinLock.h>
#include <Kernel/UserAccount.h>
#include <Fs/Vfs.h>

#define TASK_KERNEL_STACK_SIZE   (32 * 1024)
#define TASK_NAME_MAX            16
#define TASK_DEFAULT_QUANTUM     4
#define TASK_MAX_FD              32
#define FD_STDIN                 0
#define FD_STDOUT                1
#define FD_STDERR                2
#define FD_FIRST                 3

typedef enum {
    TaskStateReady = 0,
    TaskStateRunning,
    TaskStateWaiting,
    TaskStateTerminated
} TaskState;

typedef struct KTask {
    UINT32 Pid;
    UINT32 ParentPid;
    INT32 ExitCode;
    ListHead ReadyNode;
    CHAR Name[TASK_NAME_MAX];
    TaskState State;
    UINT8 Priority;
    UINT8 BasePriority;
    INT32 Quantum;
    INT32 QuantumReset;
    UINT64 SavedStackPointer;
    UINT64 KernelStackBase;
    UINT64 KernelStackSize;
    NOPTR (*Entry)(NOPTR *Arg);
    NOPTR *EntryArg;
    volatile BOOL WaitResult;
    SpinLock WaitLock;
    UINT64 *PageTable;
    UINT8 AvxState[512] ATTRIBUTE(aligned(64));
    BOOL AvxEnabled;
    VfsFile *OpenFiles[TASK_MAX_FD];
    ListHead AllTasksNode;
    BOOL Reaped;

    UINT32 Uid;
    UINT32 Gid;
    UINT32 Euid;
    UINT32 Egid;
    UserRole Role;
    BOOL Authenticated;
} KTask;

typedef NOPTR (*TaskEntry)(NOPTR *Arg);

typedef struct UserTaskContext {
    UINT64 EntryPoint;
    UINT64 UserRsp;
    UINT64 ArgvPtr;
} UserTaskContext;

KTask* TaskCreate(const CHAR *Name, TaskEntry Entry, NOPTR *Arg,
                      UINT8 Priority, INT32 Quantum);
NOPTR TaskDestroy(KTask *Task);
NOPTR TaskFreeKernelStack(KTask *Task);
NOPTR TaskInitStack(KTask *Task);
NOPTR TaskEntryTrampoline(NOPTR);
NOPTR TaskExit(INT32 ExitCode);
