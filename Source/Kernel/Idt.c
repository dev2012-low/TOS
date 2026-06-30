#include <Kernel/Idt.h>
#include <Asm/Io.h>
#include <Asm/Cpu.h>
#include <Kernel/Ints.h>
#include <Apic.h>
#include <Kernel/Types.h>

struct IdtEntry Idt[IDT_ENTRIES];
struct IdtPtr Idtp;

struct {
    UINT16 Limit;
    UINT64 Base;
} GDT64Pointer64;

struct {
    UINT16 Limit;
    UINT64 Base;
} IdtPtr;

NOPTR IdtSetGate(UINT8 Num, NOPTR (*Handler)(), UINT16 Sel, UINT8 Flags, UINT8 Ist) {
    if (Num >= IDT_ENTRIES)
        return;

    UINT64 Base = (UINT64)Handler;
    Idt[Num].OffsetLow = (UINT16)(Base & 0xFFFF);
    Idt[Num].Selector = Sel;
    Idt[Num].Ist = Ist & 0x07;
    Idt[Num].TypeAttr = Flags;
    Idt[Num].OffsetMid = (UINT16)((Base >> 16) & 0xFFFF);
    Idt[Num].OffsetHigh = (UINT32)((Base >> 32) & 0xFFFFFFFF);
    Idt[Num].Zero = 0;
}

NOPTR IdtInit(NOPTR) {
    Idtp.Limit = (UINT16)(sizeof(Idt) - 1);
    Idtp.Base = (UINT64)&Idt;

    static const NOPTR (*Stubs[32])() = {
        IsrStub9, IsrStub15, IsrStub17,
        IsrStub20, IsrStub21, IsrStub22, IsrStub23,
        IsrStub24, IsrStub25, IsrStub26, IsrStub27,
        IsrStub28, IsrStub29, IsrStub30, IsrStub31
    };

    for (INT I = 0; I < 32; ++I)
    {
        IdtSetGate(I, Stubs[I], KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    }

    IdtSetGate(0, DivideErrorStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(1, DebugExceptionStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(2, NmiStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(3, BreakpointStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(4, OverflowStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(5, BoundStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(6, InvalidOpcodeStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(7, DeviceNotAvailableStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(8, DoubleFaultStub, KERNEL_CODE_SEL, IDT_GATE_INT, 1);
    IdtSetGate(10, InvalidTssStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(11, SegmentNotPresentStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(12, StackSegmentStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(13, GeneralProtectionStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(14, PageFaultStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(16, FpuErrorStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(18, MachineCheckStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(19, SimdErrorStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);

    IdtSetGate(32, TimerIsr, KERNEL_CODE_SEL, IDT_GATE_INT, 0);

    LoadIDT(&Idtp);
}
