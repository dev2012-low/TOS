#include <Kernel/Scheduler.h>
#include <Kernel/UserAccount.h>
#include <Kernel/Task.h>
#include <Kernel/SpinLock.h>
#include <Kernel/List.h>
#include <Kernel/Return.h>
#include <Kernel/Paging.h>
#include <Kernel/SyscallFd.h>
#include <Memory/PhysAlloc.h>
#include <Memory/Allocator.h>
#include <Asm/Cpu.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Time/Timer.h>
#include <FBDevice.h>
#include <RgbColor.h>
#include <Console.h>

EXTERN(NOPTR, TssInit(KTask *));
EXTERN(NOPTR, TssSetRsp0(UINT64));
EXTERN(NOPTR, ElfEnterUserMode(UINT64 EntryPoint, UINT64 Rsp, UINT64 ArgvPtr, UINT64 CR3));

static ListHead GReadyQueues[SCHED_PRIORITY_LEVELS];
static ListHead GAllTasks;
static SpinLock GSchedulerLock;
static KTask *GCurrentTask;
static KTask *GIdleTask;
static volatile BOOL GReschedulePending;
static BOOL GSchedulerRunning;
static BOOL GSchedulerInitialized;
static UINT32 GNextPid = 1;           // PID 0 для idle
static SpinLock GPidLock;

static UINT32 GTotalProcesses = 0;
static UINT32 GUserProcessCount[USER_MAX_ACCOUNTS] = {0};  // Счетчик процессов по пользователям

UINT32 AllocPid(NOPTR) {
    UINT32 Pid;
    SpinLockAcquire(&GPidLock);
    Pid = GNextPid++;
    if (GNextPid == 0) GNextPid = 1;  // Переполнение — сброс (но это почти невозможно)
    SpinLockRelease(&GPidLock);
    return Pid;
}

NOPTR SchedulerEnqueueReady(KTask *Task) {
    if (!Task || Task->Priority >= SCHED_PRIORITY_LEVELS) {
        return;
    }
    // Terminated threads must never be re-queued (otherwise they "resurrect"
    // after returning from thread entry).
    if (Task->State == TaskStateTerminated) {
        return;
    }
    Task->State = TaskStateReady;
    ListAddTail(&GReadyQueues[Task->Priority], &Task->ReadyNode);
}

static KTask* SchedulerDequeueHighest(NOPTR) {
    INT P;

    for (P = SCHED_PRIORITY_LEVELS - 1; P >= 0; P--) {
        if (!ListEmpty(&GReadyQueues[P])) {
            ListHead *Node = GReadyQueues[P].Next;
            KTask *Task = ListEntry(Node, KTask, ReadyNode);
            ListDel(&Task->ReadyNode);
            return Task;
        }
    }
    return NULLPTR;
}

static BOOL SchedulerIsRunnable(KTask *Task) {
    if (!Task || Task == GIdleTask) {
        return FALSE;
    }
    if (Task->State == TaskStateTerminated || Task->State == TaskStateWaiting) {
        return FALSE;
    }
    return TRUE;
}

static NOPTR SchedulerRequeueCurrentIfRunnable(NOPTR) {
    if (!SchedulerIsRunnable(GCurrentTask)) {
        return;
    }
    GCurrentTask->Quantum = GCurrentTask->QuantumReset;
    SchedulerEnqueueReady(GCurrentTask);
}

static KTask* SchedulerDequeueHighestBelow(UINT8 PriorityExclusive) {
    INT P;

    if (PriorityExclusive == 0) {
        return NULLPTR;
    }

    for (P = (INT)PriorityExclusive - 1; P >= 0; P--) {
        if (!ListEmpty(&GReadyQueues[P])) {
            ListHead *Node = GReadyQueues[P].Next;
            KTask *Task = ListEntry(Node, KTask, ReadyNode);
            ListDel(&Task->ReadyNode);
            return Task;
        }
    }

    return NULLPTR;
}

static NOPTR SchedulerSwitchPageTable(KTask *Task) {
    if (Task && Task->PageTable) {
        PagingSwitch(Task->PageTable);
    } else {
        PagingSwitch(PagingGetKernelCR3());
    }
}

static NOPTR SchedulerDestroyUserTask(KTask *Task) {
    if (!Task || !Task->PageTable) {
        return;
    }

    PagingSwitch(PagingGetKernelCR3());

    if (Task->EntryArg) {
        MemoryFree(Task->EntryArg);
        Task->EntryArg = NULLPTR;
    }

    PagingDestroyUserTask(Task->PageTable);
    Task->PageTable = NULLPTR;
}

static NOPTR UserTaskEntry(NOPTR *Arg) {
    UserTaskContext *Ctx = (UserTaskContext *)Arg;
    KTask *Task = GCurrentTask;
    UINT64 Cr3Phys;

    if (!Ctx || !Task || !Task->PageTable) {
        SchedulerTerminate();
        return;
    }

    Cr3Phys = (UINT64)(UINTPTR)VirtToPhysPtr((NOPTR*)Task->PageTable);
    ElfEnterUserMode(Ctx->EntryPoint, Ctx->UserRsp, Ctx->ArgvPtr, Cr3Phys);

    for (;;) {
        Halt();
    }
}

static NOPTR SchedulerSwitchTo(KTask *Next) {
    KTask *Prev = GCurrentTask;
    KTask *NextTask = Next;
    KTask *PrevTask = Prev;

    if (!Next || Next == Prev) {
        return;
    }

    Next->State = TaskStateRunning;
    GCurrentTask = Next;
    TssSetRsp0(Next->KernelStackBase + Next->KernelStackSize);
    SchedulerSwitchPageTable(Next);

    if (Prev) {
        /* Передаём указатели на AVX состояние */
        SchedulerSwitchContext(
            &Prev->SavedStackPointer,
            Next->SavedStackPointer,
            (Prev->AvxEnabled) ? Prev->AvxState : NULLPTR,
            (Next->AvxEnabled) ? Next->AvxState : NULLPTR
        );
    }
    asm volatile("sti");
}

NOPTR SchedulerInit(NOPTR) {
    INT I;

    if (GSchedulerInitialized) {
        return;
    }

    for (I = 0; I < SCHED_PRIORITY_LEVELS; I++) {
        ListInit(&GReadyQueues[I]);
    }
    ListInit(&GAllTasks);
    SpinLockInit(&GSchedulerLock);
    GSchedulerInitialized = TRUE;
}

NOPTR SchedulerRegisterTask(KTask *Task) {
    if (!Task) {
        return;
    }
    SpinLockAcquire(&GSchedulerLock);
    ListAddTail(&GAllTasks, &Task->AllTasksNode);
    SpinLockRelease(&GSchedulerLock);
}

NOPTR SchedulerUnregisterTask(KTask *Task) {
    if (!Task) {
        return;
    }
    SpinLockAcquire(&GSchedulerLock);
    if (Task->AllTasksNode.Next && Task->AllTasksNode.Prev) {
        ListDel(&Task->AllTasksNode);
    }
    SpinLockRelease(&GSchedulerLock);
}

static KTask *SchedulerFindZombieChild(UINT32 ParentPid, UINT32 WaitPid) {
    ListHead *Pos;
    KTask *Found = NULLPTR;

    SpinLockAcquire(&GSchedulerLock);
    ListForEach(Pos, &GAllTasks) {
        KTask *T = ListEntry(Pos, KTask, AllTasksNode);
        if (T->ParentPid == ParentPid &&
            T->State == TaskStateTerminated &&
            !T->Reaped) {
            if (WaitPid == 0 || T->Pid == WaitPid) {
                Found = T;
                break;
            }
        }
    }
    SpinLockRelease(&GSchedulerLock);
    return Found;
}

INT SchedulerWaitChild(UINT32 WaitPid, INT32 *StatusOut) {
    KTask *Parent = SchedulerGetCurrent();
    UINT32 ParentPid = Parent ? Parent->Pid : 0;

    for (;;) {
        KTask *Child = SchedulerFindZombieChild(ParentPid, WaitPid);
        if (Child) {
            UINT32 Cpid = Child->Pid;
            if (StatusOut) {
                *StatusOut = Child->ExitCode;
            }
            Child->Reaped = TRUE;
            SyscallFdCloseAll(Child);
            SchedulerUnregisterTask(Child);
            TaskDestroy(Child);
            return (INT)Cpid;
        }
        SchedulerYield();
        TimerMdelay(5);
    }
}

static NOPTR IdleTaskEntry(NOPTR *Arg) {
    (NOPTR)Arg;
    for (;;) {
        LocalInterruptsEnable();
        Halt();
    }
}

INT SchedulerCreateIdleTask(NOPTR) {
    GIdleTask = TaskCreate("Idle", IdleTaskEntry, NULLPTR,
                               SCHED_PRIORITY_IDLE, TASK_DEFAULT_QUANTUM);
    if (!GIdleTask) {
        return NO_OBJECT;
    }
    SchedulerRegisterTask(GIdleTask);
    SchedulerEnqueueReady(GIdleTask);
    return SUCCESS;
}

INT SchedulerCreateTask(const CHAR *Name, TaskEntry Entry, NOPTR *Arg,
                          UINT8 Priority, INT32 Quantum) {
    KTask *Task;
    UINT32 Uid;

    if (!GSchedulerInitialized) {
        return NO_OBJECT;
    }
    if (Priority >= SCHED_PRIORITY_LEVELS) {
        return INCORRECT_VALUE;
    }

    if (GTotalProcesses >= MAX_PROCESSES) {
        ConsolePrint("The maximum number of processes in the current session has been exceeded.\n");
        return NO_MEMORY;
    }

    Uid = UserManagerGetSession()->Authenticated ? UserManagerGetSession()->Uid : UID_NOBODY;
    if (Uid < USER_MAX_ACCOUNTS && GUserProcessCount[Uid] >= MAX_PROCESSES_PER_USER) {
        ConsolePrint("The maximum number of processes in the current session has been exceeded.n");
        return NO_MEMORY;
    }

    Task = TaskCreate(Name, Entry, Arg, Priority, Quantum);
    if (!Task) {
        return NO_MEMORY;
    }

    GTotalProcesses++;
    if (Uid < USER_MAX_ACCOUNTS) {
        GUserProcessCount[Uid]++;
    }

    SchedulerRegisterTask(Task);

    SpinLockAcquire(&GSchedulerLock);
    SchedulerEnqueueReady(Task);
    SpinLockRelease(&GSchedulerLock);
    GReschedulePending = TRUE;
    return SUCCESS;
}

INT SchedulerCreateUserTask(const CHAR *Name, UINT64 *PageTable,
                            UINT64 EntryPoint, UINT64 UserRsp, UINT64 ArgvPtr,
                            UINT8 Priority, INT32 Quantum) {
    KTask *Task;
    UserTaskContext *Ctx;
    KTask *Parent;
    UINT32 Uid;

    if (!GSchedulerInitialized || !PageTable || !EntryPoint) {
        return INCORRECT_VALUE;
    }
    if (Priority >= SCHED_PRIORITY_LEVELS) {
        return INCORRECT_VALUE;
    }

    Ctx = (UserTaskContext*)MemoryAllocate(sizeof(UserTaskContext));
    if (!Ctx) {
        return NO_MEMORY;
    }

    Ctx->EntryPoint = EntryPoint;
    Ctx->UserRsp = UserRsp;
    Ctx->ArgvPtr = ArgvPtr;

    if (GTotalProcesses >= MAX_PROCESSES) {
        ConsolePrint("[Scheduler] Too many processes! (%u max)\n", MAX_PROCESSES);
        MemoryFree(Ctx);
        return NO_MEMORY;
    }

    Uid = UserManagerGetSession()->Authenticated ? UserManagerGetSession()->Uid : UID_NOBODY;
    if (Uid < USER_MAX_ACCOUNTS && GUserProcessCount[Uid] >= MAX_PROCESSES_PER_USER) {
        ConsolePrint("[Scheduler] User %u has too many processes (%u max)\n", 
                     Uid, MAX_PROCESSES_PER_USER);
        MemoryFree(Ctx);
        return NO_MEMORY;
    }

    Task = TaskCreate(Name, UserTaskEntry, (NOPTR*)Ctx, Priority, Quantum);
    if (!Task) {
        MemoryFree(Ctx);
        return NO_MEMORY;
    }

    GTotalProcesses++;
    if (Uid < USER_MAX_ACCOUNTS) {
        GUserProcessCount[Uid]++;
    }

    Parent = SchedulerGetCurrent();
    Task->ParentPid = Parent ? Parent->Pid : 0;
    Task->PageTable = PageTable;

    if (Parent && Parent->Authenticated) {
        Task->Uid = Parent->Uid;
        Task->Gid = Parent->Gid;
        Task->Euid = Parent->Euid;
        Task->Egid = Parent->Egid;
        Task->Role = Parent->Role;
        Task->Authenticated = TRUE;
    }
    SchedulerRegisterTask(Task);

    SpinLockAcquire(&GSchedulerLock);
    SchedulerEnqueueReady(Task);
    SpinLockRelease(&GSchedulerLock);
    GReschedulePending = TRUE;
    return SUCCESS;
}

NOPTR SchedulerStart(NOPTR) {
    KTask *Next;

    if (!GIdleTask) {
        return;
    }

    LocalInterruptsDisable();
    SpinLockAcquire(&GSchedulerLock);
    Next = SchedulerDequeueHighest();
    if (!Next) {
        Next = GIdleTask;
    }
    SpinLockRelease(&GSchedulerLock);

    GCurrentTask = Next;
    Next->State = TaskStateRunning;
    TssInit(Next);
    SchedulerSwitchPageTable(Next);
    GSchedulerRunning = TRUE;
    {
        static UINT64 BootstrapSp;
        SchedulerSwitchContext(&BootstrapSp, Next->SavedStackPointer, NULLPTR, NULLPTR);
    }
}

KTask* SchedulerGetCurrent(NOPTR) {
    return GCurrentTask;
}

NOPTR SchedulerYield(NOPTR) {
    KTask *Next;

    if (!GSchedulerRunning || !GCurrentTask) {
        return;
    }

    LocalInterruptsDisable();
    SpinLockAcquire(&GSchedulerLock);

    // Always requeue the current runnable task before picking the next one.
    // Skipping enqueue when quantum hits 0 orphans the task (shell stops
    // after long Format/Mount sequences that exhaust the time slice).
    if (SchedulerIsRunnable(GCurrentTask)) {
        GCurrentTask->Quantum = GCurrentTask->QuantumReset;
        SchedulerEnqueueReady(GCurrentTask);
    }

    Next = SchedulerDequeueHighest();
    if (!Next) {
        Next = GIdleTask;
    }

    // Prevent starvation of lower-priority threads if the current one
    // is again the highest ready thread.
    if (Next == GCurrentTask && GCurrentTask != GIdleTask) {
        KTask *Alt = SchedulerDequeueHighestBelow(GCurrentTask->Priority);
        if (Alt) {
            // Next was dequeued above; put the current task back before switching.
            SchedulerEnqueueReady(GCurrentTask);
            SpinLockRelease(&GSchedulerLock);
            SchedulerSwitchTo(Alt);
            return;
        }
        // No lower-priority work — park on idle, but keep current runnable.
        SchedulerEnqueueReady(GCurrentTask);
        Next = GIdleTask;
    }

    SpinLockRelease(&GSchedulerLock);
    SchedulerSwitchTo(Next);
}

NOPTR SchedulerOnTimer(NOPTR) {
    if (!GSchedulerRunning || !GCurrentTask) {
        return;
    }

    if (GCurrentTask->Quantum > 0) {
        GCurrentTask->Quantum--;
        if (GCurrentTask->Quantum <= 0) {
            GReschedulePending = TRUE;
        }
    }
}

NOPTR SchedulerRequestReschedule(NOPTR) {
    if (!GSchedulerRunning) {
        return;
    }
    GReschedulePending = TRUE;
    SchedulerCheckPreempt();
}

NOPTR SchedulerCheckPreempt(NOPTR) {
    KTask *Next;

    if (!GReschedulePending || !GSchedulerRunning) {
        return;
    }
    GReschedulePending = FALSE;

    SpinLockAcquire(&GSchedulerLock);
    SchedulerRequeueCurrentIfRunnable();

    Next = SchedulerDequeueHighest();
    if (!Next) {
        Next = GIdleTask;
    }

    if (Next == GCurrentTask) {
        // Avoid strict-priority starvation: if there is any lower ready thread,
        // switch to it instead of resuming the same one.
        if (GCurrentTask != GIdleTask) {
            KTask *Alt = SchedulerDequeueHighestBelow(GCurrentTask->Priority);
            if (Alt) {
                // Next==current means current was dequeued from its ready queue.
                SchedulerEnqueueReady(GCurrentTask);
                SpinLockRelease(&GSchedulerLock);
                SchedulerSwitchTo(Alt);
                return;
            }
        }

        if (GCurrentTask->State != TaskStateRunning) {
            GCurrentTask->State = TaskStateRunning;
        }
        GCurrentTask->Quantum = GCurrentTask->QuantumReset;
        SpinLockRelease(&GSchedulerLock);
        return;
    }

    SpinLockRelease(&GSchedulerLock);
    SchedulerSwitchTo(Next);
}

NOPTR SchedulerTerminate(NOPTR) {
    KTask *Task;

    if (!GCurrentTask) {
        for (;;) {
            Halt();
        }
    }

    Task = GCurrentTask;

    GTotalProcesses--;
    if (Task->Uid < USER_MAX_ACCOUNTS && GUserProcessCount[Task->Uid] > 0) {
        GUserProcessCount[Task->Uid]--;
    }

    Task->State = TaskStateTerminated;

    if (Task->PageTable) {
        SchedulerDestroyUserTask(Task);
    }

    SyscallFdCloseAll(Task);
    GReschedulePending = TRUE;
    SchedulerYield();

    for (;;) {
        Halt();
    }
}

NOPTR TaskEntryTrampoline(NOPTR) {
    KTask *Task = GCurrentTask;

    if (Task && Task->Entry) {
        Task->Entry(Task->EntryArg);
    }
    SchedulerTerminate();
}

NOPTR SchedulerSleep(SpinLock *Lock) {
    if (!GSchedulerRunning || !GCurrentTask) {
        if (Lock) {
            SpinLockRelease(Lock);
        }
        return;
    }

    LocalInterruptsDisable();
    SpinLockAcquire(&GSchedulerLock);

    GCurrentTask->State = TaskStateWaiting;
    GCurrentTask->WaitResult = FALSE;
    
    // Если передан lock, отпускаем его ПОСЛЕ захвата GSLock
    if (Lock) {
        SpinLockRelease(Lock);
    }

    KTask *Next = SchedulerDequeueHighest();
    if (!Next) Next = GIdleTask;

    SpinLockRelease(&GSchedulerLock);
    SchedulerSwitchTo(Next);  // <-- переключаем, пока TaskStateWaiting

    // Проснулись — возвращаемся сюда
    LocalInterruptsEnable();
}

NOPTR SchedulerWakeup(KTask *Task) {
    if (!Task || !GSchedulerRunning) return;

    LocalInterruptsDisable();
    SpinLockAcquire(&GSchedulerLock);

    if (Task->State == TaskStateWaiting) {
        Task->State = TaskStateReady;
        Task->WaitResult = TRUE;
        SchedulerEnqueueReady(Task);
        GReschedulePending = TRUE;
        
        // ПРИНУДИТЕЛЬНО переключаемся, если задача имеет 
        // более высокий или равный приоритет
        if (GCurrentTask && Task->Priority >= GCurrentTask->Priority) {
            // Убираем текущую задачу из очереди (если она там есть)
            if (GCurrentTask->ReadyNode.Next && GCurrentTask->ReadyNode.Prev) {
                ListDel(&GCurrentTask->ReadyNode);
            }
            // Добавляем её обратно в очередь, если она не завершена
            if (GCurrentTask->State != TaskStateTerminated) {
                SchedulerEnqueueReady(GCurrentTask);
            }
            
            // Переключаемся на пробуждённую задачу
            KTask *Next = Task;
            // Удаляем её из очереди (она там уже есть)
            if (Next->ReadyNode.Next && Next->ReadyNode.Prev) {
                ListDel(&Next->ReadyNode);
            }
            Next->State = TaskStateRunning;
            
            SpinLockRelease(&GSchedulerLock);
            SchedulerSwitchTo(Next);
            LocalInterruptsEnable();
            return;
        }
    }

    SpinLockRelease(&GSchedulerLock);
    LocalInterruptsEnable();
    SchedulerCheckPreempt();
}

BOOL SchedulerIsAwake(KTask *Task) {
    if (!Task) return FALSE;
    return Task->WaitResult;
}

UINT32 SchedulerGetPid(NOPTR) {
    KTask *Cur = SchedulerGetCurrent();
    return Cur ? Cur->Pid : 0;
}

UINT32 SchedulerGetParentPid(NOPTR) {
    KTask *Cur = SchedulerGetCurrent();
    return Cur ? Cur->ParentPid : 0;
}

KTask *SchedulerFindTaskByPid(UINT32 Pid) {
    SpinLockAcquire(&GSchedulerLock);
    
    // Проверяем текущую задачу
    if (GCurrentTask && GCurrentTask->Pid == Pid) {
        SpinLockRelease(&GSchedulerLock);
        return GCurrentTask;
    }
    
    // Ищем в очередях
    for (INT P = 0; P < SCHED_PRIORITY_LEVELS; P++) {
        ListHead *Pos;
        ListForEach(Pos, &GReadyQueues[P]) {
            KTask *T = ListEntry(Pos, KTask, ReadyNode);
            if (T->Pid == Pid) {
                SpinLockRelease(&GSchedulerLock);
                return T;
            }
        }
    }
    
    SpinLockRelease(&GSchedulerLock);
    return NULLPTR;
}

NOPTR SchedulerKillTask(UINT32 Pid) {
    KTask *T = SchedulerFindTaskByPid(Pid);
    if (!T) {
        ConsolePrint("Task with PID %u not found!\n", Pid);
        return;
    }
    
    if (Pid == 0) {
        ConsolePrint("Cannot kill this task!\n");
        return;
    }

    GTotalProcesses--;
    if (T->Uid < USER_MAX_ACCOUNTS && GUserProcessCount[T->Uid] > 0) {
        GUserProcessCount[T->Uid]--;
    }
    
    ConsolePrint("Killing task %u (%s)...\n", Pid, T->Name);
    
    // Прямое убийство (без вызова TaskExit)
    SpinLockAcquire(&GSchedulerLock);
    T->State = TaskStateTerminated;
    
    // Удаляем из очереди, если была
    if (T->ReadyNode.Next && T->ReadyNode.Prev) {
        ListDel(&T->ReadyNode);
    }
    
    // Освобождаем ресурсы userspace-задачи
    if (T->PageTable) {
        SpinLockRelease(&GSchedulerLock);
        SchedulerDestroyUserTask(T);
        SpinLockAcquire(&GSchedulerLock);
    }

    // Освобождаем стек
    if (T->KernelStackBase) {
        SpinLockRelease(&GSchedulerLock);
        TaskFreeKernelStack(T);
        SpinLockAcquire(&GSchedulerLock);
    }
    
    SpinLockRelease(&GSchedulerLock);
    
    SchedulerRequestReschedule();
}

NOPTR SchedulerChangeTaskName(UINT32 Pid, const CHAR *NewName) {
    KTask *T = SchedulerFindTaskByPid(Pid);
    if (!T) {
        ConsolePrint("\033[31mTask with PID\033[0m %u \033[31mnot found!\033[0m\n", Pid);
        return;
    }
    
    StrnCpy(T->Name, NewName, TASK_NAME_MAX - 1);
    ConsolePrint("Task %u renamed to '%s'\n", Pid, T->Name);
}

NOPTR SchedulerChangeTaskPriority(UINT32 Pid, UINT8 NewPriority) {
    if (NewPriority >= SCHED_PRIORITY_LEVELS) {
        ConsolePrint("\033[31mInvalid priority!\033[0m Must be 0-%d\n", SCHED_PRIORITY_LEVELS - 1);
        return;
    }
    
    KTask *T = SchedulerFindTaskByPid(Pid);
    if (!T) {
        ConsolePrint("\033[31mTask with PID\033[0m %u \033[31mnot found!\033[0m\n", Pid);
        return;
    }
    
    T->Priority = NewPriority;
    T->BasePriority = NewPriority;
    ConsolePrint("Task %u priority changed to %u\n", Pid, NewPriority);
    
    // Перепланируем
    SchedulerRequestReschedule();
}

static const CHAR* TaskStateToString(TaskState State) {
    switch (State) {
        case TaskStateReady:     return "READY";
        case TaskStateRunning:   return "RUNNING";
        case TaskStateWaiting:   return "WAITING";
        case TaskStateTerminated:return "TERMINATED";
        default:                 return "UNKNOWN";
    }
}

static char GTaskListBuffer[8192];

NOPTR SchedulerListTasks(NOPTR) {
    CHAR *Ptr = GTaskListBuffer;
    USIZE Remaining = sizeof(GTaskListBuffer);
    INT Printed;
    
    #define APPEND(FMT, ...) do { \
        Printed = SnPrintf(Ptr, Remaining, FMT, ##__VA_ARGS__); \
        if (Printed > 0 && (USIZE)Printed < Remaining) { \
            Ptr += Printed; \
            Remaining -= Printed; \
        } else { \
            goto flush; \
        } \
    } while(0)
    
    APPEND("\n");
    APPEND("  PID | Prio | State        | Name\n");
    APPEND("------+------+--------------+------------------\n");
    
    SpinLockAcquire(&GSchedulerLock);
    
    if (GCurrentTask) {
        APPEND(" %4u |  %2u  | %-12s | %s *\n",
               GCurrentTask->Pid,
               GCurrentTask->Priority,
               TaskStateToString(GCurrentTask->State),
               GCurrentTask->Name);
    }
    
    for (INT P = 0; P < SCHED_PRIORITY_LEVELS; P++) {
        ListHead *Pos;
        ListForEach(Pos, &GReadyQueues[P]) {
            KTask *T = ListEntry(Pos, KTask, ReadyNode);
            if (T != GCurrentTask) {
                APPEND(" %4u |  %2u  | %-12s | %s\n",
                       T->Pid, T->Priority,
                       TaskStateToString(T->State),
                       T->Name);
            }
        }
    }
    
    SpinLockRelease(&GSchedulerLock);
    
    APPEND("-----+------+--------------+------------------\n");
    APPEND(" * = current task\n\n");
    
flush:
    if (Ptr > GTaskListBuffer) {
        *Ptr = '\0';
        ConsolePrint("%s", GTaskListBuffer);
    }
    
    #undef APPEND
}
