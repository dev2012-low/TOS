#pragma once

#include <Multiboot2Struct.h>
#include <Kernel/Types.h>

INT Multiboot2ParserInitInfo(Multiboot2Info *Info);
INT Multiboot2ParserParse(NOPTR *Multiboot2Addr, Multiboot2Info *Info);
NOPTR Multiboot2ParserFreeInfo(Multiboot2Info *Info);
UINT64 Multiboot2ParserGetTotalMemory(Multiboot2Info *Info);
UINT64 Multiboot2GetUsableMemory(Multiboot2Info *Info);
UINT32 Multiboot2ParserGetMemoryRegions(Multiboot2Info *Info, MemoryRegion *Regions, UINT32 MaxRegions);
UINT64 Multiboot2ParserGetUsableMemory(Multiboot2Info *Info);
UINT8 Multiboot2ParserGetBootDisk(Multiboot2Info *Info);
UINT32 Multiboot2ParserGetBootPartition(Multiboot2Info *Info);
NOPTR* Multiboot2ParserFindModule(const CHAR *Name, UINT64 *Size);
CHAR* Multiboot2ParserGetCmdLine(NOPTR);
CHAR* Multiboot2ParserGetCmdLineParameter(const CHAR *Parameter);
Multiboot2Info* Multiboot2ParserGet(NOPTR);
