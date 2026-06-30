#include <Multiboot2Parser.h>
#include <Multiboot2Struct.h>
#include <AcpiTables.h>
#include <Kernel/Return.h>

struct Multiboot2Tag {
    UINT32 Type;
    UINT32 Size;
} ATTRIBUTE(packed);

struct Multiboot2TagString {
    UINT32 Type;
    UINT32 Size;
    CHAR String[];
} ATTRIBUTE(packed);

struct Multiboot2TagBasicMeminfo {
    UINT32 Type;
    UINT32 Size;
    UINT32 MemLower;
    UINT32 MemUpper;
} ATTRIBUTE(packed);

struct Multiboot2TagBootDevice {
    UINT32 Type;
    UINT32 Size;
    UINT32 BiosDevice;
    UINT32 Partition;
    UINT32 SubPartition;
} ATTRIBUTE(packed);

struct Multiboot2TagMmap {
    UINT32 Type;
    UINT32 Size;
    UINT32 EntrySize;
    UINT32 EntryVersion;
    struct {
	UINT64 Addr;
	UINT64 Len;
	UINT32 Type;
	UINT32 Zero;
    } Entries[];
} ATTRIBUTE(packed);

struct Multiboot2TagModule {
    UINT32 Type;
    UINT32 Size;
    UINT32 ModStart;
    UINT32 ModEnd;
    CHAR CmdLine[];
} ATTRIBUTE(packed);

struct Multiboot2TagFramebuffer {
    UINT32 Type;
    UINT32 Size;
    UINT64 FramebufferAddr;
    UINT32 FramebufferPitch;
    UINT32 FramebufferWidth;
    UINT32 FramebufferHeight;
    UINT8 FramebufferBpp;
    UINT8 FramebufferType;
    UINT8 Reserved;
    union {
        struct {
            UINT16 FramebufferPaletteNumColors;
            UINT8 FramebufferPalette[256][4];
        } Palette;
        struct {
            UINT8 FramebufferRedFieldPosition;
            UINT8 FramebufferRedMaskSize;
            UINT8 FramebufferGreenFieldPosition;
            UINT8 FramebufferGreenMaskSize;
            UINT8 FramebufferBlueFieldPosition;
            UINT8 FramebufferBlueMaskSize;
        } Color;
    } U;
} ATTRIBUTE(packed);

struct Multiboot2TagAcpiV1 {
    UINT32 Type;
    UINT32 Size;
    RSDPV1 Rsdp;
} ATTRIBUTE(packed);

struct Multiboot2TagAcpiV2 {
    UINT32 Type;
    UINT32 Size;
    RSDPV2 Rsdp;
} ATTRIBUTE(packed);

struct Multiboot2TagEfi64 {
    UINT32 Type;
    UINT32 Size;
    UINT64 Pointer;
} ATTRIBUTE(packed);

struct Multiboot2TagEfi32 {
    UINT32 Type;
    UINT32 Size;
    UINT32 Pointer;
} ATTRIBUTE(packed);

struct Multiboot2TagEfiMmap {
    UINT32 Type;
    UINT32 Size;
    UINT32 DescSize;
    UINT32 DescVersion;
    UINT8 EfiMmap[];
} ATTRIBUTE(packed);

struct Multiboot2TagLoadBaseAddr {
    UINT32 Type;
    UINT32 Size;
    UINT32 LoadBaseAddr;
} ATTRIBUTE(packed);

static inline UINT32 AlignUp(UINT32 Value, UINT32 Alignment) {
    return (Value + Alignment - 1) & ~(Alignment - 1);
}

INT Multiboot2ParserParse(NOPTR *Multiboot2Addr, Multiboot2Info *Info) {
    if (!Multiboot2Addr || !Info) RETURN(NO_OBJECT);
    UINT32 TotalSize = *(UINT32*)Multiboot2Addr;

    struct Multiboot2Tag* Tag = (struct Multiboot2Tag*)((UINT8*)Multiboot2Addr + 8);

    while (Tag->Type != 0 && (UINT8*)Tag < (UINT8*)Multiboot2Addr + TotalSize) {
        switch (Tag->Type) {
            case 1: {  // Command line
                struct Multiboot2TagString* T = (struct Multiboot2TagString*)Tag;
                Info->CmdLine = T->String;
                break;
            }
            
            case 2: {  // Boot loader name
                struct Multiboot2TagString* T = (struct Multiboot2TagString*)Tag;
                Info->BootLoaderName = T->String;
                break;
            }
            
            case 3: {  // Module
    		struct Multiboot2TagModule* T = (struct Multiboot2TagModule*)Tag;
    
    		// Если первый модуль — выделяем память
    		if (Info->Modules.Count < MAX_MODULES) {
        		Info->Modules.Modules[Info->Modules.Count].Start = T->ModStart;
        		Info->Modules.Modules[Info->Modules.Count].End = T->ModEnd;
       			Info->Modules.Modules[Info->Modules.Count].CmdLine = T->CmdLine;
        		Info->Modules.Count++;
    		}
    		break;
	    }
            
            case 4: {  // Basic meminfo
                struct Multiboot2TagBasicMeminfo* T = (struct Multiboot2TagBasicMeminfo*)Tag;
                Info->BasicMeminfo.MemLower = T->MemLower;
                Info->BasicMeminfo.MemUpper = T->MemUpper;
                break;
            }
            
            case 5: {  // Boot device
                struct Multiboot2TagBootDevice* T = (struct Multiboot2TagBootDevice*)Tag;
                Info->BootDevice.BiosDevice = T->BiosDevice;
                Info->BootDevice.Partition = T->Partition;
                Info->BootDevice.SubPartition = T->SubPartition;
                break;
            }
            
            case 6: {  // Memory map
                struct Multiboot2TagMmap* T = (struct Multiboot2TagMmap*)Tag;
                Info->Mmap.EntrySize = T->EntrySize;

                UINT32 EntriesDataSize = Tag->Size - sizeof(struct Multiboot2TagMmap) + 4;
                Info->Mmap.EntryCount = EntriesDataSize / T->EntrySize;

                NOPTR* EntriesPtr = (NOPTR*)T->Entries;
                Info->Mmap.Entries = (NOPTR*)EntriesPtr;
                break;
            }
            
            case 8: {  // Framebuffer
                struct Multiboot2TagFramebuffer* T = (struct Multiboot2TagFramebuffer*)Tag;
                Info->Framebuffer.Addr = T->FramebufferAddr;
                Info->Framebuffer.Pitch = T->FramebufferPitch;
                Info->Framebuffer.Width = T->FramebufferWidth;
                Info->Framebuffer.Height = T->FramebufferHeight;
                Info->Framebuffer.Bpp = T->FramebufferBpp;
                Info->Framebuffer.Type = T->FramebufferType;
                
                if (T->FramebufferType == 1) {  // RGB
                    Info->Framebuffer.ColorInfo.Rgb.RedFieldPos = T->U.Color.FramebufferRedFieldPosition;
                    Info->Framebuffer.ColorInfo.Rgb.RedMaskSize = T->U.Color.FramebufferRedMaskSize;
                    Info->Framebuffer.ColorInfo.Rgb.GreenFieldPos = T->U.Color.FramebufferGreenFieldPosition;
                    Info->Framebuffer.ColorInfo.Rgb.GreenMaskSize = T->U.Color.FramebufferGreenMaskSize;
                    Info->Framebuffer.ColorInfo.Rgb.BlueFieldPos = T->U.Color.FramebufferBlueFieldPosition;
                    Info->Framebuffer.ColorInfo.Rgb.BlueMaskSize = T->U.Color.FramebufferBlueMaskSize;
                }
                break;
            }
            
            case 14: {  // Old ACPI (v1.0)
    	    	UINT64 PhysRsdp = (UINT64)Multiboot2Addr + ((UINT8*)Tag + 8 - (UINT8*)Multiboot2Addr);
    		Info->Acpi.RsdpV1Addr = PhysRsdp;
    		break;
	    }

	    case 15: {  // New ACPI (v2.0+)
    	    	UINT64 PhysRsdp = (UINT64)Multiboot2Addr + ((UINT8*)Tag + 8 - (UINT8*)Multiboot2Addr);
    		Info->Acpi.RsdpV2Addr = PhysRsdp;
		break;
	    }
            
            case 11: {  // EFI 32-bit
                struct Multiboot2TagEfi32* T = (struct Multiboot2TagEfi32*)Tag;
                Info->Efi.SystemTable32 = T->Pointer;
                break;
            }
            
            case 12: {  // EFI 64-bit
                struct Multiboot2TagEfi64* T = (struct Multiboot2TagEfi64*)Tag;
                Info->Efi.SystemTable64 = T->Pointer;
                break;
            }
            
            case 17: {  // EFI memory map
                struct Multiboot2TagEfiMmap* T = (struct Multiboot2TagEfiMmap*)Tag;
                Info->Efi.Mmap = T->EfiMmap;
                Info->Efi.MmapSize = Tag->Size - sizeof(struct Multiboot2TagEfiMmap);
                Info->Efi.MmapDescSize = T->DescSize;
                Info->Efi.MmapDescVersion = T->DescVersion;
                break;
            }
            
            case 19: {  // EFI 32-bit image handle
                struct Multiboot2TagEfi32* T = (struct Multiboot2TagEfi32*)Tag;
                Info->Efi.ImageHandle32 = (NOPTR*)(UINTPTR)T->Pointer;
                break;
            }
            
            case 20: {  // EFI 64-bit image handle
                struct Multiboot2TagEfi64* T = (struct Multiboot2TagEfi64*)Tag;
                Info->Efi.ImageHandle64 = (NOPTR*)(UINTPTR)T->Pointer;
                break;
            }
            
            case 21: {  // Load base address
                struct Multiboot2TagLoadBaseAddr* T = (struct Multiboot2TagLoadBaseAddr*)Tag;
                Info->LoadBaseAddr = T->LoadBaseAddr;
                break;
            }
        }

        UINT32 NextTagOffset = AlignUp(Tag->Size, 8);
        Tag = (struct Multiboot2Tag*)((UINT8*)Tag + NextTagOffset);
    }
    
    RETURN(SUCCESS);
}