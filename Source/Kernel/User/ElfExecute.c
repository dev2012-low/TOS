#include <Elf.h>
#include <Kernel/Paging.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Task.h>
#include <Kernel/Return.h>
#include <Memory/Allocator.h>
#include <Memory/PhysAlloc.h>
#include <Asm/Cpu.h>
#include <Lib/String.h>
#include <Console.h>

/* User stack size (8 MB) */
#define USER_STACK_SIZE (8 * 1024 * 1024)

/* User stack base (top of user space) */
#define USER_STACK_BASE 0x7FFFFFFFF000ULL

/* Helper: Align address up */
static UINT64 AlignUp(UINT64 Addr, UINT64 Align) {
    if (Align == 0) return Addr;
    return (Addr + Align - 1) & ~(Align - 1);
}

/* Helper: Align address down */
static UINT64 AlignDown(UINT64 Addr, UINT64 Align) {
    if (Align == 0) return Addr;
    return Addr & ~(Align - 1);
}

static INT ElfWriteUser(UINT64 *PML4, UINT64 VirtAddr, const NOPTR *Src, USIZE Len) {
    USIZE Done = 0;

    while (Done < Len) {
        NOPTR *Dst = PagingUserVirtToPtr(PML4, VirtAddr + Done);
        UINT64 PageRemain;
        USIZE Chunk;

        if (!Dst) {
            return IO_ERROR;
        }

        PageRemain = PAGE_SIZE - ((VirtAddr + Done) & (PAGE_SIZE - 1));
        Chunk = Len - Done;
        if (Chunk > PageRemain) {
            Chunk = (USIZE)PageRemain;
        }

        MemCpy(Dst, Src + Done, Chunk);
        Done += Chunk;
    }

    return SUCCESS;
}

static INT ElfWriteUserU64(UINT64 *PML4, UINT64 VirtAddr, UINT64 Value) {
    NOPTR *Dst = PagingUserVirtToPtr(PML4, VirtAddr);
    if (!Dst) {
        return IO_ERROR;
    }
    *(UINT64*)Dst = Value;
    return SUCCESS;
}

/* Helper: Set up user stack with arguments */
static INT ElfSetupUserStack(UINT64 *PML4, INT Argc, CHAR **Argv,
                              UINT64 *OutStackTop, UINT64 *OutArgvPtr) {
    UINT64 StackTop = USER_STACK_BASE;
    UINT64 StackBottom = StackTop - USER_STACK_SIZE;
    UINT64 CurrentPos = StackTop;
    UINT64 ArgvArray[256];
    UINT64 ArgvPtr;
    INT I;
    INT Result;

    /* Map stack pages */
    for (UINT64 Addr = StackBottom; Addr < StackTop; Addr += PAGE_SIZE) {
        NOPTR *PhysPage = PhysAllocAllocatePage(PhysAllocGet());
        UINT64 PagePhys;
        UINT64 PageFlags;

        if (!PhysPage) {
            return NO_MEMORY;
        }

        PagePhys = (UINT64)(UINTPTR)PhysPage;
        PageFlags = PTE_PRESENT | PTE_USER | PTE_WRITABLE | PTE_NO_EXEC;

        MemSet(PhysPage, 0, PAGE_SIZE);

        if (PagingMapPage(PML4, Addr, PagePhys, PageFlags) != 0) {
            PhysAllocFreePage(PhysAllocGet(), PhysPage);
            return IO_ERROR;
        }
    }

    CurrentPos = AlignDown(StackTop - 8, 8);

    CurrentPos -= 8;
    Result = ElfWriteUserU64(PML4, CurrentPos, 0);
    if (Result != 0) {
        return Result;
    }

    for (I = Argc - 1; I >= 0; I--) {
        USIZE Len = StrLen(Argv[I]) + 1;
        CurrentPos -= Len;
        Result = ElfWriteUser(PML4, CurrentPos, (NOPTR*)Argv[I], Len);
        if (Result != 0) {
            return Result;
        }
        ArgvArray[I] = CurrentPos;
    }

    CurrentPos = AlignDown(CurrentPos - (UINT64)(Argc + 1) * 8, 8);
    ArgvPtr = CurrentPos;

    for (I = 0; I < Argc; I++) {
        Result = ElfWriteUserU64(PML4, CurrentPos, ArgvArray[I]);
        if (Result != 0) {
            return Result;
        }
        CurrentPos += 8;
    }

    Result = ElfWriteUserU64(PML4, CurrentPos, 0);
    if (Result != 0) {
        return Result;
    }

    CurrentPos -= 8;
    Result = ElfWriteUserU64(PML4, CurrentPos, (UINT64)Argc);
    if (Result != 0) {
        return Result;
    }

    CurrentPos -= 8;
    Result = ElfWriteUserU64(PML4, CurrentPos, 0);
    if (Result != 0) {
        return Result;
    }

    *OutStackTop = CurrentPos;
    *OutArgvPtr = ArgvPtr;

    return SUCCESS;
}

/* Execute a loaded ELF image */
ElfLoadResult ElfExecute(ElfLoadedImage *Image, INT Argc, CHAR **Argv) {
    UINT64 UserStackTop, UserArgvPtr;
    INT Result;
    CHAR TaskName[TASK_NAME_MAX];

    if (!Image || !Image->PML4 || !Image->EntryPoint) {
        return ELF_LOAD_INVALID_MAGIC;
    }

    Result = ElfSetupUserStack(Image->PML4, Argc, Argv, &UserStackTop, &UserArgvPtr);
    if (Result != 0) {
        return ELF_LOAD_NO_MEMORY;
    }

    if (Image->ProgramName) {
        StrnCpy(TaskName, (const CHAR*)Image->ProgramName, TASK_NAME_MAX - 1);
    } else {
        StrnCpy(TaskName, "user_prog", TASK_NAME_MAX - 1);
    }

    Result = SchedulerCreateUserTask(TaskName, Image->PML4,
                                     Image->EntryPoint, UserStackTop, UserArgvPtr,
                                     SCHED_PRIORITY_NORMAL, TASK_DEFAULT_QUANTUM);
    if (Result != 0) {
        return ELF_LOAD_NO_MEMORY;
    }

    ConsolePrint("[ELF] Scheduled '%s' at 0x%llX (stack=0x%llX)\n",
                 TaskName, Image->EntryPoint, UserStackTop);

    Image->PML4 = NULLPTR;
    return ELF_LOAD_SUCCESS;
}

/* Unload ELF image and free resources */
NOPTR ElfUnload(ElfLoadedImage *Image) {
    if (!Image) return;

    if (Image->PML4) {
        PagingDestroyUserTask(Image->PML4);
        Image->PML4 = NULLPTR;
    }

    if (Image->ProgramName) {
        MemoryFree(Image->ProgramName);
        Image->ProgramName = NULLPTR;
    }

    MemSet(Image, 0, sizeof(ElfLoadedImage));
}
