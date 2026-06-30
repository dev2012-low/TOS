#pragma once

#include <Kernel/Types.h>

#define MULTIBOOT2_MMAP_AVAILABLE 1
#define MULTIBOOT2_MMAP_RESERVED 2
#define MULTIBOOT2_MMAP_ACPI_RECLAIMABLE 3
#define MULTIBOOT2_MMAP_ACPI_NVS 4
#define MULTIBOOT2_MMAP_BAD 5

#define MULTIBOOT2_FRAMEBUFFER_TYPE_INDEXED   0
#define MULTIBOOT2_FRAMEBUFFER_TYPE_RGB       1
#define MULTIBOOT2_FRAMEBUFFER_TYPE_EGA_TEXT  2

#define MAX_MODULES 16

typedef struct {
    CHAR* CmdLine;
    CHAR* BootLoaderName;
    struct {
	UINT32 MemLower;
        UINT32 MemUpper;
    } BasicMeminfo;
    struct {
	UINT32 BiosDevice;
	UINT32 Partition;
	UINT32 SubPartition;
    } BootDevice;
    struct {
	UINT32 EntryCount;
	UINT32 EntrySize;
	struct {
	    UINT64 Addr;
	    UINT64 Len;
	    UINT32 Type;
	} *Entries;
    } Mmap;
    struct {
	UINT64 Addr;
	UINT32 Pitch;
	UINT32 Width;
	UINT32 Height;
	UINT8 Bpp;
	UINT8 Type;
	union {
	    struct {
		UINT8 RedFieldPos;
		UINT8 RedMaskSize;
		UINT8 GreenFieldPos;
		UINT8 GreenMaskSize;
		UINT8 BlueFieldPos;
		UINT8 BlueMaskSize;
	    } Rgb;
	} ColorInfo;
    } Framebuffer;
    struct {
	UINT32 Count;
	struct {
	    UINT32 Start;
	    UINT32 End;
	    CHAR* CmdLine;
	} Modules[MAX_MODULES];
    } Modules;
    struct {
	UINT32 RsdpV1Addr;
	UINT32 RsdpV2Addr;
    } Acpi;
    struct {
	UINT32 SystemTable32;
	UINT64 SystemTable64;
	NOPTR* Mmap;
	UINT32 MmapSize;
	UINT32 MmapDescSize;
	UINT32 MmapDescVersion;	
	NOPTR* ImageHandle32;
	NOPTR* ImageHandle64;
    } Efi;
    UINT64 LoadBaseAddr;
} Multiboot2Info;

typedef struct {
    UINT64 BaseAddr;
    UINT64 Length;
    UINT32 Type;
} MemoryRegion;