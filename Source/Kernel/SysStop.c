#include <Kernel/Types.h>
#include <FBDevice.h>
#include <Acpi.h>
#include <RgbColor.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Asm/Cpu.h>
#include <Kernel/SysStop.h>

static void SysGetRegs(SysStopRegisterState *Regs, NOPTR *CallerRip) {
    if (!Regs) return;
    
    asm volatile(
        "movq %%rsp, 0x00(%0)\n\t"
        "movq %1, 0x08(%0)\n\t"          // RIP из параметра
        "pushfq\n\t"
        "popq %%rax\n\t"
        "movq %%rax, 0x10(%0)\n\t"
        "movw %%cs, 0x18(%0)\n\t"
        "movw %%ds, 0x1A(%0)\n\t"
        "movw %%es, 0x1C(%0)\n\t"
        "movw %%fs, 0x1E(%0)\n\t"
        "movw %%gs, 0x20(%0)\n\t"
        "movw %%ss, 0x22(%0)\n\t"
        "mov %%cr0, %%rax\n\t"
        "movq %%rax, 0x28(%0)\n\t"
        "mov %%cr2, %%rax\n\t"
        "movq %%rax, 0x30(%0)\n\t"
        "mov %%cr3, %%rax\n\t"
        "movq %%rax, 0x38(%0)\n\t"
        "mov %%cr4, %%rax\n\t"
        "movq %%rax, 0x40(%0)\n\t"
        :
        : "r" (Regs), "r" (CallerRip)
        : "memory", "rax"
    );
}

// 0xXXXXXXXXXXXXXXXX
static NOPTR UINT64ToHexStr(UINT64 Value, CHAR *Buffer) {
    const CHAR* HexDigits = "0123456789ABCDEF";
    Buffer[0] = '0';
    Buffer[1] = 'x';
    for (INT I = 0; I < 16; I++) {
	UINT8 Nibble = (Value >> (60 - I * 4)) & 0xF;
	Buffer[I + 2] = HexDigits[Nibble];
    }
    Buffer[18] = '\0';
}

// 0xXXXX
static NOPTR UINT16ToHexStr(UINT16 Value, CHAR *Buffer) {
    const CHAR* HexDigits = "0123456789ABCDEF";
    Buffer[0] = '0';
    Buffer[1] = 'x';
    for (INT I = 0; I < 4; I++) {
	UINT8 Nibble = (Value >> ((3 - I) * 4)) & 0xF;
	Buffer[I + 2] = HexDigits[Nibble];
    }
    Buffer[6] = '\0';
}

NOPTR PrintRegisters(const SysStopRegisterState *Regs) {
    if (!Regs) return;
    
    FBDeviceDrawString("REGISTER DUMP:", 1, (FONT_LINE_HEIGHT * 2), RGB_BLACK, RGB_RED);
    UINT64 Rsp = Regs->Rsp;
    UINT64 Rip = Regs->Rip;
    UINT64 Rflags = Regs->Rflags;
    UINT16 Cs = Regs->Cs;
    UINT16 Ds = Regs->Ds;
    UINT16 Es = Regs->Es;
    UINT16 Fs = Regs->Fs;
    UINT16 Gs = Regs->Gs;
    UINT16 Ss = Regs->Ss;
    UINT64 Cr0 = Regs->Cr0;
    UINT64 Cr2 = Regs->Cr2;
    UINT64 Cr3 = Regs->Cr3;
    UINT64 Cr4 = Regs->Cr4;
    FBDeviceDrawString("  RSP=", 1, (FONT_LINE_HEIGHT * 3), RGB_BLACK, RGB_RED);
    CHAR RspBuffer[19];
    UINT64ToHexStr(Rsp, RspBuffer);
    FBDeviceDrawString(RspBuffer, (1 + (FONT_WIDTH * StrLen("  RSP="))), (FONT_LINE_HEIGHT * 3), RGB_BLACK, RGB_RED);
    FBDeviceDrawString("  RIP=", 1, (FONT_LINE_HEIGHT * 4), RGB_BLACK, RGB_RED);
    CHAR RipBuffer[19];
    UINT64ToHexStr(Rip, RipBuffer);
    FBDeviceDrawString(RipBuffer, (1 + (FONT_WIDTH * StrLen("  RIP="))), (FONT_LINE_HEIGHT * 4), RGB_BLACK, RGB_RED);
    FBDeviceDrawString("  RFLAGS=", 1, (FONT_LINE_HEIGHT * 5), RGB_BLACK, RGB_RED);
    CHAR RflagsBuffer[19];
    UINT64ToHexStr(Rflags, RflagsBuffer);
    FBDeviceDrawString(RflagsBuffer, (1 + (FONT_WIDTH * StrLen("  RFLAGS="))), (FONT_LINE_HEIGHT * 5), RGB_BLACK, RGB_RED);
    FBDeviceDrawString("  CS=", 1, (FONT_LINE_HEIGHT * 6), RGB_BLACK, RGB_RED);
    CHAR CsBuffer[7];
    UINT16ToHexStr(Cs, CsBuffer);
    FBDeviceDrawString(CsBuffer, (1 + (FONT_WIDTH * StrLen("  CS="))), (FONT_LINE_HEIGHT * 6), RGB_BLACK, RGB_RED);
    FBDeviceDrawString(" DS=", (1 + (FONT_WIDTH * StrLen("  CS=      "))), (FONT_LINE_HEIGHT * 6), RGB_BLACK, RGB_RED);
    CHAR DsBuffer[7];
    UINT16ToHexStr(Ds, DsBuffer);
    FBDeviceDrawString(DsBuffer, (1 + (FONT_WIDTH * StrLen("  CS=       DS="))), (FONT_LINE_HEIGHT * 6), RGB_BLACK, RGB_RED);
    FBDeviceDrawString("  ES=", 1, (FONT_LINE_HEIGHT * 7), RGB_BLACK, RGB_RED);
    CHAR EsBuffer[7];
    UINT16ToHexStr(Es, EsBuffer);
    FBDeviceDrawString(EsBuffer, (1 + (FONT_WIDTH * StrLen("  ES="))), (FONT_LINE_HEIGHT * 7), RGB_BLACK, RGB_RED);
    FBDeviceDrawString(" FS=", (1 + (FONT_WIDTH * StrLen("  ES=      "))), (FONT_LINE_HEIGHT * 7), RGB_BLACK, RGB_RED);
    CHAR FsBuffer[7];
    UINT16ToHexStr(Fs, FsBuffer);
    FBDeviceDrawString(FsBuffer, (1 + (FONT_WIDTH * StrLen("  ES=       FS="))), (FONT_LINE_HEIGHT * 7), RGB_BLACK, RGB_RED);
    FBDeviceDrawString("  GS=", 1, (FONT_LINE_HEIGHT * 8), RGB_BLACK, RGB_RED);
    CHAR GsBuffer[7];
    UINT16ToHexStr(Gs, GsBuffer);
    FBDeviceDrawString(GsBuffer, (1 + (FONT_WIDTH * StrLen("  GS="))), (FONT_LINE_HEIGHT * 8), RGB_BLACK, RGB_RED);
    FBDeviceDrawString(" SS=", (1 + (FONT_WIDTH * StrLen("  GS=      "))), (FONT_LINE_HEIGHT * 8), RGB_BLACK, RGB_RED);
    CHAR SsBuffer[7];
    UINT16ToHexStr(Ss, SsBuffer);
    FBDeviceDrawString(SsBuffer, (1 + (FONT_WIDTH * StrLen("  GS=       SS="))), (FONT_LINE_HEIGHT * 8), RGB_BLACK, RGB_RED);
    FBDeviceDrawString("  CR0=", 1, (FONT_LINE_HEIGHT * 9), RGB_BLACK, RGB_RED);
    CHAR Cr0Buffer[19];
    UINT64ToHexStr(Cr0, Cr0Buffer);
    FBDeviceDrawString(Cr0Buffer, (1 + (FONT_WIDTH * StrLen("  CR0="))), (FONT_LINE_HEIGHT * 9), RGB_BLACK, RGB_RED);
    FBDeviceDrawString("  CR2=", 1, (FONT_LINE_HEIGHT * 10), RGB_BLACK, RGB_RED);
    CHAR Cr2Buffer[19];
    UINT64ToHexStr(Cr2, Cr2Buffer);
    FBDeviceDrawString(Cr2Buffer, (1 + (FONT_WIDTH * StrLen("  CR2="))), (FONT_LINE_HEIGHT * 10), RGB_BLACK, RGB_RED);
    FBDeviceDrawString("  CR3=", 1, (FONT_LINE_HEIGHT * 11), RGB_BLACK, RGB_RED);
    CHAR Cr3Buffer[19];
    UINT64ToHexStr(Cr3, Cr3Buffer);
    FBDeviceDrawString(Cr3Buffer, (1 + (FONT_WIDTH * StrLen("  CR3="))), (FONT_LINE_HEIGHT * 11), RGB_BLACK, RGB_RED);
    FBDeviceDrawString("  CR4=", 1, (FONT_LINE_HEIGHT * 12), RGB_BLACK, RGB_RED);
    CHAR Cr4Buffer[19];
    UINT64ToHexStr(Cr4, Cr4Buffer);
    FBDeviceDrawString(Cr4Buffer, (1 + (FONT_WIDTH * StrLen("  CR4="))), (FONT_LINE_HEIGHT * 12), RGB_BLACK, RGB_RED);
    FBDeviceSwapBuffers(); 
}

static NOPTR StrCopy(CHAR *Dest, const CHAR *Src) {
    while (*Src) {
        *Dest++ = *Src++;
    }
    *Dest = '\0';
}

static NOPTR DumpMemory(UINT64 Address, INT Lines) {
    CHAR Line[128];
    CHAR HexBuf[20];
    INT32 BaseX = 1;
    
    FBDeviceDrawString("MEMORY DUMP AROUND ", BaseX, (FONT_LINE_HEIGHT * 14), RGB_BLACK, RGB_RED);
    UINT64ToHexStr(Address, HexBuf);
    FBDeviceDrawString(HexBuf, (BaseX + (StrLen("MEMORY DUMP AROUND 0x") * FONT_WIDTH)), (FONT_LINE_HEIGHT * 14), RGB_BLACK, RGB_RED);
    
    UINT64 *Ptr = (UINT64*)(Address & ~0xF);
    UINT64 StartAddr = (UINT64)Ptr - ((Lines / 2) * 16);
    
    for (INT I = 0; I < Lines; I++) {
        UINT64 CurrentAddr = StartAddr + (I * 16);
        if (CurrentAddr < 0x1000 || CurrentAddr > 0xFFFFFFFFFFFFFFF0) {
            continue;
        }
        
        UINT64ToHexStr(CurrentAddr, HexBuf);
        INT Pos = 0;
        StrCopy(Line, HexBuf);
        Pos = 18;
        Line[Pos++] = ':';
        Line[Pos++] = ' ';
        
        for (INT J = 0; J < 2; J++) {
            if (CurrentAddr + J * 8 < 0xFFFFFFFFFFFFFFF8) {
                UINT64 Value = *(UINT64*)(CurrentAddr + J * 8);
                UINT64ToHexStr(Value, HexBuf);
                for (INT K = 0; K < 18; K++) {
                    Line[Pos++] = HexBuf[K];
                }
                Line[Pos++] = ' ';
            } else {
                StrCopy(Line + Pos, "???????????????? ");
                Pos += 18;
            }
        }
        Line[Pos] = '\0';
        
        // Каждая строка на новой Y позиции!
        FBDeviceDrawString(Line, (BaseX + FONT_WIDTH), (FONT_LINE_HEIGHT * (15 + I)), RGB_BLACK, RGB_RED);
    }
    FBDeviceSwapBuffers();
}

void SysStopImpl(const CHAR *Message, NOPTR *CallerRip) {
    LocalInterruptsDisable();
    FBDeviceClear(RGB_RED);
    FBDeviceDrawString(" Thunder Operating System Failure ", 
    	(FBDeviceGetWidth() - (StrLen(" Thunder Operating System Failure ") * FONT_WIDTH)) / 2, 
    	1, RGB_RED, RGB_BLACK);
    char StopBuffer[64];
    SnPrintf(StopBuffer, sizeof(StopBuffer), " STOP: %s ", Message);
    FBDeviceDrawString(StopBuffer, 1, (FONT_LINE_HEIGHT * 25), RGB_RED, RGB_BLACK);
    FBDeviceSwapBuffers();
    SysStopRegisterState Regs;
    SysGetRegs(&Regs, CallerRip);
    PrintRegisters(&Regs);
    DumpMemory(Regs.Rip, 8);
    for (;;) {
        Halt();
    }
}