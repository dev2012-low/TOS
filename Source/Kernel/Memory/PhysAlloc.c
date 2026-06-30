#include <Multiboot2Parser.h>
#include <Memory/PhysAlloc.h>
#include <Multiboot2Struct.h>
#include <Lib/String.h>
#include <Kernel/Types.h>

EXTERN(Multiboot2Info, MB);

static UINT8 PhysAllocBitmap[64 * 1024 * 1024]; //64MB bitmap = 512GB memory
static PhysAlloc GPhysAlloc;
PhysAlloc GPhysAllocator;

EXTERN(CHAR, KernelPhysStart);
EXTERN(CHAR, KernelPhysEnd);

NOPTR PhysAllocInit(PhysAlloc *PhysAllocator, UINT64 Multiboot2Addr) {
    //1. Parsim MB2
    MemoryRegion Regions[64];
    INT RegionCount = Multiboot2ParserGetMemoryRegions(&MB, Regions, 64);
    
    //2. Find the LOWEST and HIGHEST address of available memory
    UINT64 LowestAddr = 0xFFFFFFFFFFFFFFFF;
    UINT64 HighestAddr = 0;
    
    for (INT I = 0; I < RegionCount; I++) {   
        if (Regions[I].Type == MULTIBOOT2_MMAP_AVAILABLE) {
            if (Regions[I].BaseAddr < LowestAddr)
                LowestAddr = Regions[I].BaseAddr;
            
            UINT64 RegionEnd = Regions[I].BaseAddr + Regions[I].Length;
            if (RegionEnd > HighestAddr)
                HighestAddr = RegionEnd;
        }
    }
    
    //3. Align to the page border
    if (LowestAddr & (PAGE_SIZE - 1))
        LowestAddr = (LowestAddr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    if (HighestAddr & (PAGE_SIZE - 1))
        HighestAddr = HighestAddr & ~(PAGE_SIZE - 1);
    
    //4. Initialize the structure
    PhysAllocator->Bitmap = PhysAllocBitmap;
    PhysAllocator->BaseAddr = LowestAddr;
    PhysAllocator->TotalPages = (HighestAddr - LowestAddr) / PAGE_SIZE;
    PhysAllocator->UsedPages = 0;
    
    //5. Reset the bitmap
    UINT32 BitmapBytes = (PhysAllocator->TotalPages + 7) / 8;
    MemSet(PhysAllocator->Bitmap, 0, BitmapBytes);
    
    //6. WE RESERVE EVERYTHING THAT CANNOT BE TOUCHED
    //First we mark ALL pages as busy
    MemSet(PhysAllocator->Bitmap, 0xFF, BitmapBytes);
    PhysAllocator->UsedPages = PhysAllocator->TotalPages;
    
    //7. Free up ONLY available memory from MB2
    for (INT I = 0; I < RegionCount; I++) {
        if (Regions[I].Type == MULTIBOOT2_MMAP_AVAILABLE) {
            UINT64 Start = Regions[I].BaseAddr;
            UINT64 End = Regions[I].BaseAddr + Regions[I].Length;
            
            //Align
            Start = (Start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            End = End & ~(PAGE_SIZE - 1);
            
            for (UINT64 Addr = Start; Addr < End; Addr += PAGE_SIZE) {
                if (Addr >= LowestAddr && Addr < HighestAddr) {
                    UINT32 PageIdx = (Addr - PhysAllocator->BaseAddr) / PAGE_SIZE;
                    if (PageIdx < PhysAllocator->TotalPages) {
                        UINT32 ByteIdx = PageIdx / 8;
                        UINT32 Bit = PageIdx % 8;
                        PhysAllocator->Bitmap[ByteIdx] &= ~(1 << Bit);
                        PhysAllocator->UsedPages--;
                    }
                }
            }
        }
    }
    
    //8. We reserve MANDATORY areas:
    //- First 1MB (BIOS, boot.asm)
    //- The kernel itself (_kernel_phys_start - _kernel_phys_end)
    //- Multiboot2 structures
    //- PMM bitmap
    
    //First 1MB
    for (UINT64 Addr = 0; Addr < 0x100000; Addr += PAGE_SIZE) {
    	// Вычисляем PageIdx относительно BaseAddr
    	if (Addr >= PhysAllocator->BaseAddr && Addr < PhysAllocator->BaseAddr + PhysAllocator->TotalPages * PAGE_SIZE) {
            UINT32 PageIdx = (Addr - PhysAllocator->BaseAddr) / PAGE_SIZE;
            if (PageIdx < PhysAllocator->TotalPages) {
            	UINT32 ByteIdx = PageIdx / 8;
            	UINT32 Bit = PageIdx % 8;
            	if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                    PhysAllocator->Bitmap[ByteIdx] |= (1 << Bit);
                    PhysAllocator->UsedPages++;
            	}
            }
    	}
    	// Если Addr вне диапазона PMM — просто игнорируем (эта область не управляется PMM)
    }
    
    //Core
    for (UINT64 Addr = (UINT64)&KernelPhysStart; 
         Addr < (UINT64)&KernelPhysEnd; 
         Addr += PAGE_SIZE) {
         if (Addr >= LowestAddr && Addr < HighestAddr) {
            UINT32 PageIdx = (Addr - PhysAllocator->BaseAddr) / PAGE_SIZE;
            if (PageIdx < PhysAllocator->TotalPages) {
                UINT32 ByteIdx = PageIdx / 8;
                UINT32 Bit = PageIdx % 8;
                if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                    PhysAllocator->Bitmap[ByteIdx] |= (1 << Bit);
                    PhysAllocator->UsedPages++;
                }
            }
        }
    }
    
    //Multiboot2 structures
    UINT64 MB2Start = Multiboot2Addr & ~(PAGE_SIZE - 1);
    UINT32 MB2Size = *(UINT32*)Multiboot2Addr; //total_size at the beginning
    UINT64 MB2End = (Multiboot2Addr + MB2Size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    for (UINT64 Addr = MB2Start; Addr < MB2End; Addr += PAGE_SIZE) {
        if (Addr >= LowestAddr && Addr < HighestAddr) {
            UINT32 PageIdx = (Addr - PhysAllocator->BaseAddr) / PAGE_SIZE;
            if (PageIdx < PhysAllocator->TotalPages) {
                UINT32 ByteIdx = PageIdx / 8;
                UINT32 Bit = PageIdx % 8;
                if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                    PhysAllocator->Bitmap[ByteIdx] |= (1 << Bit);
                    PhysAllocator->UsedPages++;
                }
            }
        }
    }
    
    //PMM bitmap (self)
    UINT64 BitmapStart = (UINT64)PhysAllocator->Bitmap;
    UINT64 BitmapEnd = BitmapStart + BitmapBytes;
    
    for (UINT64 Addr = BitmapStart; Addr < BitmapEnd; Addr += PAGE_SIZE) {
        if (Addr >= LowestAddr && Addr < HighestAddr) {
            UINT32 PageIdx = (Addr - PhysAllocator->BaseAddr) / PAGE_SIZE;
            if (PageIdx < PhysAllocator->TotalPages) {
                UINT32 ByteIdx = PageIdx / 8;
                UINT32 Bit = PageIdx % 8;
                if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                    PhysAllocator->Bitmap[ByteIdx] |= (1 << Bit);
                    PhysAllocator->UsedPages++;
                }
            }
        }
    }
}

NOPTR* PhysAllocAllocatePage(PhysAlloc *PhysAllocator) {
    if (!PhysAllocator || !PhysAllocator->Bitmap) return NULLPTR;
    
    UINT32 BitmapBytes = (PhysAllocator->TotalPages + 7) / 8;
    
    for (UINT32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++) {
        if (PhysAllocator->Bitmap[ByteIdx] == 0x00) {
            //The entire byte is free - we are looking for the first bit
            PhysAllocator->Bitmap[ByteIdx] = 0x01;
            UINT32 PageIdx = ByteIdx * 8;
            
            if (PageIdx < PhysAllocator->TotalPages) {
                PhysAllocator->UsedPages++;
                UINT64 Addr = PhysAllocator->BaseAddr + (PageIdx * PAGE_SIZE);
                return (NOPTR*)Addr;
            }
        }
        
        if (PhysAllocator->Bitmap[ByteIdx] != 0xFF) {
            for (INT Bit = 0; Bit < 8; Bit++) {
                if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                    PhysAllocator->Bitmap[ByteIdx] |= (1 << Bit);
                    UINT32 PageIdx = ByteIdx * 8 + Bit;
                    
                    if (PageIdx < PhysAllocator->TotalPages) {
                        PhysAllocator->UsedPages++;
                        UINT64 Addr = PhysAllocator->BaseAddr + (PageIdx * PAGE_SIZE);
                        return (NOPTR*)Addr;
                    }
                }
            }
        }
    }
    
    // tio_printerr("[PMM] OF MEMORY!\n");
    return NULLPTR;
}

NOPTR PhysAllocFreePage(PhysAlloc *PhysAllocator, NOPTR* Addr) {
    if (!PhysAllocator || !PhysAllocator->Bitmap || !Addr) return;
    
    UINT64 PageAddr = (UINT64)Addr & ~(PAGE_SIZE - 1);
    
    if (PageAddr < PhysAllocator->BaseAddr) return;
    
    UINT32 PageIdx = (PageAddr - PhysAllocator->BaseAddr) / PAGE_SIZE;
    if (PageIdx >= PhysAllocator->TotalPages) return;
    
    UINT32 ByteIdx = PageIdx / 8;
    UINT32 Bit = PageIdx % 8;
    
    if (PhysAllocator->Bitmap[ByteIdx] & (1 << Bit)) {
        PhysAllocator->Bitmap[ByteIdx] &= ~(1 << Bit);
        PhysAllocator->UsedPages--;
    }
}

static UINT32 PhysAllocFindContinuousPages(PhysAlloc *PhysAllocator, UINT32 Count) {
    if (!PhysAllocator || Count == 0 || Count > PhysAllocator->TotalPages) return (UINT32)-1;
    
    UINT32 BitmapBytes = (PhysAllocator->TotalPages + 7) / 8;
    UINT32 Continuous = 0;
    UINT32 StartPage = 0;
    
    for (UINT32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++) {
        if (PhysAllocator->Bitmap[ByteIdx] == 0xFF) {
            //All bits in the byte are occupied, reset the counter
            Continuous = 0;
            continue;
        }
        
        //Checking every bit in a byte
        for (INT Bit = 0; Bit < 8; Bit++) {
            UINT32 PageIdx = ByteIdx * 8 + Bit;
            if (PageIdx >= PhysAllocator->TotalPages) break;
            
            if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                //Page is free
                if (Continuous == 0) {
                    StartPage = PageIdx;
                }
                Continuous++;
                
                if (Continuous == Count) {
                    return StartPage;
                }
            } else {
                //The page is busy, reset the counter
                Continuous = 0;
            }
        }
    }
    
    return (UINT32)-1;
}

static NOPTR PhysAllocMarkRange(PhysAlloc *PhysAllocator, UINT32 StartPage, UINT32 Count, BOOL Used) {
    for (UINT32 I = 0; I < Count; I++) {
        UINT32 PageIdx = StartPage + I;
        if (PageIdx >= PhysAllocator->TotalPages) break;
        
        UINT32 ByteIdx = PageIdx / 8;
        UINT32 Bit = PageIdx % 8;
        
        if (Used) {
            if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                PhysAllocator->Bitmap[ByteIdx] |= (1 << Bit);
                PhysAllocator->UsedPages++;
            }
        } else {
            if (PhysAllocator->Bitmap[ByteIdx] & (1 << Bit)) {
                PhysAllocator->Bitmap[ByteIdx] &= ~(1 << Bit);
                PhysAllocator->UsedPages--;
            }
        }
    }
}

NOPTR* PhysAllocAllocateRange(PhysAlloc *PhysAllocator, UINT32 PageCount) {
    if (!PhysAllocator || PageCount == 0) return NULLPTR;
    
    UINT32 StartPage = PhysAllocFindContinuousPages(PhysAllocator, PageCount);
    if (StartPage == (UINT32)-1) {
        // error("[PMM] Failed to allocate %u continuous pages\n", page_count);
        return NULLPTR;
    }
    
    PhysAllocMarkRange(PhysAllocator, StartPage, PageCount, TRUE);
    
    UINT64 Addr = PhysAllocator->BaseAddr + (StartPage * PAGE_SIZE);
    
    //Reset allocated memory to zero (important for security)
    NOPTR *VirtAddr = (NOPTR*)(UINTPTR)Addr;
    MemSet(VirtAddr, 0, PageCount * PAGE_SIZE);
    return VirtAddr;
}

NOPTR PhysAllocFreeRange(PhysAlloc *PhysAllocator, NOPTR *Addr, UINT32 PageCount) {
    if (!PhysAllocator || !Addr || PageCount == 0) return;
    
    UINT64 PageAddr = (UINT64)Addr & ~(PAGE_SIZE - 1);
    if (PageAddr < PhysAllocator->BaseAddr) return;
    
    UINT32 StartPage = (PageAddr - PhysAllocator->BaseAddr) / PAGE_SIZE;
    if (StartPage >= PhysAllocator->TotalPages) return;
    
    //Checking that all pages have actually been selected
    for (UINT32 I = 0; I < PageCount; I++) {
        UINT32 PageIdx = StartPage + I;
        if (PageIdx >= PhysAllocator->TotalPages) break;
        
        UINT32 ByteIdx = PageIdx / 8;
        UINT32 Bit = PageIdx % 8;
        
        if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
            // warn("[PMM] Warning: freeing already free page %u\n", page_idx);
        }
    }
    
    PhysAllocMarkRange(PhysAllocator, StartPage, PageCount, FALSE);
}

NOPTR* PhysAllocAllocateAlignedRange(PhysAlloc *PhysAllocator, UINT32 PageCount, UINT32 AlignmentPages) {
    if (!PhysAllocator || PageCount == 0 || AlignmentPages == 0) return NULLPTR;
    
    //alignment_pages must be a power of two
    if ((AlignmentPages & (AlignmentPages - 1)) != 0) return NULLPTR;
    
    UINT32 BitmapBytes = (PhysAllocator->TotalPages + 7) / 8;
    UINT32 Continuous = 0;
    UINT32 StartPage = 0;
    BOOL InAligned = FALSE;
    
    for (UINT32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++) {
        if (PhysAllocator->Bitmap[ByteIdx] == 0xFF) {
            Continuous = 0;
            InAligned = FALSE;
            continue;
        }
        
        for (INT Bit = 0; Bit < 8; Bit++) {
            UINT32 PageIdx = ByteIdx * 8 + Bit;
            if (PageIdx >= PhysAllocator->TotalPages) break;
            
            //Checking alignment for the start of a block
            if (!InAligned && (PageIdx % AlignmentPages) != 0) {
                continue;
            }
            
            if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                if (Continuous == 0) {
                    StartPage = PageIdx;
                    InAligned = TRUE;
                }
                Continuous++;
                
                if (Continuous == PageCount) {
                    PhysAllocMarkRange(PhysAllocator, StartPage, PageCount, TRUE);
                    UINT64 Addr = PhysAllocator->BaseAddr + (StartPage * PAGE_SIZE);
                    NOPTR *VirtAddr = (NOPTR*)(UINTPTR)Addr;
                    MemSet(VirtAddr, 0, PageCount * PAGE_SIZE);
                    return VirtAddr;
                }
            } else {
                Continuous = 0;
                InAligned = FALSE;
            }
        }
    }
    
    // error("[PMM] Failed to allocate aligned %u pages (align=%u)\n", 
    //                       page_count, alignment_pages);
    return NULLPTR;
}

PhysAlloc* PhysAllocGet(NOPTR) {
    return &GPhysAllocator;
}