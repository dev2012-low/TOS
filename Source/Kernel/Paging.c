#include <Kernel/Paging.h>
#include <Kernel/Types.h>
#include <Memory/PhysAlloc.h>
#include <Lib/String.h>
#include <Kernel/Return.h>
#include <Kernel/Idt.h>
#include <Kernel/CpuFeatures.h>

#define PML4_SHIFT 39
#define PDPT_SHIFT 30
#define PD_SHIFT   21
#define PT_SHIFT   12

#define PML4_INDEX(Va) (((UINT64)(Va) >> PML4_SHIFT) & 0x1FF)
#define PDPT_INDEX(Va) (((UINT64)(Va) >> PDPT_SHIFT) & 0x1FF)
#define PD_INDEX(Va)   (((UINT64)(Va) >> PD_SHIFT) & 0x1FF)
#define PT_INDEX(Va)   (((UINT64)(Va) >> PT_SHIFT) & 0x1FF)

#define PAGE_MASK      0xFFFFFFFFF000ULL
#define HUGE_PAGE_MASK 0xFFFFFFFFFFE00000ULL

static UINT64 *KernelPML4 = NULLPTR;
static UINT64 TotalPhysPages = 0;
static BOOL PagingInitialized = FALSE;

static UINT64* GetNextLevel(UINT64 *Table, UINT64 Index, UINT64 Flags, BOOL Allocate) {
    UINT64 Entry = Table[Index];

    if (Entry & PTE_HUGE) {
        if (!Allocate) {
            return NULLPTR;
        }

        UINT64 HugePhys = Entry & PAGE_MASK;
        // Сохраняем все флаги, кроме HUGE и адреса
        UINT64 PtFlags = (Entry & ~HUGE_PAGE_MASK & ~PTE_HUGE) | PTE_PRESENT | PTE_WRITABLE;
        // Сохраняем PTE_USER и PTE_NO_EXEC из huge page
        PtFlags |= (Entry & (PTE_USER | PTE_NO_EXEC));

        UINT64 *Pt = (UINT64*)PhysAllocAllocatePage(PhysAllocGet());
        if (!Pt) {
            return NULLPTR;
        }

        for (INT I = 0; I < 512; I++) {
            UINT64 PagePhys = HugePhys + (UINT64)I * PAGE_SIZE;
            Pt[I] = PagePhys | PtFlags;
        }

        UINT64 PtPhys = (UINT64)VirtToPhysPtr((NOPTR*)Pt);
        Table[Index] = PtPhys | PtFlags;

        return (UINT64*)PhysToVirtPtr((NOPTR*)PtPhys);
    }
    
    if (!(Entry & PTE_PRESENT)) {
        if (!Allocate) return NULLPTR;

        UINT64 *NewTable = (UINT64*)PhysAllocAllocatePage(PhysAllocGet());
        if (!NewTable) {
            return NULLPTR;
        }

        MemSet(NewTable, 0, PAGE_SIZE);

        UINT64 PhysAddr = (UINT64)VirtToPhysPtr(NewTable);
        Table[Index] = PhysAddr | Flags | PTE_PRESENT | PTE_WRITABLE;
        
        return NewTable;
    }

    UINT64 PhysAddr = Entry & PAGE_MASK;
    return (UINT64*)PhysToVirtPtr((NOPTR*)PhysAddr);
}

NOPTR PagingInit(UINT64 TotalPhysMem) {
    if (PagingInitialized) return;
    
    TotalPhysPages = TotalPhysMem / PAGE_SIZE;

    UINT64 CR3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(CR3));
    KernelPML4 = (UINT64*)PhysToVirtPtr((NOPTR*)(CR3 & PAGE_MASK));

    extern struct IdtEntry Idt[IDT_ENTRIES];
    extern UINT64 Gdt[];

    PagingUnmapPage(KernelPML4, 0x0);
    PagingUnmapPage(KernelPML4, 0xFFFFFFFFFFFFF000ULL);
    PagingUnmapPage(KernelPML4, (UINT64)Idt);
    PagingUnmapPage(KernelPML4, (UINT64)Gdt);

    CpuEnableSmepSmap();
    CpuEnableUmip();
    if (CpuHasAvx()) CpuEnableXsave();
    
    PagingInitialized = TRUE;
}

UINT64 *PagingGetKernelCR3(NOPTR) {
    return KernelPML4;
}

NOPTR PagingSwitch(UINT64 *PML4) {
    if (!PML4) return;
    
    UINT64 PhysAddr = (UINT64)VirtToPhysPtr(PML4);
    __asm__ volatile("mov %0, %%cr3" : : "r"(PhysAddr) : "memory");
}

UINT64 PagingLookupVirt(UINT64 *PML4, UINT64 VirtAddr) {
    UINT64 *Pdpt;
    UINT64 *Pdir;
    UINT64 *Ptbl;
    UINT64 Entry;

    if (!PML4) {
        return 0;
    }

    Entry = PML4[PML4_INDEX(VirtAddr)];
    if (!(Entry & PTE_PRESENT)) {
        return 0;
    }

    Pdpt = (UINT64*)PhysToVirtPtr((NOPTR*)(Entry & PAGE_MASK));
    Entry = Pdpt[PDPT_INDEX(VirtAddr)];
    if (!(Entry & PTE_PRESENT)) {
        return 0;
    }
    if (Entry & PTE_HUGE) {
        return (Entry & HUGE_PAGE_MASK) + (VirtAddr & (PAGE_SIZE * 512 - 1));
    }

    Pdir = (UINT64*)PhysToVirtPtr((NOPTR*)(Entry & PAGE_MASK));
    Entry = Pdir[PD_INDEX(VirtAddr)];
    if (!(Entry & PTE_PRESENT)) {
        return 0;
    }
    if (Entry & PTE_HUGE) {
        return (Entry & HUGE_PAGE_MASK) + (VirtAddr & (PAGE_SIZE * 512 - 1));
    }

    Ptbl = (UINT64*)PhysToVirtPtr((NOPTR*)(Entry & PAGE_MASK));
    Entry = Ptbl[PT_INDEX(VirtAddr)];
    if (!(Entry & PTE_PRESENT)) {
        return 0;
    }

    return (Entry & PAGE_MASK) | (VirtAddr & (PAGE_SIZE - 1));
}

NOPTR* PagingUserVirtToPtr(UINT64 *PML4, UINT64 VirtAddr) {
    UINT64 Phys = PagingLookupVirt(PML4, VirtAddr);
    if (!Phys) {
        return NULLPTR;
    }
    return (NOPTR*)PhysToVirt(Phys);
}

INT PagingMapPage(UINT64 *PML4, UINT64 VirtAddr, UINT64 PhysAddr, UINT64 Flags) {
    if (!PML4) RETURN(NO_OBJECT);

    Flags |= PTE_PRESENT | PTE_WRITABLE;

    UINT64 *Pdpt = GetNextLevel(PML4, PML4_INDEX(VirtAddr), Flags, TRUE);
    if (!Pdpt) RETURN(NO_OBJECT);
    
    UINT64 *Pdir = GetNextLevel(Pdpt, PDPT_INDEX(VirtAddr), Flags, TRUE);
    if (!Pdir) RETURN(NO_OBJECT);
    
    UINT64 *Ptbl = GetNextLevel(Pdir, PD_INDEX(VirtAddr), Flags, TRUE);
    if (!Ptbl) RETURN(NO_OBJECT);
 
    UINT64 PteIndex = PT_INDEX(VirtAddr);
    Ptbl[PteIndex] = (PhysAddr & PAGE_MASK) | Flags;

    __asm__ volatile("invlpg (%0)" : : "r"(VirtAddr) : "memory");
    
    RETURN(SUCCESS);
}

INT PagingMapRange(UINT64 *PML4, UINT64 VirtStart, UINT64 PhysStart, USIZE Size, UINT64 Flags) {
    if (!PML4) RETURN(NO_OBJECT);
    if (Size == 0) RETURN(INCORRECT_VALUE);
    
    UINT64 Virt = VirtStart & ~(PAGE_SIZE - 1);
    UINT64 Phys = PhysStart & ~(PAGE_SIZE - 1);
    UINT64 End = VirtStart + Size;
    
    while (Virt < End) {
        if (PagingMapPage(PML4, Virt, Phys, Flags) != 0) {
            RETURN(INCORRECT_VALUE);
        }
        Virt += PAGE_SIZE;
        Phys += PAGE_SIZE;
    }
    
    RETURN(SUCCESS);
}

NOPTR PagingUnmapPage(UINT64 *PML4, UINT64 VirtAddr) {
    if (!PML4) return;

    UINT64 *Pdpt = GetNextLevel(PML4, PML4_INDEX(VirtAddr), 0, FALSE);
    if (!Pdpt) return;
    
    UINT64 *Pdir = GetNextLevel(Pdpt, PDPT_INDEX(VirtAddr), 0, FALSE);
    if (!Pdir) return;
    
    UINT64 *Ptbl = GetNextLevel(Pdir, PD_INDEX(VirtAddr), 0, FALSE);
    if (!Ptbl) return;

    UINT64 PteIndex = PT_INDEX(VirtAddr);
    Ptbl[PteIndex] = 0;

    __asm__ volatile("invlpg (%0)" : : "r"(VirtAddr) : "memory");
}

NOPTR PagingUnmapRange(UINT64 *PML4, UINT64 VirtStart, USIZE Size) {
    if (!PML4 || Size == 0) return;
    
    UINT64 Virt = VirtStart & ~(PAGE_SIZE - 1);
    UINT64 End = VirtStart + Size;
    
    while (Virt < End) {
        PagingUnmapPage(PML4, Virt);
        Virt += PAGE_SIZE;
    }
}

UINT64 *PagingCreateUserTask(NOPTR *UserMem, USIZE UserSize) {
    if (!KernelPML4) return NULLPTR;
    
    UINT64 *UserPML4 = (UINT64*)PhysAllocAllocatePage(PhysAllocGet());
    if (!UserPML4) return NULLPTR;
    
    UserPML4 = (UINT64*)PhysToVirtPtr(UserPML4);
    MemSet(UserPML4, 0, PAGE_SIZE);

    // Копируем kernel space (верхние 256 записей) из kernel_pml4
    for (INT I = 256; I < 512; I++) {
        UINT64 Entry = KernelPML4[I];
        if (Entry & PTE_PRESENT) {
            Entry &= ~PTE_USER;
	    UserPML4[I]	= Entry;	
        }
    }

    if (UserMem && UserSize > 0) {
        UINT64 UserStart = (UINT64)UserMem;
        UINT64 UserEnd = UserStart + UserSize;
        
        // Для кода (первые 2-4 страницы) — без NX
        UINT64 CodeStart = UserStart;
        UINT64 CodeEnd = UserStart + 4 * PAGE_SIZE;  // 16KB кода
        
        if (CodeEnd > UserEnd) CodeEnd = UserEnd;
        
        // Мапим код (исполняемый, без NX)
        if (CodeEnd > CodeStart) {
            UINT64 Flags = PTE_USER | PTE_WRITABLE;
            if (PagingMapRange(UserPML4, CodeStart, CodeStart, 
                                 CodeEnd - CodeStart, Flags) != 0) {
                PagingDestroyUserTask(UserPML4);
                return NULLPTR;
            }
        }
        
        // Мапим данные, стек, heap (с NX)
        if (CodeEnd < UserEnd) {
            UINT64 Flags = PTE_USER | PTE_WRITABLE | PTE_NO_EXEC;
            if (PagingMapRange(UserPML4, CodeEnd, CodeEnd, 
                                 UserEnd - CodeEnd, Flags) != 0) {
                PagingDestroyUserTask(UserPML4);
                return NULLPTR;
            }
        }
    }
    
    return UserPML4;
}

NOPTR PagingDestroyUserTask(UINT64 *PML4) {
    if (!PML4) return;

    for (INT I = 0; I < 256; I++) {
        if (PML4[I] & PTE_PRESENT) {
            UINT64 *Pdpt = (UINT64*)PhysToVirtPtr((NOPTR*)(PML4[I] & PAGE_MASK));
            
            for (INT J = 0; J < 512; J++) {
                if (Pdpt[J] & PTE_PRESENT) {
                    UINT64 *Pdir = (UINT64*)PhysToVirtPtr((NOPTR*)(Pdpt[J] & PAGE_MASK));
                    
                    for (INT K = 0; K < 512; K++) {
                        if (Pdir[K] & PTE_PRESENT && !(Pdir[K] & PTE_HUGE)) {
                            UINT64 *Ptbl = (UINT64*)PhysToVirtPtr((NOPTR*)(Pdir[K] & PAGE_MASK));
                            
                            for (INT L = 0; L < 512; L++) {
                                if (Ptbl[L] & PTE_PRESENT) {
                                    UINT64 Phys = Ptbl[L] & PAGE_MASK;
                                    PhysAllocFreePage(PhysAllocGet(), (NOPTR*)Phys);
                                }
                            }
                            PhysAllocFreePage(PhysAllocGet(), (NOPTR*)VirtToPhysPtr(Ptbl));
                        } else if (Pdir[K] & PTE_PRESENT && (Pdir[K] & PTE_HUGE)) {
                            UINT64 Phys = Pdir[K] & HUGE_PAGE_MASK;
                            PhysAllocFreePage(PhysAllocGet(), (NOPTR*)Phys);
                        }
                    }
                    PhysAllocFreePage(PhysAllocGet(), (NOPTR*)VirtToPhysPtr(Pdir));
                }
            }
            PhysAllocFreePage(PhysAllocGet(), (NOPTR*)VirtToPhysPtr(Pdpt));
        }
    }

    PhysAllocFreePage(PhysAllocGet(), (NOPTR*)VirtToPhysPtr(PML4));
}

INT PagingMapUserRegion(UINT64 *PML4, NOPTR *Addr, USIZE Size) {
    UINT64 Virt = (UINT64)Addr;
    UINT64 Flags = PTE_USER | PTE_WRITABLE | PTE_NO_EXEC;
    
    return PagingMapRange(PML4, Virt, Virt, Size, Flags);
}

NOPTR PagingUnmapUserRegion(UINT64 *PML4, NOPTR *Addr, USIZE Size) {
    PagingUnmapRange(PML4, (UINT64)Addr, Size);
}
