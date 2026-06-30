#include <Kernel/Types.h>
#include <Kernel/Task.h>
#include <Kernel/Scheduler.h>
#include <Console.h>
#include <Asm/Cpu.h>
#include <Memory/PhysAlloc.h>
#include <Kernel/Paging.h>
#include <Kernel/SysStop.h>
#include <Lib/String.h>

#define PF_PRESENT   (1 << 0)
#define PF_WRITE     (1 << 1)
#define PF_USER      (1 << 2)

#define LAZY_ALLOC_START    0x100000000000ULL  /* Область для ленивой аллокации */
#define LAZY_ALLOC_END      0x200000000000ULL

static const CHAR* PageFaultType(UINT64 ErrorCode) {
    if (!(ErrorCode & PF_PRESENT)) return "page not present";
    if (ErrorCode & PF_WRITE) return "write protection";
    return "read protection";
}

NOPTR PageFaultHandler(UINT64 FaultAddr, UINT64 ErrorCode, NOPTR *StackFrame) {
    KTask *Current = SchedulerGetCurrent();
    
    /* СЦЕНАРИЙ 1: Ленивая аллокация (выделяем страницу при первом обращении) */
    if (!(ErrorCode & PF_PRESENT) && 
        FaultAddr >= LAZY_ALLOC_START && FaultAddr < LAZY_ALLOC_END) {
        
        NOPTR *PhysPage = PhysAllocAllocatePage(PhysAllocGet());
        if (!PhysPage) {
            goto KillTask;
        }
        
        UINT64 PageVirt = FaultAddr & ~(PAGE_SIZE - 1);
        UINT64 PagePhys = (UINT64)(UINTPTR)PhysPage;
        UINT64 Flags = PTE_PRESENT | PTE_WRITABLE;
        
        if (ErrorCode & PF_USER) Flags |= PTE_USER;
        
        if (PagingMapPage(PagingGetKernelCR3(), PageVirt, PagePhys, Flags) == 0) {
            MemSet((NOPTR*)(UINTPTR)PageVirt, 0, PAGE_SIZE);
            return;  /* Всё ок, страница выделена */
        }
        
        PhysAllocFreePage(PhysAllocGet(), PhysPage);
    }
    
    /* СЦЕНАРИЙ 2: Ошибка доступа в юзермоде — убиваем задачу */
    if ((ErrorCode & PF_USER) && Current) {
        goto KillTask;
    }
    
    SysStop("PAGE_FAULT");

KillTask:
    if (Current && (ErrorCode & PF_USER)) {
        SchedulerKillTask(Current->Pid);
        SchedulerYield();
    }
    
    for (;;) Halt();
}