#pragma once

#include <Kernel/Types.h>
#include <Kernel/Task.h>

/*
 * 31 priority levels: 0 (idle) .. 30 (highest).
 * Windows-style: higher number = higher priority.
 */
#define SCHED_PRIORITY_LEVELS      31
#define SCHED_PRIORITY_IDLE        0
#define SCHED_PRIORITY_NORMAL      8
#define SCHED_PRIORITY_HIGH        16
#define SCHED_PRIORITY_REALTIME    30

#define MAX_PROCESSES          32768   // Максимум процессов в системе
#define MAX_PROCESSES_PER_USER 4096    // Максимум процессов на одного пользователя

NOPTR SchedulerInit(NOPTR);
NOPTR SchedulerStart(NOPTR);
KTask* SchedulerGetCurrent(NOPTR);
NOPTR SchedulerYield(NOPTR);
NOPTR SchedulerOnTimer(NOPTR);
NOPTR SchedulerCheckPreempt(NOPTR);
NOPTR SchedulerRequestReschedule(NOPTR);
NOPTR SchedulerTerminate(NOPTR);
NOPTR SchedulerSleep(SpinLock *Lock);
NOPTR SchedulerWakeup(KTask *Task);
BOOL SchedulerIsAwake(KTask *Task);

NOPTR SchedulerEnqueueReady(KTask *Task);

NOPTR SchedulerSwitchContext(UINT64 *SaveStackPointer, UINT64 NewStackPointer,
                              NOPTR *SaveAvxState, NOPTR *RestoreAvxState);;

INT SchedulerCreateIdleTask(NOPTR);
INT SchedulerCreateTask(const CHAR *Name, TaskEntry Entry, NOPTR *Arg,
                          UINT8 Priority, INT32 Quantum);
INT SchedulerCreateUserTask(const CHAR *Name, UINT64 *PageTable,
                            UINT64 EntryPoint, UINT64 UserRsp, UINT64 ArgvPtr,
                            UINT8 Priority, INT32 Quantum);

UINT32 SchedulerGetPid(NOPTR);
UINT32 SchedulerGetParentPid(NOPTR);

KTask *SchedulerFindTaskByPid(UINT32 Pid);
NOPTR SchedulerKillTask(UINT32 Pid);
NOPTR SchedulerChangeTaskName(UINT32 Pid, const CHAR *NewName);
NOPTR SchedulerChangeTaskPriority(UINT32 Pid, UINT8 NewPriority);
INT SchedulerWaitChild(UINT32 WaitPid, INT32 *StatusOut);
NOPTR SchedulerRegisterTask(KTask *Task);
NOPTR SchedulerUnregisterTask(KTask *Task);
NOPTR SchedulerListTasks(NOPTR);
