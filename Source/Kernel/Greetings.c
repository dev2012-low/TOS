#include <Kernel/Types.h>
#include <Multiboot2Parser.h>
#include <Console.h>
#include <Greetings.h>
#include <Smbios.h>
#include <FBDevice.h>
#include <Rgb.h>
#include <Acpi.h>
#include <Apic.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Asm/Cpu.h>
#include <Gui/Logo32.h>
#include <Gui/Rus.h>

#define GREETINGS_BOX_INSET 3

static NOPTR DrawDoubleRect(INT32 X, INT32 Y, UINT32 W, UINT32 H, FBDeviceColor Color) {
	FBDeviceDrawRect(X, Y, W, H, Color);
	FBDeviceDrawRect(X + GREETINGS_BOX_INSET, Y + GREETINGS_BOX_INSET,
	                 W - GREETINGS_BOX_INSET * 2, H - GREETINGS_BOX_INSET * 2, Color);
}

static NOPTR GreetingsDrawHeader(const CHAR *Title) {
	USIZE TitleLen = StrLen(Title);
	UINT32 TitleWidth = (UINT32)TitleLen * FONT_WIDTH;
	UINT32 InnerW = TitleWidth + 32;
	UINT32 InnerH = FONT_HEIGHT + 16;
	UINT32 OuterW = InnerW + GREETINGS_BOX_INSET * 2;
	UINT32 OuterH = InnerH + GREETINGS_BOX_INSET * 2;
	INT32 BoxX = (INT32)((FBDeviceGetWidth() - OuterW) / 2);
	INT32 BoxY = 8;

	DrawDoubleRect(BoxX, BoxY, OuterW, OuterH, RGB_WHITE);

	INT32 TextX = BoxX + (INT32)GREETINGS_BOX_INSET + (INT32)((InnerW - TitleWidth) / 2);
	INT32 TextY = BoxY + (INT32)GREETINGS_BOX_INSET + (INT32)((InnerH - FONT_HEIGHT) / 2);
	FBDeviceDrawString(Title, TextX, TextY, RGB_CYAN, RGB_BLACK);
	FBDeviceSwapBuffers();

        FBDeviceDrawBitmap(BoxX - 35, BoxY + 2, 32, 32, Logo32);

	UINT32 FirstFreeLine = (UINT32)(BoxY + (INT32)OuterH + FONT_LINE_HEIGHT - 1) / FONT_LINE_HEIGHT;
	for (UINT32 I = 0; I < FirstFreeLine; I++) {
		ConsolePrint(" \n");
	}
}

static NOPTR CopyTrimmedField(CHAR *Dst, USIZE DstSize, const CHAR *Src, USIZE Len) {
	USIZE Start = 0;
	USIZE End = Len;
	USIZE OutLen;

	while (Start < End && Src[Start] == ' ') {
		Start++;
	}
	while (End > Start && Src[End - 1] == ' ') {
		End--;
	}
	OutLen = End - Start;
	if (OutLen >= DstSize) {
		OutLen = DstSize - 1;
	}
	MemCpy(Dst, Src + Start, OutLen);
	Dst[OutLen] = '\0';
}

static NOPTR FormatMemSize(CHAR *Buf, USIZE Size, UINT64 Bytes) {
	if (Bytes >= 1024ULL * 1024 * 1024) {
		SnPrintf(Buf, Size, "%llu GB", Bytes / (1024ULL * 1024 * 1024));
	} else if (Bytes >= 1024ULL * 1024) {
		SnPrintf(Buf, Size, "%llu MB", Bytes / (1024ULL * 1024));
	} else if (Bytes >= 1024) {
		SnPrintf(Buf, Size, "%llu KB", Bytes / 1024);
	} else {
		SnPrintf(Buf, Size, "%llu B", Bytes);
	}
}

static UINT32 CountEnabledCpus(Acpi *Table) {
	UINT32 Count = 0;
	if (!Table) {
		return 0;
	}
	for (UINT32 I = 0; I < Table->Apic.ProcessorCount; I++) {
		if (Table->Apic.Processors[I].Enabled) {
			Count++;
		}
	}
	return Count;
}

static inline NOPTR CpuidGetBrandString(CHAR *Brand) {
	UINT32 Eax, Ebx, Ecx, Edx;

	Cpuid(0x80000000, &Eax, &Ebx, &Ecx, &Edx);
	if (Eax < 0x80000004) {
		StrCpy(Brand, "Unknown CPU");
		return;
	}

	CHAR *Ptr = Brand;

	Cpuid(0x80000002, &Eax, &Ebx, &Ecx, &Edx);
	*(UINT32*)(Ptr + 0) = Eax;
	*(UINT32*)(Ptr + 4) = Ebx;
	*(UINT32*)(Ptr + 8) = Ecx;
	*(UINT32*)(Ptr + 12) = Edx;

	Cpuid(0x80000003, &Eax, &Ebx, &Ecx, &Edx);
	*(UINT32*)(Ptr + 16) = Eax;
	*(UINT32*)(Ptr + 20) = Ebx;
	*(UINT32*)(Ptr + 24) = Ecx;
	*(UINT32*)(Ptr + 28) = Edx;

	Cpuid(0x80000004, &Eax, &Ebx, &Ecx, &Edx);
	*(UINT32*)(Ptr + 32) = Eax;
	*(UINT32*)(Ptr + 36) = Ebx;
	*(UINT32*)(Ptr + 40) = Ecx;
	*(UINT32*)(Ptr + 44) = Edx;

	Brand[48] = '\0';

	while (Brand[0] == ' ') {
		for (INT I = 0; Brand[I]; I++) {
			Brand[I] = Brand[I + 1];
		}
	}
}

static NOPTR GreetingsPrintBootInfo(Multiboot2Info *Info) {
	Acpi *AcpiTable = AcpiGetTable();
	CHAR OemId[16];
	CHAR BiosId[16];
	CHAR TotalMem[16];
	CHAR UsableMem[16];
	CHAR CpuBrand[49];
	const CHAR *Loader = Info->BootLoaderName ? Info->BootLoaderName : "?";
	const CHAR *BootMode = (Info->Efi.SystemTable64 || Info->Efi.SystemTable32) ? "EFI" : "BIOS";
	UINT64 TotalBytes = Multiboot2ParserGetTotalMemory(Info);
	UINT64 UsableBytes = Multiboot2GetUsableMemory(Info);
	UINT32 CpuCount = CountEnabledCpus(AcpiTable);
	UINT8 BootDisk = Multiboot2ParserGetBootDisk(Info);
	UINT32 BootPart = Multiboot2ParserGetBootPartition(Info);

	OemId[0] = '\0';
	BiosId[0] = '\0';
	if (AcpiTable && AcpiTable->Rsdp) {
		CopyTrimmedField(OemId, sizeof(OemId), AcpiTable->Rsdp->V1.OemId, 6);
	}
	if (AcpiTable && AcpiTable->Fadt) {
		CopyTrimmedField(BiosId, sizeof(BiosId), AcpiTable->Fadt->Header.OemTableId, 8);
	}
	if (SmbiosIsReady()) {
		const CHAR *SmbBios = SmbiosGetBiosVendor();
		const CHAR *SmbSys = SmbiosGetSystemProduct();
		if (SmbBios[0]) {
			StrnCpy(BiosId, SmbBios, sizeof(BiosId) - 1);
			BiosId[sizeof(BiosId) - 1] = '\0';
		}
		if (SmbSys[0]) {
			StrnCpy(OemId, SmbSys, sizeof(OemId) - 1);
			OemId[sizeof(OemId) - 1] = '\0';
		}
	}

	FormatMemSize(TotalMem, sizeof(TotalMem), TotalBytes);
	FormatMemSize(UsableMem, sizeof(UsableMem), UsableBytes);
	CpuidGetBrandString(CpuBrand);

	ConsolePrint(" Boot: %s | OEM: %s | %s", Loader, OemId[0] ? OemId : "?", BootMode);
	if (Info->BootDevice.BiosDevice != 0) {
		ConsolePrint(" | Disk 0x%02X", BootDisk);
		if (BootPart != 0xFFFFFFFF) {
			ConsolePrint(":%u", BootPart + 1);
		}
	}
	ConsolePrint("\n");

	if (AcpiTable && AcpiTable->Fadt) {
		ConsolePrint(" BIOS: %s rev 0x%08X | ACPI r%u",
		             BiosId[0] ? BiosId : OemId,
		             AcpiTable->Fadt->Header.OemRevision,
		             AcpiTable->Rsdp ? AcpiTable->Rsdp->V1.Revision : 0);
		if (Info->LoadBaseAddr) {
			ConsolePrint(" | Load 0x%X", (UINT32)Info->LoadBaseAddr);
		}
		ConsolePrint("\n");
	} else if (AcpiTable && AcpiTable->Rsdp) {
		ConsolePrint(" ACPI: r%u", AcpiTable->Rsdp->V1.Revision);
		if (Info->LoadBaseAddr) {
			ConsolePrint(" | Load 0x%X", (UINT32)Info->LoadBaseAddr);
		}
		ConsolePrint("\n");
	}

	if (Info->Framebuffer.Width && Info->Framebuffer.Height) {
		ConsolePrint(" Mem: %s (%s usable) | %ux%ux%u\n",
		             TotalMem, UsableMem,
		             Info->Framebuffer.Width,
		             Info->Framebuffer.Height,
		             Info->Framebuffer.Bpp);
	} else {
		ConsolePrint(" Mem: %s (%s usable)\n", TotalMem, UsableMem);
	}

	if (CpuCount > 0) {
		const CHAR *ApicMode = ApicIsX2ApicMode() ? "x2APIC" :
		                       (AcpiTable && AcpiTable->Apic.UsesX2Apic) ? "x2APIC (ACPI)" : "xAPIC";
		ConsolePrint(" CPU: %s | %u core%s | %s\n",
		             CpuBrand, CpuCount, CpuCount == 1 ? "" : "s", ApicMode);
	} else {
		const CHAR *ApicMode = ApicIsX2ApicMode() ? " | x2APIC" : "";
		ConsolePrint(" CPU: %s%s\n", CpuBrand, ApicMode);
	}

        ConsolePrint("    By Aleksey Kazakevich\n");
        FBDeviceDrawBitmap(8, (48 * 2) + 16, 16, 16, Rus);
}

NOPTR GreetingsPrint(NOPTR) {
	Multiboot2Info *Info = Multiboot2ParserGet();
	CHAR *Title = "Thunder Operating System";

	GreetingsDrawHeader(Title);
	GreetingsPrintBootInfo(Info);
}
