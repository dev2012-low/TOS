#include <Elf.h>
#include <Memory/Allocator.h>
#include <Memory/PhysAlloc.h>
#include <Kernel/Paging.h>
#include <Kernel/Return.h>
#include <Lib/String.h>
#include <Console.h>
#include <Crypto/Rng.h>

static UINT64 ElfRandomizeBase(UINT64 Base) {
    UINT64 RandomPage;
    
    // Нам вполне хватит 8 байт для генерации номера страницы
    RngGetRandomBytes((UINT8*)&RandomPage, sizeof(RandomPage));
    
    // Случайная страница от 1 до 8191 (~32 МБ диапазона)
    RandomPage = (RandomPage % 8191) + 1;
    
    // Строго выровненное смещение, кратное 4КБ!
    UINT64 Offset = RandomPage * PAGE_SIZE;
    
    return Base + Offset;
}


/* Helper: Check if ELF header is valid */
static BOOL ElfIsValid(const Elf64Ehdr *Ehdr) {
    if (!Ehdr) return FALSE;
    
    /* Check magic number */
    if (Ehdr->EIdent[EI_MAG0] != ELFMAG0 ||
        Ehdr->EIdent[EI_MAG1] != ELFMAG1 ||
        Ehdr->EIdent[EI_MAG2] != ELFMAG2 ||
        Ehdr->EIdent[EI_MAG3] != ELFMAG3) {
        return FALSE;
    }
    
    /* Check 64-bit class */
    if (Ehdr->EIdent[EI_CLASS] != ELFCLASS64) {
        return FALSE;
    }
    
    /* Check little-endian */
    if (Ehdr->EIdent[EI_DATA] != ELFDATA2LSB) {
        return FALSE;
    }
    
    /* Check version */
    if (Ehdr->EVersion != EV_CURRENT) {
        return FALSE;
    }
    
    /* Check architecture */
    if (Ehdr->EMachine != EM_X86_64) {
        return FALSE;
    }
    
    /* Must be executable or shared object */
    if (Ehdr->EType != ET_EXEC && Ehdr->EType != ET_DYN) {
        return FALSE;
    }
    
    return TRUE;
}

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

/* Helper: Get page-aligned load range */
static INT ElfGetLoadRange(const Elf64Ehdr *Ehdr, const UINT8 *ElfData,
                              UINT64 *BaseOut, UINT64 *SizeOut) {
    UINT64 Base = ~0ULL;
    UINT64 End = 0;
    
    Elf64Phdr *Phdr = (Elf64Phdr*)(ElfData + Ehdr->EPhoff);
    
    for (UINT16 I = 0; I < Ehdr->EPhnum; I++) {
        if (Phdr[I].PType != PT_LOAD) continue;
        
        UINT64 SegmentStart = AlignDown(Phdr[I].PVaddr, PAGE_SIZE);
        UINT64 SegmentEnd = AlignUp(Phdr[I].PVaddr + Phdr[I].PMemsz, PAGE_SIZE);
         
        if (SegmentStart < Base) Base = SegmentStart;
        if (SegmentEnd > End) End = SegmentEnd;
    }
    
    if (Base == ~0ULL) {
        return NO_OBJECT;
    }
    
    *BaseOut = Base;
    *SizeOut = End - Base;
    return SUCCESS;
}

/* Helper: Map segment into user space */
static INT ElfMapSegment(UINT64 *PML4, UINT64 VirtAddr, UINT64 FileOffset,
                          UINT64 FileSize, UINT64 MemSize, UINT32 Flags,
                          const UINT8 *ElfData) {
    UINT64 PageStart = AlignDown(VirtAddr, PAGE_SIZE);
    UINT64 PageEnd = AlignUp(VirtAddr + MemSize, PAGE_SIZE);
    UINT64 FileOffsetAligned = AlignDown(FileOffset, PAGE_SIZE);
    UINT64 OffsetDiff = VirtAddr - PageStart;
    
    for (UINT64 Addr = PageStart; Addr < PageEnd; Addr += PAGE_SIZE) {
        UINT64 PagePhys;
        UINT64 PageFlags = PTE_PRESENT | PTE_USER;
        
        /* Set permissions */
        if (Flags & PF_W) PageFlags |= PTE_WRITABLE;
        if (!(Flags & PF_X)) PageFlags |= PTE_NO_EXEC;
        
        /* Allocate physical page */
        NOPTR *PhysPage = PhysAllocAllocatePage(PhysAllocGet());
        if (!PhysPage) {
            return(NO_MEMORY);
        }
        PagePhys = (UINT64)(UINTPTR)PhysPage;
        
        /* Clear page via identity-mapped physical address */
        MemSet(PhysPage, 0, PAGE_SIZE);
        
        /* Copy data from ELF if this page overlaps with file data */
        UINT64 PageFileStart = 0;
        UINT64 PageFileEnd = PAGE_SIZE;
        
        if (Addr >= VirtAddr) {
            PageFileStart = (Addr - VirtAddr) + FileOffsetAligned + OffsetDiff;
        } else {
            PageFileStart = FileOffsetAligned;
        }
        
        if (PageFileStart < FileOffset + FileSize) {
            UINT64 CopyStart = (PageFileStart > FileOffset) ? PageFileStart : FileOffset;
            UINT64 CopyEnd = (PageFileStart + PAGE_SIZE < FileOffset + FileSize) ?
                              PageFileStart + PAGE_SIZE : FileOffset + FileSize;
            UINT64 CopySize = CopyEnd - CopyStart;
            UINT64 PageOffset = (CopyStart - FileOffsetAligned) - OffsetDiff;
            
            if (PageOffset < PAGE_SIZE && CopySize > 0) {
                MemCpy((NOPTR*)(PhysPage + PageOffset),
                       (NOPTR*)(ElfData + CopyStart),
                       CopySize);
            }
        }
        
        /* Map page */
        if (PagingMapPage(PML4, Addr, PagePhys, PageFlags) != 0) {
            PhysAllocFreePage(PhysAllocGet(), PhysPage);
            RETURN(IO_ERROR);
        }
    }
    
    RETURN(SUCCESS);
}

/* Helper: Apply relocations */
static INT ElfApplyRelocations(const UINT8 *ElfData, const Elf64Ehdr *Ehdr,
                                UINT64 *PML4, UINT64 LoadBase) {
    Elf64Shdr *Shdr = (Elf64Shdr*)(ElfData + Ehdr->EShoff);
    UINT8 *StrTab = NULLPTR;
    UINT64 StrTabSize = 0;
    
    /* Find string table */
    for (UINT16 I = 0; I < Ehdr->EShnum; I++) {
        if (Shdr[I].ShType == SHT_STRTAB && I != Ehdr->EShstrndx) {
            StrTab = (UINT8*)ElfData + Shdr[I].ShOffset;
            StrTabSize = Shdr[I].ShSize;
            break;
        }
    }
    
    /* Apply relocations */
    for (UINT16 I = 0; I < Ehdr->EShnum; I++) {
        if (Shdr[I].ShType != SHT_RELA) continue;
        
        Elf64Rela *Rela = (Elf64Rela*)(ElfData + Shdr[I].ShOffset);
        UINT64 RelaCount = Shdr[I].ShSize / sizeof(Elf64Rela);
        UINT64 TargetSection = Shdr[I].ShInfo;
        
        if (TargetSection >= Ehdr->EShnum) continue;
        
        UINT64 TargetAddr = Shdr[TargetSection].ShAddr;
        
        for (UINT64 J = 0; J < RelaCount; J++) {
            UINT64 Offset = Rela[J].ROffset;
            UINT64 Type = ELF64_R_TYPE(Rela[J].RInfo);
            UINT64 SymIdx = ELF64_R_SYM(Rela[J].RInfo);
            UINT64 PatchVirt = TargetAddr + Offset;
            UINT64 Value = 0;
            NOPTR *PatchAddr;
            
            PatchAddr = PagingUserVirtToPtr(PML4, PatchVirt);
            if (!PatchAddr) {
                return IO_ERROR;
            }
            
            /* Get symbol value */
            if (SymIdx != 0 && StrTab) {
                /* Find symbol section */
                for (UINT16 K = 0; K < Ehdr->EShnum; K++) {
                    if (Shdr[K].ShType == SHT_SYMTAB) {
                        Elf64Sym *Sym = (Elf64Sym*)(ElfData + Shdr[K].ShOffset);
                        if (SymIdx < (Shdr[K].ShSize / sizeof(Elf64Sym))) {
                            Value = Sym[SymIdx].StValue;
                            break;
                        }
                    }
                }
            }
            
            switch (Type) {
                case R_X86_64_NONE:
                    break;
                    
                case R_X86_64_64:
                    *(UINT64*)PatchAddr = Value + Rela[J].RAddend;
                    break;
                    
                case R_X86_64_32:
                    *(UINT32*)PatchAddr = (UINT32)(Value + Rela[J].RAddend);
                    break;
                    
                case R_X86_64_32S:
                    *(INT32*)PatchAddr = (INT32)(Value + Rela[J].RAddend);
                    break;
                    
                case R_X86_64_RELATIVE:
                    *(UINT64*)PatchAddr = LoadBase + Rela[J].RAddend;
                    break;
                    
                case R_X86_64_GLOB_DAT:
                case R_X86_64_JUMP_SLOT:
                    *(UINT64*)PatchAddr = Value;
                    break;
                    
                default:
                    ConsolePrint("[ELF] Unsupported relocation type: %llu\n", Type);
                    return NOT_SUPPORTED;
            }
        }
    }
    
    return SUCCESS;
}

/* Main ELF load function */
ElfLoadResult ElfLoad(const UINT8 *ElfData, USIZE ElfSize, ElfLoadedImage *Out) {
    Elf64Ehdr *Ehdr;
    UINT64 LoadBase, LoadSize;
    UINT64 MinVaddr = ~0ULL;
    INT Result;
    
    if (!ElfData || !Out) {
        return ELF_LOAD_INVALID_MAGIC;
    }
    
    if (ElfSize < sizeof(Elf64Ehdr)) {
        return ELF_LOAD_INVALID_MAGIC;
    }
    
    Ehdr = (Elf64Ehdr*)ElfData;
    
    if (!ElfIsValid(Ehdr)) {
        return ELF_LOAD_INVALID_MAGIC;
    }
    
    /* Get load range */
    if (IsError(ElfGetLoadRange(Ehdr, ElfData, &LoadBase, &LoadSize)).IsError) {
        return ELF_LOAD_INVALID_PHDR;
    }
    
    /* Рандомизация базы загрузки */
    UINT64 RandomizedBase = ElfRandomizeBase(LoadBase);
    UINT64 Offset = RandomizedBase - LoadBase;
    LoadBase = RandomizedBase;

    /* Create page tables */
    Out->PML4 = PagingCreateUserTask(NULLPTR, 0);
    if (!Out->PML4) {
        return ELF_LOAD_NO_MEMORY;
    }
    
    Out->BaseAddr = LoadBase;
    Out->TotalSize = LoadSize;
    
    /* Точка входа с учетом смещения */
    Out->EntryPoint = Ehdr->EEntry + Offset;
    
    /* Load segments */
    Elf64Phdr *Phdr = (Elf64Phdr*)(ElfData + Ehdr->EPhoff);
    
    for (UINT16 I = 0; I < Ehdr->EPhnum; I++) {
        if (Phdr[I].PType != PT_LOAD) continue;
        
        if (Phdr[I].PVaddr < MinVaddr) {
            MinVaddr = Phdr[I].PVaddr;
        }

        /* <-- СЕГМЕНТЫ ЗАГРУЖАЮТСЯ СО СМЕЩЕНИЕМ! */
        UINT64 VirtAddr = Phdr[I].PVaddr + Offset;
        
        Result = ElfMapSegment(Out->PML4, VirtAddr, Phdr[I].POffset,
                                Phdr[I].PFilesz, Phdr[I].PMemsz,
                                Phdr[I].PFlags, ElfData);
        if (Result != 0) {
            ElfUnload(Out);
            return ELF_LOAD_NO_MEMORY;
        }
    }
    
    /* <-- ПРИМЕНЯЕМ РЕЛОКАЦИИ ДЛЯ PIE (ET_DYN) */
    /* Исправлено: убрано условие MinVaddr != 0 */
    if (Ehdr->EType == ET_DYN) {
        /* <-- НЕ ДОБАВЛЯЕМ LoadBase К EntryPoint! УЖЕ СДЕЛАНО! */
        /* Out->EntryPoint += LoadBase; // <-- УБРАТЬ! */
        
        if (Ehdr->EShoff != 0) {
            Result = ElfApplyRelocations(ElfData, Ehdr, Out->PML4, LoadBase);
            if (Result != 0) {
                ConsolePrint("[ELF] Relocation failed: %d\n", Result);
                /* Not fatal for statically linked */
            }
        }
    }
    
    return ELF_LOAD_SUCCESS;
}