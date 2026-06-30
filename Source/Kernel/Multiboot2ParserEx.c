#include <Multiboot2Parser.h>
#include <Multiboot2Struct.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Kernel/Return.h>

EXTERN(Multiboot2Info, MB);

struct Multiboot2Tag {
    UINT32 Type;
    UINT32 Size;
} ATTRIBUTE(packed);

struct Multiboot2TagModule {
    UINT32 Type;
    UINT32 Size;
    UINT32 ModStart;
    UINT32 ModEnd;
    CHAR CmdLine[];
} ATTRIBUTE(packed);

static inline UINT32 AlignUp(UINT32 Value, UINT32 Alignment) {
    return (Value + Alignment - 1) & ~(Alignment -1);
}

INT Multiboot2ParserInitInfo(Multiboot2Info *Info) {
    if (!Info) RETURN(NO_OBJECT);
    UINT8* Ptr = (UINT8*)Info;
    for (UINT32 I = 0; I < sizeof(Multiboot2Info); I++) {
	Ptr[I] = 0;
    }
    Info->CmdLine = 0;
    Info->BootLoaderName = 0;
    Info->Mmap.Entries = 0;
    Info->Efi.Mmap = 0;
    RETURN(SUCCESS);
}

NOPTR Multiboot2ParserFreeInfo(Multiboot2Info *Info) {
    if (!Info) return;
    Multiboot2ParserInitInfo(Info);
}

INT Multiboot2ParserCollectModules(NOPTR *Multiboot2Addr, Multiboot2Info *Info) {
    if (!Multiboot2Addr || !Info) RETURN(NO_OBJECT);
    Info->Modules.Count = 0;
    UINT32 TotalSize = *(UINT32*)Multiboot2Addr;
    struct Multiboot2Tag* Tag = (struct Multiboot2Tag*)((UINT8*)Multiboot2Addr + 8);
    while (Tag->Type != 0 && (UINT8*)Tag < (UINT8*)Multiboot2Addr + TotalSize) {
	if (Tag->Type == 3) {
	    Info->Modules.Count++;
    	}
	UINT32 NextTagOffset = AlignUp(Tag->Size, 8);
	Tag = (struct Multiboot2Tag*)((UINT8*)Tag + NextTagOffset);
    }
    return Info->Modules.Count;
}

UINT64 Multiboot2ParserGetTotalMemory(Multiboot2Info *Info) {
    if (!Info) return 0;
    UINT64 Total = 0;
    if (Info->BasicMeminfo.MemUpper > 0) {
	Total = 1024 * 1024 + ((UINT64)Info->BasicMeminfo.MemUpper * 1024);
    }
    if (Info->Mmap.Entries && Info->Mmap.EntryCount > 0) {
	UINT64 MmapTotal = 0;
	for (UINT32 I = 0; I < Info->Mmap.EntryCount; I++) {
	    if (Info->Mmap.Entries[I].Type == MULTIBOOT2_MMAP_AVAILABLE) {
		MmapTotal += Info->Mmap.Entries[I].Len;
	    }
	}
	if (MmapTotal > Total) {
	    Total = MmapTotal;
	}
    }
    return Total;
}

UINT32 Multiboot2ParserGetMemoryRegions(Multiboot2Info *Info, MemoryRegion *Regions, UINT32 MaxRegions) {
    if (!Info || !Regions || MaxRegions == 0) return 0;
    
    UINT32 Count = 0;

    if (Info->Mmap.Entries && Info->Mmap.EntryCount > 0) {
        for (UINT32 I = 0; I < Info->Mmap.EntryCount && Count < MaxRegions; I++) {
            Regions[Count].BaseAddr = Info->Mmap.Entries[I].Addr;
            Regions[Count].Length = Info->Mmap.Entries[I].Len;
            Regions[Count].Type = Info->Mmap.Entries[I].Type;
            Count++;
        }
    } else {
        if (Info->BasicMeminfo.MemLower > 0 && Count < MaxRegions) {
            Regions[Count].BaseAddr = 0;
            Regions[Count].Length = (UINT64)Info->BasicMeminfo.MemLower * 1024;
            Regions[Count].Type = MULTIBOOT2_MMAP_AVAILABLE;
            Count++;
        }
        
        if (Info->BasicMeminfo.MemUpper > 0 && Count < MaxRegions) {
            Regions[Count].BaseAddr = 1024 * 1024; // 1MB
            Regions[Count].Length = (UINT64)Info->BasicMeminfo.MemUpper * 1024;
            Regions[Count].Type = MULTIBOOT2_MMAP_AVAILABLE;
            Count++;
        }
    }
    
    return Count;
}

UINT64 Multiboot2GetUsableMemory(Multiboot2Info *Info) {
    if (!Info) return 0;
    
    UINT64 Total = 0;
    
    if (Info->Mmap.Entries && Info->Mmap.EntryCount > 0) {
        for (UINT32 I = 0; I < Info->Mmap.EntryCount; I++) {
            if (Info->Mmap.Entries[I].Type == MULTIBOOT2_MMAP_AVAILABLE) {
                Total += Info->Mmap.Entries[I].Len;
            }
        }
    } else {
        if (Info->BasicMeminfo.MemLower > 0) {
            Total += (UINT64)Info->BasicMeminfo.MemLower * 1024;
        }
        if (Info->BasicMeminfo.MemUpper > 0) {
            Total += (UINT64)Info->BasicMeminfo.MemUpper * 1024;
        }
        Total += 1024 * 1024;
    }
    
    return Total;
}

UINT8 Multiboot2ParserGetBootDisk(Multiboot2Info *Info) {
    if (!Info) return 0;
    return (UINT8)(Info->BootDevice.BiosDevice & 0xFF);
}

UINT32 Multiboot2ParserGetBootPartition(Multiboot2Info *Info) {
    if (!Info) return 0xFFFFFFFF;
    return Info->BootDevice.Partition;
}

CHAR* Multiboot2ParserGetCmdLine(NOPTR) {
    EXTERN(Multiboot2Info, MB);
    if (MB.CmdLine && MB.CmdLine[0] != '\0') {
	return MB.CmdLine;
    }
    return NULLPTR;
}

CHAR* Multiboot2ParserGetCmdLineParameter(const CHAR *Parameter) {
    CHAR *CmdLine = Multiboot2ParserGetCmdLine();
    if (!CmdLine) return NULLPTR;
    CHAR SearchStr[64];
    SnPrintf(SearchStr, sizeof(SearchStr), "%s=", Parameter);
    CHAR *Start = StrStr(CmdLine, SearchStr);
    if (!Start) return NULLPTR;
    Start += StrLen(SearchStr);
    static CHAR Value[256];
    INT I = 0;
    while (Start[I] && Start[I] != ' ' && Start[I] != '\t' && Start[I] != '\n' && I < 255) {
	Value[I] = Start[I];
	I++;
    }
    Value[I] = '\0';
    return Value;
}

NOPTR* Multiboot2ParserFindModule(const CHAR *Name, UINT64 *Size) {
    EXTERN(Multiboot2Info, MB);
    for (UINT32 I = 0; I < MB.Modules.Count; I++) {
	if (MB.Modules.Modules[I].CmdLine && StrCmp(MB.Modules.Modules[I].CmdLine, Name) == 0) {
	    if (Size) *Size = MB.Modules.Modules[I].End - MB.Modules.Modules[I].Start;
	    return (NOPTR*)(UINTPTR)MB.Modules.Modules[I].Start;
	}
    }
    return NULLPTR;
}

Multiboot2Info* Multiboot2ParserGet(NOPTR) {
    return &MB;
}