#include <Usb/Ohci.h>
#include <Pci.h>
#include <Asm/Mmio.h>
#include <Asm/Io.h>
#include <Memory/PhysAlloc.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Console.h>
#include <Time/Timer.h>
#include <Kernel/Return.h>
#include <Kernel/Paging.h>
#include <Ioapic.h>
#include <Apic.h>
#include <Kernel/Idt.h>

static OhciCtx *GOhci = NULLPTR;

// ==================== MMIO ACCESS ====================

static inline UINT32 OhciRead32(volatile NOPTR *Base, UINT32 Reg) {
    return MmioRead32((volatile NOPTR*)((UINTPTR)Base + Reg));
}

static inline NOPTR OhciWrite32(volatile NOPTR *Base, UINT32 Reg, UINT32 Val) {
    MmioWrite32((volatile NOPTR*)((UINTPTR)Base + Reg), Val);
}

// ==================== TD/ED ALLOCATION ====================

static OhciTd *OhciAllocTd(NOPTR) {
    OhciTd *Td = (OhciTd*)PhysAllocAllocatePage(PhysAllocGet());
    if (Td) {
        MemSet(Td, 0, PAGE_SIZE);
    }
    return Td;
}

static OhciEd *OhciAllocEd(NOPTR) {
    OhciEd *Ed = (OhciEd*)PhysAllocAllocatePage(PhysAllocGet());
    if (Ed) {
        MemSet(Ed, 0, PAGE_SIZE);
    }
    return Ed;
}

static NOPTR OhciFreeTd(OhciTd *Td) {
    if (Td) {
        PhysAllocFreePage(PhysAllocGet(), (NOPTR*)Td);
    }
}

static NOPTR OhciFreeEd(OhciEd *Ed) {
    if (Ed) {
        PhysAllocFreePage(PhysAllocGet(), (NOPTR*)Ed);
    }
}

// ==================== TD SETUP ====================

static NOPTR OhciSetupTd(OhciTd *Td, UINT32 Flags, UINT32 BufferPhys, UINT32 BufferLen) {
    MemSet(Td, 0, sizeof(OhciTd));
    Td->Flags = Flags | (0xF << 28);  // CC = Not Accessed (0xF)
    Td->Cbp = BufferPhys;
    Td->Be = BufferPhys + BufferLen - 1;
    Td->NextTd = 0;
}

// ==================== PROCESS DONE QUEUE ====================

static NOPTR OhciProcessDoneQueue(OhciCtx *Ctx) {
    UINT32 DoneHead;
    INT Processed = 0;
    
    if (!Ctx->Hcca) return;
    
    DoneHead = Ctx->Hcca->DoneHead;
    if (!DoneHead) return;
    
    Ctx->Hcca->DoneHead = 0;
    
    while (DoneHead) {
        OhciTd *Td = (OhciTd*)(UINTPTR)(DoneHead & ~0xF);
        if (Td && Td->Callback) {
            UINT32 Condition = (Td->Flags >> 28) & 0xF;
            Td->Callback(Td, Condition);
            Processed++;
        }
        DoneHead = Td->NextTd;
    }
}

// ==================== CONTROL TRANSFER ====================

typedef struct {
    UsbDevice *Dev;
    UINT8 *Data;
    UINT32 DataLen;
    UsbSetupPacket Setup;
    INT Result;
    BOOL Done;
} OhciControlContext;

static NOPTR OhciControlCallback(OhciTd *Td, UINT32 Condition) {
    OhciControlContext *Ctx = (OhciControlContext*)Td->Context;
    if (!Ctx) return;
    
    if (Condition == 0) {
        Ctx->Result = SUCCESS;
    } else {
        ConsolePrint("[OHCI] Control transfer TD error: %d\n", Condition);
        Ctx->Result = DEVICE_ERROR;
    }
    Ctx->Done = TRUE;
}

static INT OhciControlTransfer(UsbDevice *Dev, UsbSetupPacket *Setup, 
                                UINT8 *Data, UINT32 DataLen) {
    OhciCtx *Ohci = GOhci;
    OhciTd *SetupTd = NULLPTR;
    OhciTd *DataTd = NULLPTR;
    OhciTd *StatusTd = NULLPTR;
    OhciEd *Ed = NULLPTR;
    OhciControlContext Context;
    INT Timeout = 5000;
    INT Ret = NO_MEMORY;
    
    if (!Ohci || !Dev || !Setup) {
        RETURN(NO_OBJECT);
    }
    
    MemSet(&Context, 0, sizeof(Context));
    Context.Dev = Dev;
    Context.Data = Data;
    Context.DataLen = DataLen;
    Context.Result = TIMEOUT;
    Context.Done = FALSE;
    
    // Allocate TDs
    SetupTd = OhciAllocTd();
    if (!SetupTd) goto cleanup;
    
    if (DataLen > 0 && Data) {
        DataTd = OhciAllocTd();
        if (!DataTd) goto cleanup;
    }
    
    StatusTd = OhciAllocTd();
    if (!StatusTd) goto cleanup;
    
    Ed = OhciAllocEd();
    if (!Ed) goto cleanup;
    
    // Setup TD
    UINT32 SetupPhys = (UINT32)(UINTPTR)VirtToPhysPtr(Setup);
    OhciSetupTd(SetupTd, OHCI_TD_SETUP, SetupPhys, sizeof(UsbSetupPacket));
    SetupTd->Callback = OhciControlCallback;
    SetupTd->Context = &Context;
    SetupTd->NextTd = 0;
    
    // Data TD (if any)
    if (DataTd) {
        UINT32 DataPhys = (UINT32)(UINTPTR)VirtToPhysPtr(Data);
        UINT32 Flags = (Setup->BmRequestType & USB_DIR_IN) ? OHCI_TD_IN : OHCI_TD_OUT;
        OhciSetupTd(DataTd, Flags, DataPhys, DataLen);
        DataTd->Callback = OhciControlCallback;
        DataTd->Context = &Context;
        
        // Chain: Setup -> Data
        SetupTd->NextTd = (UINT32)(UINTPTR)VirtToPhysPtr(DataTd);
        DataTd->NextTd = 0;
    }
    
    // Status TD
    UINT32 StatusFlags = (Setup->BmRequestType & USB_DIR_IN) ? OHCI_TD_OUT : OHCI_TD_IN;
    OhciSetupTd(StatusTd, StatusFlags, 0, 0);
    StatusTd->Callback = OhciControlCallback;
    StatusTd->Context = &Context;
    
    // Chain to status
    if (DataTd) {
        DataTd->NextTd = (UINT32)(UINTPTR)VirtToPhysPtr(StatusTd);
    } else {
        SetupTd->NextTd = (UINT32)(UINTPTR)VirtToPhysPtr(StatusTd);
    }
    
    // Setup ED
    Ed->Flags = (Dev->Address << 0) |           // Function address
                (0 << 7) |                       // Endpoint 0
                (Dev->Speed << 9) |              // Speed
                (0 << 11) |                      // Format (Control)
                (0 << 13) |                      // Skip = 0 (enabled)
                (Dev->MaxPacketSize0 << 16);     // Max packet size
    Ed->HeadP = (UINT32)(UINTPTR)VirtToPhysPtr(SetupTd);
    Ed->TailP = 0;
    
    // Add to Control list
    Ed->NextEd = Ohci->ControlHead;
    Ohci->ControlHead = (UINT32)(UINTPTR)VirtToPhysPtr(Ed);
    OhciWrite32(Ohci->Regs, OHCI_CONTROL_HEAD_ED, Ohci->ControlHead);
    
    // Enable Control List
    UINT32 Ctrl = OhciRead32(Ohci->Regs, OHCI_CONTROL);
    Ctrl |= OHCI_CTRL_CLE;
    OhciWrite32(Ohci->Regs, OHCI_CONTROL, Ctrl);
    
    // Wait for completion
    while (Timeout-- && !Context.Done) {
        OhciProcessDoneQueue(Ohci);
        TimerMdelay(1);
    }
    
    // Cleanup
    Ctrl = OhciRead32(Ohci->Regs, OHCI_CONTROL);
    Ctrl &= ~OHCI_CTRL_CLE;
    OhciWrite32(Ohci->Regs, OHCI_CONTROL, Ctrl);
    OhciWrite32(Ohci->Regs, OHCI_CONTROL_HEAD_ED, 0);
    Ohci->ControlHead = 0;
    
    Ret = Context.Result;
    
cleanup:
    if (SetupTd) OhciFreeTd(SetupTd);
    if (DataTd) OhciFreeTd(DataTd);
    if (StatusTd) OhciFreeTd(StatusTd);
    if (Ed) OhciFreeEd(Ed);
    
    RETURN(Ret);
}

// ==================== PORT MANAGEMENT ====================

static NOPTR OhciPortPowerOn(OhciCtx *Ctx, UINT32 Port) {
    volatile UINT32 *Portsc = (volatile UINT32*)((UINTPTR)Ctx->Regs + OHCI_RH_PORT_STATUS(Port));
    UINT32 Value = OhciRead32(Portsc, 0);
    
    if (!(Value & OHCI_RHPS_PPS)) {
        ConsolePrint("[OHCI] Port %d: enabling power\n", Port);
        Value |= OHCI_RHPS_PPS;
        OhciWrite32(Portsc, 0, Value);
        TimerMdelay(20);
    }
}

static NOPTR OhciScanPorts(OhciCtx *Ctx) {
    for (UINT32 I = 0; I < Ctx->Ports; I++) {
        volatile UINT32 *Portsc = (volatile UINT32*)((UINTPTR)Ctx->Regs + OHCI_RH_PORT_STATUS(I));
        UINT32 Status = OhciRead32(Portsc, 0);
        
        BOOL Connected = (Status & OHCI_RHPS_CCS) != 0;
        
        if (Connected) {
            // Clear change bits
            if (Status & OHCI_RHPS_CSC) {
                OhciWrite32(Portsc, 0, Status | OHCI_RHPS_CSC);
                Status = OhciRead32(Portsc, 0);
            }
            
            // Port reset
            Status |= OHCI_RHPS_PRS;
            OhciWrite32(Portsc, 0, Status);
            TimerMdelay(10);
            Status &= ~OHCI_RHPS_PRS;
            OhciWrite32(Portsc, 0, Status);
            TimerMdelay(10);
            
            Status = OhciRead32(Portsc, 0);
            Status |= OHCI_RHPS_PES;
            OhciWrite32(Portsc, 0, Status);
            
            UINT32 Speed = (Status & OHCI_RHPS_LSDA) ? 1 : 0;
            
            ConsolePrint("[OHCI] Port %d: device connected (speed=%s)\n", 
                         I, Speed ? "Low" : "Full");
            
            // Create USB device
            UsbDevice *Dev = UsbDeviceAlloc(Ctx->Hcd);
            if (Dev) {
                Dev->Address = 0;
                Dev->Speed = Speed;
                Dev->MaxPacketSize0 = 8;
                Dev->Hcd = Ctx->Hcd;
                
                if (UsbEnumeration(Dev) == SUCCESS) {
                    UsbDeviceAdd(Ctx->Hcd, Dev);
                } else {
                    UsbDeviceFree(Dev);
                }
            }
        }
    }
}

// ==================== INTERRUPT HANDLER ====================

static NOPTR OhciIrqHandlerInternal(OhciCtx *Ctx) {
    UINT32 Status;
    
    if (!Ctx || Ctx->Running == FALSE) return;
    
    Status = OhciRead32(Ctx->Regs, OHCI_INTERRUPT_STATUS);
    
    if (Status == 0 || Status == 0xFFFFFFFF) {
        return;
    }
    
    // Clear interrupts
    OhciWrite32(Ctx->Regs, OHCI_INTERRUPT_STATUS, Status);
    
    if (Status & OHCI_INTR_WDH) {
        // Writeback Done Head - transfer completed
        OhciProcessDoneQueue(Ctx);
    }
    
    if (Status & OHCI_INTR_RHSC) {
        ConsolePrint("[OHCI] Root hub status change\n");
        OhciScanPorts(Ctx);
    }
    
    if (Status & OHCI_INTR_UE) {
        ConsolePrint("[OHCI] Unrecoverable error\n");
        Ctx->Running = FALSE;
    }
}

NOPTR OhciIrqHandler(NOPTR *Hcd) {
    OhciCtx *Ctx = GOhci;
    
    if (!Ctx) {
        ApicEoi();
        return;
    }
    
    OhciIrqHandlerInternal(Ctx);
    
    ApicEoi();
    if (!Ctx->MsiEnabled && Ctx->Gsi) {
        IoapicEoi(Ctx->Gsi);
    }
}

static INT OhciSetupIrq(OhciCtx *Ctx, PciDevice *PciDev) {
    UINT32 Flags;
    
    Ctx->Irq = PciDev->InterruptLine;
    if (IoapicGetOverride(Ctx->Irq, &Ctx->Gsi, &Flags) != SUCCESS) {
        Ctx->Gsi = Ctx->Irq;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }
    
    Ctx->Vector = 48 + (Ctx->Gsi % 16);
    
    if (IoapicRedirectIrq(Ctx->Gsi, Ctx->Vector, ApicGetId(), Flags) != SUCCESS) {
        ConsolePrint("[OHCI] Failed to redirect IRQ\n");
        RETURN(IO_ERROR);
    }
    
    IdtSetGate(Ctx->Vector, (NOPTR(*)(NOPTR))OhciIrqHandler, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IoapicUnmaskIrq(Ctx->Gsi);
    
    ConsolePrint("[OHCI] IRQ %d (GSI %d) -> vector %d\n", Ctx->Irq, Ctx->Gsi, Ctx->Vector);
    
    RETURN(SUCCESS);
}

// ==================== CONTROLLER INITIALIZATION ====================

static INT OhciControllerInit(OhciCtx *Ctx) {
    UINT32 Ctrl;
    UINT32 RhDescA;
    INT Timeout;
    
    ConsolePrint("[OHCI] Initializing controller...\n");
    
    // Reset controller
    OhciWrite32(Ctx->Regs, OHCI_COMMAND_STATUS, OHCI_CMD_HCR);
    TimerMdelay(10);
    
    Timeout = 10000;
    while (Timeout-- && (OhciRead32(Ctx->Regs, OHCI_COMMAND_STATUS) & OHCI_CMD_HCR)) {
        TimerUdelay(100);
    }
    
    if (Timeout == 0) {
        ConsolePrint("[OHCI] Reset timeout\n");
        RETURN(TIMEOUT);
    }
    
    // Allocate HCCA (must be 256-byte aligned)
    Ctx->Hcca = (OhciHcca*)PhysAllocAllocateAlignedRange(PhysAllocGet(), 1, 64);
    if (!Ctx->Hcca) RETURN(NO_MEMORY);
    MemSet((NOPTR*)Ctx->Hcca, 0, PAGE_SIZE);
    Ctx->HccaPhys = (UINT32)(UINTPTR)VirtToPhysPtr((NOPTR*)Ctx->Hcca);
    
    OhciWrite32(Ctx->Regs, OHCI_HCCA, Ctx->HccaPhys);
    
    // Get number of ports
    RhDescA = OhciRead32(Ctx->Regs, OHCI_RH_DESCRIPTOR_A);
    Ctx->Ports = RhDescA & 0xFF;
    ConsolePrint("[OHCI] Ports: %d\n", Ctx->Ports);
    
    // Set frame interval (1 ms)
    OhciWrite32(Ctx->Regs, OHCI_FM_INTERVAL, 0x2EDF);
    OhciWrite32(Ctx->Regs, OHCI_PERIOD_START, 0x2EDF);
    OhciWrite32(Ctx->Regs, OHCI_LS_THRESHOLD, 0x628);
    
    // Set HCFS to Operational
    Ctrl = OhciRead32(Ctx->Regs, OHCI_CONTROL);
    Ctrl &= ~OHCI_CTRL_HCFS;
    Ctrl |= (2 << 6);  // Operational
    Ctrl |= OHCI_CTRL_PLE | OHCI_CTRL_CLE | OHCI_CTRL_BLE;
    OhciWrite32(Ctx->Regs, OHCI_CONTROL, Ctrl);
    
    // Clear and enable interrupts
    OhciWrite32(Ctx->Regs, OHCI_INTERRUPT_STATUS, 0xFFFFFFFF);
    OhciWrite32(Ctx->Regs, OHCI_INTERRUPT_ENABLE, 
                OHCI_INTR_RHSC | OHCI_INTR_WDH | OHCI_INTR_UE | OHCI_INTR_MIE);
    
    // Power on ports
    for (UINT32 I = 0; I < Ctx->Ports; I++) {
        OhciPortPowerOn(Ctx, I);
    }
    
    TimerMdelay(50);
    
    Ctx->Running = TRUE;
    ConsolePrint("[OHCI] Controller running\n");
    
    // Initial port scan
    OhciScanPorts(Ctx);
    
    RETURN(SUCCESS);
}

// ==================== HCD CALLBACKS ====================

static INT OhciHcdInit(UsbHcd *Hcd) {
    OhciCtx *Ctx = (OhciCtx*)Hcd->Private;
    if (!Ctx) RETURN(NO_OBJECT);
    
    return OhciControllerInit(Ctx);
}

static INT OhciHcdShutdown(UsbHcd *Hcd) {
    OhciCtx *Ctx = (OhciCtx*)Hcd->Private;
    if (!Ctx) RETURN(NO_OBJECT);
    
    // Stop controller
    UINT32 Ctrl = OhciRead32(Ctx->Regs, OHCI_CONTROL);
    Ctrl &= ~OHCI_CTRL_HCFS;
    Ctrl |= (0 << 6);  // Reset
    OhciWrite32(Ctx->Regs, OHCI_CONTROL, Ctrl);
    
    // Disable interrupts
    OhciWrite32(Ctx->Regs, OHCI_INTERRUPT_DISABLE, 0xFFFFFFFF);
    
    Ctx->Running = FALSE;
    
    if (Ctx->Hcca) {
        PhysAllocFreePage(PhysAllocGet(), (NOPTR*)Ctx->Hcca);
        Ctx->Hcca = NULLPTR;
    }
    
    RETURN(SUCCESS);
}

static INT OhciHcdSubmitTransfer(UsbHcd *Hcd, UsbTransfer *Transfer) {
    INT Ret;

    (NOPTR)Hcd;
    if (!Transfer || !Transfer->Device) {
        RETURN(NO_OBJECT);
    }

    switch (Transfer->Type) {
    case USB_TRANSFER_CONTROL:
        Ret = OhciControlTransfer(Transfer->Device,
                                  (UsbSetupPacket*)Transfer->Context,
                                  Transfer->Buffer, Transfer->Length);
        Transfer->Status = Ret;
        RETURN(Ret);

    case USB_TRANSFER_INTERRUPT:
        if (Transfer->Callback) {
            Transfer->Status = 0;
            RETURN(SUCCESS);
        }
        Transfer->Status = NOT_IMPLEMENTED;
        RETURN(NOT_IMPLEMENTED);

    default:
        Transfer->Status = NOT_IMPLEMENTED;
        RETURN(NOT_IMPLEMENTED);
    }
}

static UsbHcdOps GOhciHcdOps = {
    .Init = OhciHcdInit,
    .Shutdown = OhciHcdShutdown,
    .Reset = NULLPTR,
    .SubmitTransfer = OhciHcdSubmitTransfer,
    .CancelTransfer = NULLPTR,
    .IrqHandler = (NOPTR(*)(NOPTR))OhciIrqHandler,
};

// ==================== PCI PROBE ====================

INT OhciInit(PciDevice *PciDev) {
    OhciCtx *Ctx;
    UsbHcd *Hcd;
    UINT32 MmioBase;
    
    if (!PciDev) RETURN(NO_OBJECT);
    
    ConsolePrint("[OHCI] Found controller at %02X:%02X.%X (%04X:%04X)\n",
                 PciDev->Bus, PciDev->Slot, PciDev->Function,
                 PciDev->VendorId, PciDev->DeviceId);
    
    // Enable bus mastering and memory space
    PciEnableBusmaster(PciDev);
    PciEnable(PciDev);
    
    // Get MMIO base (BAR0)
    MmioBase = PciDev->Bars[0] & ~0xF;
    if (!MmioBase) {
        ConsolePrint("[OHCI] No MMIO BAR\n");
        RETURN(NO_OBJECT);
    }
    
    ConsolePrint("[OHCI] MMIO base: 0x%x\n", MmioBase);
    
    // Allocate context
    Ctx = (OhciCtx*)MemoryAllocate(sizeof(OhciCtx));
    if (!Ctx) RETURN(NO_MEMORY);
    MemSet(Ctx, 0, sizeof(OhciCtx));
    
    Ctx->Regs = (volatile NOPTR*)(UINTPTR)MmioBase;
    Ctx->PciDev = PciDev;
    Ctx->ControlHead = 0;
    Ctx->BulkHead = 0;
    Ctx->MsiEnabled = FALSE;
    
    // Create HCD
    Hcd = (UsbHcd*)MemoryAllocate(sizeof(UsbHcd));
    if (!Hcd) {
        MemoryFree(Ctx);
        RETURN(NO_MEMORY);
    }
    
    MemSet(Hcd, 0, sizeof(UsbHcd));
    SnPrintf(Hcd->Name, sizeof(Hcd->Name), "ohci");
    Hcd->VendorId = PciDev->VendorId;
    Hcd->DeviceId = PciDev->DeviceId;
    Hcd->MmioBase = MmioBase;
    Hcd->Irq = PciDev->InterruptLine;
    Hcd->Ops = &GOhciHcdOps;
    Hcd->Private = Ctx;
    
    Ctx->Hcd = Hcd;
    GOhci = Ctx;
    
    // Setup IRQ
    if (OhciSetupIrq(Ctx, PciDev) != SUCCESS) {
        ConsolePrint("[OHCI] IRQ setup failed, using polling\n");
    }
    
    // Register with USB core
    if (UsbHcdRegister(Hcd) != SUCCESS) {
        MemoryFree(Hcd);
        MemoryFree(Ctx);
        RETURN(DEVICE_ERROR);
    }
    
    RETURN(SUCCESS);
}

NOPTR OhciProbeAll(NOPTR) {
    PciDevice *Dev = PciGetFirst();
    
    while (Dev) {
        if (Dev->ClassCode == 0x0C && Dev->SubClass == 0x03 && Dev->ProgIf == 0x10) {
            OhciInit(Dev);
        }
        Dev = PciGetNext(Dev);
    }
}