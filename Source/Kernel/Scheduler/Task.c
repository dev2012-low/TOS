#include <Kernel/Task.h>
#include <Kernel/Scheduler.h>
#include <Memory/PhysAlloc.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Kernel/Idt.h>
#include <Kernel/Syscall.h>
#include <Kernel/SyscallFd.h>
#include <Kernel/CpuFeatures.h>
#include <Kernel/Paging.h>
#include <Asm/Cpu.h>

EXTERN(NOPTR, TaskEntryTrampoline(NOPTR));

static UINT32 TaskStackPageCount(KTask *Task) {
    if (!Task || Task->KernelStackSize == 0) {
        return 0;
    }
    return Task->KernelStackSize / PAGE_SIZE;
}

NOPTR TaskFreeKernelStack(KTask *Task) {
    UINT32 Pages;

    if (!Task || !Task->KernelStackBase) {
        return;
    }

    Pages = TaskStackPageCount(Task);
    if (Pages == 0) {
        return;
    }

    PhysAllocFreeRange(PhysAllocGet(), (NOPTR*)(UINTPTR)Task->KernelStackBase, Pages);
    Task->KernelStackBase = 0;
}

NOPTR TaskInitStack(KTask *Task) {
    UINT64 *Sp;

    if (!Task->KernelStackBase) {
        return;
    }

    Sp = (UINT64 *)(Task->KernelStackBase + Task->KernelStackSize);

    *--Sp = (UINT64)TaskEntryTrampoline;
    *--Sp = 0x202;
    *--Sp = 0; /* r15 */
    *--Sp = 0; /* r14 */
    *--Sp = 0; /* r13 */
    *--Sp = 0; /* r12 */
    *--Sp = 0; /* rbx */
    *--Sp = 0; /* rbp */

    Task->SavedStackPointer = (UINT64)Sp;
}

EXTERN(UINT32, AllocPid(NOPTR));

KTask* TaskCreate(const CHAR *Name, TaskEntry Entry, NOPTR *Arg,
                      UINT8 Priority, INT32 Quantum) {
    KTask *Task;
    NOPTR *StackPage;
    UINT32 StackPages;

    if (!Entry) {
        return NULLPTR;
    }

    Task = (KTask*)MemoryAllocate(sizeof(KTask));
    if (!Task) {
        return NULLPTR;
    }

    MemSet(Task, 0, sizeof(KTask));
    Task->Pid = AllocPid();
    Task->ParentPid = 0;
    Task->ExitCode = 0;
    Task->Reaped = FALSE;
    SyscallFdInitTask(Task);
    StrnCpy(Task->Name, Name ? Name : "?", TASK_NAME_MAX - 1);
    Task->Entry = Entry;
    Task->EntryArg = Arg;
    Task->Priority = Priority;
    Task->BasePriority = Priority;
    Task->QuantumReset = Quantum > 0 ? Quantum : TASK_DEFAULT_QUANTUM;
    Task->Quantum = Task->QuantumReset;
    Task->State = TaskStateReady;
    Task->Uid = UID_NOBODY;
    Task->Gid = GID_NOBODY;
    Task->Euid = UID_NOBODY;
    Task->Egid = GID_NOBODY;
    Task->Role = UserRoleGuest;
    Task->Authenticated = FALSE;
    Task->AvxEnabled = CpuHasAvx();
    if (Task->AvxEnabled) {
    	MemSet(Task->AvxState, 0, sizeof(Task->AvxState));
    }

    // <-- ВЫДЕЛЯЕМ СТЕК + GUARD СТРАНИЦУ
    // (Одну страницу берем как guard, остальное — стек)
    StackPages = (TASK_KERNEL_STACK_SIZE / PAGE_SIZE) + 1;
    StackPage = PhysAllocAllocateRange(PhysAllocGet(), StackPages);
    if (!StackPage) {
        MemoryFree(Task);
        return NULLPTR;
    }

    UINT64 GuardAddr = (UINT64)(UINTPTR)StackPage;
    PagingUnmapPage(PagingGetKernelCR3(), GuardAddr);

    // Рабочая база стека для ядра начинается СО СЛЕДУЮЩЕЙ страницы (сдвигаем на 4КБ вверх)
    Task->KernelStackBase = GuardAddr + PAGE_SIZE; 
    Task->KernelStackSize = TASK_KERNEL_STACK_SIZE;

    TaskInitStack(Task);
    return Task;
}

NOPTR TaskDestroy(KTask *Task) {
    if (!Task) {
        return;
    }
    TaskFreeKernelStack(Task);
    MemoryFree(Task);
}

NOPTR TaskExit(INT32 ExitCode) {
    KTask *Task = SchedulerGetCurrent();
    if (!Task) {
        for (;;) { Halt(); }
    }
    
    Task->ExitCode = ExitCode;
    Task->State = TaskStateTerminated;
    
    // Освобождаем стек
    TaskFreeKernelStack(Task);
    
    SchedulerRequestReschedule();
    SchedulerYield();
    
    for (;;) { Halt(); }
}
