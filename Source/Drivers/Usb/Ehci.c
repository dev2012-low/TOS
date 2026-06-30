// Drivers/Usb/Ehci.c
#include <Usb/Ehci.h>
#include <Pci.h>
#include <Asm/Mmio.h>
#include <Memory/PhysAlloc.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Console.h>
#include <Time/Timer.h>
#include <Kernel/Paging.h>
#include <Kernel/Return.h>
#include <Kernel/KDriver.h>
#include <Ioapic.h>
#include <Apic.h>
#include <Kernel/Idt.h>

static EhciContext *GEhci = NULLPTR;

// ============ MMIO Helpers ============
static inline UINT32 EhciReadCap(EhciContext *Ctx, UINT32 Reg) {
    return MmioRead32((volatile UINT32*)((UINTPTR)Ctx->CapRegs + Reg));
}

static inline UINT32 EhciReadOp(EhciContext *Ctx, UINT32 Reg) {
    return MmioRead32((volatile UINT32*)((UINTPTR)Ctx->OpRegs + Reg));
}

static inline void EhciWriteOp(EhciContext *Ctx, UINT32 Reg, UINT32 Val) {
    MmioWrite32((volatile UINT32*)((UINTPTR)Ctx->OpRegs + Reg), Val);
}

static inline void EhciSetOpBit(EhciContext *Ctx, UINT32 Reg, UINT32 Bit) {
    EhciWriteOp(Ctx, Reg, EhciReadOp(Ctx, Reg) | Bit);
}

static inline void EhciClearOpBit(EhciContext *Ctx, UINT32 Reg, UINT32 Bit) {
    EhciWriteOp(Ctx, Reg, EhciReadOp(Ctx, Reg) & ~Bit);
}

static inline BOOL EhciIsHalt(EhciContext *Ctx) {
    return (EhciReadOp(Ctx, EHC_USBSTS_OFFSET) & USBSTS_HALT) != 0;
}

static inline BOOL EhciIsSysError(EhciContext *Ctx) {
    return (EhciReadOp(Ctx, EHC_USBSTS_OFFSET) & USBSTS_SYS_ERROR) != 0;
}

static inline void EhciAckAllInterrupts(EhciContext *Ctx) {
    EhciWriteOp(Ctx, EHC_USBSTS_OFFSET, USBSTS_INTACK_MASK);
}

// ============ Memory Management ============
static void* EhciAllocDmaBuffer(EhciContext *Ctx, UINT32 Size, UINT32 Alignment, UINT64 *PhysAddr) {
    UINT32 Pages = (Size + PAGE_SIZE - 1) / PAGE_SIZE;
    UINT32 AlignPages = (Alignment + PAGE_SIZE - 1) / PAGE_SIZE;
    
    NOPTR *Virt = PhysAllocAllocateAlignedRange(PhysAllocGet(), Pages, AlignPages);
    if (!Virt) return NULLPTR;
    
    MemSet(Virt, 0, Pages * PAGE_SIZE);
    
    if (PhysAddr) {
        *PhysAddr = (UINT64)(UINTPTR)VirtToPhysPtr(Virt);
    }
    
    return Virt;
}

static void EhciFreeDmaBuffer(EhciContext *Ctx, void *Virt, UINT32 Size) {
    UINT32 Pages = (Size + PAGE_SIZE - 1) / PAGE_SIZE;
    PhysAllocFreeRange(PhysAllocGet(), (NOPTR*)Virt, Pages);
}

// ============ QH/QTD Management ============
static EhciQtd* EhciAllocQtd(EhciContext *Ctx) {
    UINT64 Phys;
    EhciQtd *Qtd = (EhciQtd*)EhciAllocDmaBuffer(Ctx, sizeof(EhciQtd), 32, &Phys);
    if (Qtd) {
        MemSet(Qtd, 0, sizeof(EhciQtd));
        Qtd->Signature = 0x51544445;
        ListInit(&Qtd->Link);
    }
    return Qtd;
}

static void EhciFreeQtd(EhciContext *Ctx, EhciQtd *Qtd) {
    if (Qtd) {
        EhciFreeDmaBuffer(Ctx, Qtd, sizeof(EhciQtd));
    }
}

static EhciQh* EhciAllocQh(EhciContext *Ctx) {
    UINT64 Phys;
    EhciQh *Qh = (EhciQh*)EhciAllocDmaBuffer(Ctx, sizeof(EhciQh), 32, &Phys);
    if (Qh) {
        MemSet(Qh, 0, sizeof(EhciQh));
        Qh->Signature = 0x51484545;
        ListInit(&Qh->Qtds);
        Qh->NextQh = NULLPTR;
        Qh->Interval = 1;
    }
    return Qh;
}

static void EhciFreeQh(EhciContext *Ctx, EhciQh *Qh) {
    if (Qh) {
        EhciFreeDmaBuffer(Ctx, Qh, sizeof(EhciQh));
    }
}

static EhciQtd* EhciCreateQtd(EhciContext *Ctx, UINT8 *Data, UINT8 *DataPhy, UINTN DataLen,
                               UINT8 Pid, UINT8 Toggle, UINTN MaxPacket) {
    EhciQtd *Qtd = EhciAllocQtd(Ctx);
    if (!Qtd) return NULLPTR;
    
    Qtd->Data = Data;
    Qtd->DataLen = DataLen;
    
    EhciQtdHw *Hw = &Qtd->Hw;
    Hw->NextQtd = QTD_LINK(NULLPTR, TRUE);
    Hw->AltNext = QTD_LINK(NULLPTR, TRUE);
    Hw->Status = QTD_STAT_ACTIVE;
    Hw->Pid = Pid;
    Hw->ErrCnt = QTD_MAX_ERR;
    Hw->Ioc = 0;
    Hw->DataToggle = Toggle;
    
    if (Data != NULLPTR&& DataLen > 0) {
        UINTN Len = 0;
        UINTN Index;
        
        for (Index = 0; Index < QTD_MAX_BUFFER; Index++) {
            UINT32 PageOff = (UINT32)((UINTPTR)DataPhy & QTD_BUF_MASK);
            UINT32 ThisBufLen = QTD_BUF_LEN - PageOff;
            
            Hw->Page[Index] = EHC_LOW_32BIT(DataPhy);
            Hw->PageHigh[Index] = EHC_HIGH_32BIT(DataPhy);
            
            if (Len + ThisBufLen >= DataLen) {
                Len = DataLen;
                break;
            }
            
            Len += ThisBufLen;
            Data += ThisBufLen;
            DataPhy += ThisBufLen;
        }
        
        if (Len < DataLen) {
            Len = Len - (Len % MaxPacket);
        }
        
        Hw->TotalBytes = (UINT32)Len;
        Qtd->DataLen = Len;
    }
    
    return Qtd;
}

static void EhciFreeQtds(EhciContext *Ctx, ListHead *Qtds) {
    ListHead *Pos, *Tmp;
    ListForEachSafe(Pos, Tmp, Qtds) {
        EhciQtd *Qtd = ListEntry(Pos, EhciQtd, Link);
        ListDel(&Qtd->Link);
        EhciFreeQtd(Ctx, Qtd);
    }
}

static UINTN EhciConvertPollRate(UINTN Interval) {
    UINTN BitCount = 0;
    
    if (Interval == 0) return 1;
    
    while (Interval != 0) {
        Interval >>= 1;
        BitCount++;
    }
    
    return (UINTN)1 << (BitCount - 1);
}

// ============ Transfer Management ============
static INT EhciCreateQtds(EhciContext *Ctx, EhciUrb *Urb) {
    EhciEndpoint *Ep = &Urb->Ep;
    EhciQh *Qh = Urb->Qh;
    EhciQtd *Qtd, *StatusQtd = NULLPTR;
    UINT32 AlterNext = QTD_LINK(NULLPTR, TRUE);
    UINT8 Toggle = 0;
    UINTN Len = 0;
    UINT8 Pid;
    UINT64 Phys;
    
    // Setup short read stop
    Phys = (UINT64)(UINTPTR)VirtToPhysPtr(Ctx->ShortReadStop);
    if (Ep->Direction == USB_DIR_IN) {
        AlterNext = QTD_LINK(Phys, FALSE);
    }
    
    // Control transfer: Setup stage
    if (Urb->Ep.Type == EHC_CTRL_TRANSFER) {
        Qtd = EhciCreateQtd(Ctx, (UINT8*)Urb->Request, (UINT8*)Urb->RequestPhy,
                            sizeof(UsbSetupPacket), QTD_PID_SETUP, 0, Ep->MaxPacket);
        if (!Qtd) goto ON_ERROR;
        ListAddTail(&Qh->Qtds, &Qtd->Link);
        
        // Create status packet
        Pid = (Ep->Direction == USB_DIR_IN) ? QTD_PID_OUTPUT : QTD_PID_INPUT;
        StatusQtd = EhciCreateQtd(Ctx, NULLPTR, NULLPTR, 0, Pid, 1, Ep->MaxPacket);
        if (!StatusQtd) goto ON_ERROR;
        
        if (Ep->Direction == USB_DIR_IN) {
            Phys = (UINT64)(UINTPTR)VirtToPhysPtr(StatusQtd);
            AlterNext = QTD_LINK(Phys, FALSE);
        }
        
        Toggle = 1;
    }
    
    // Data stage
    Pid = (Ep->Direction == USB_DIR_IN) ? QTD_PID_INPUT : QTD_PID_OUTPUT;
    
    while (Len < Urb->DataLen) {
        Qtd = EhciCreateQtd(Ctx, Urb->Data + Len, (UINT8*)Urb->DataPhy + Len,
                            Urb->DataLen - Len, Pid, Toggle, Ep->MaxPacket);
        if (!Qtd) goto ON_ERROR;
        
        Qtd->Hw.AltNext = AlterNext;
        ListAddTail(&Qh->Qtds, &Qtd->Link);
        
        // Toggle for odd number of packets
        if (((Qtd->DataLen + Ep->MaxPacket - 1) / Ep->MaxPacket) % 2) {
            Toggle = (UINT8)(1 - Toggle);
        }
        
        Len += Qtd->DataLen;
    }
    
    // Control transfer: status stage
    if (Ep->Type == EHC_CTRL_TRANSFER) {
        ListAddTail(&Qh->Qtds, &StatusQtd->Link);
    }
    
    // Link QTDs together
    ListHead *Pos;
    EhciQtd *Prev = NULLPTR;
    ListForEach(Pos, &Qh->Qtds) {
        Qtd = ListEntry(Pos, EhciQtd, Link);
        if (Prev) {
            Phys = (UINT64)(UINTPTR)VirtToPhysPtr(Prev);
            Qtd->Hw.NextQtd = QTD_LINK(Phys, FALSE);
        }
        Prev = Qtd;
    }
    
    // Link first QTD to QH
    EhciQtd *First = ListEntry(Qh->Qtds.Next, EhciQtd, Link);
    Phys = (UINT64)(UINTPTR)VirtToPhysPtr(First);
    Qh->Hw.NextQtd = QTD_LINK(Phys, FALSE);
    
    return SUCCESS;
    
ON_ERROR:
    EhciFreeQtds(Ctx, &Qh->Qtds);
    return NO_MEMORY;
}

static EhciUrb* EhciCreateUrb(EhciContext *Ctx, UINT8 DevAddr, UINT8 EpAddr, UINT8 DevSpeed,
                               UINT8 Toggle, UINTN MaxPacket, UINT8 HubAddr, UINT8 HubPort,
                               UINTN Type, UsbSetupPacket *Request, UINT8 *Data, UINTN DataLen,
                               void (*Callback)(UINT8*, UINTN, void*, UINT32), void *Context,
                               UINTN Interval) {
    EhciUrb *Urb = (EhciUrb*)MemoryAllocate(sizeof(EhciUrb));
    if (!Urb) return NULLPTR;
    
    MemSet(Urb, 0, sizeof(EhciUrb));
    Urb->Signature = 0x55454245;
    ListInit(&Urb->Link);
    
    Urb->Ep.DevAddr = DevAddr;
    Urb->Ep.EpAddr = EpAddr & 0x0F;
    Urb->Ep.Direction = (EpAddr & 0x80) ? USB_DIR_IN : USB_DIR_OUT;
    Urb->Ep.DevSpeed = DevSpeed;
    Urb->Ep.MaxPacket = MaxPacket;
    Urb->Ep.HubAddr = HubAddr;
    Urb->Ep.HubPort = HubPort;
    Urb->Ep.Toggle = Toggle;
    Urb->Ep.Type = Type;
    Urb->Ep.PollRate = EhciConvertPollRate(Interval);
    
    Urb->Request = Request;
    Urb->Data = Data;
    Urb->DataLen = DataLen;
    Urb->Callback = Callback;
    Urb->Context = Context;
    Urb->DataPhy = Data ? (void*)(UINTPTR)VirtToPhysPtr(Data) : NULLPTR;
    Urb->RequestPhy = Request ? (void*)(UINTPTR)VirtToPhysPtr(Request) : NULLPTR;
    
    Urb->Qh = EhciAllocQh(Ctx);
    if (!Urb->Qh) {
        MemoryFree(Urb);
        return NULLPTR;
    }
    
    EhciQhHw *Hw = &Urb->Qh->Hw;
    Hw->DeviceAddr = DevAddr;
    Hw->EpNum = Urb->Ep.EpAddr;
    Hw->EpSpeed = DevSpeed;
    Hw->MaxPacketLen = MaxPacket;
    Hw->HubAddr = HubAddr;
    Hw->PortNum = HubPort;
    Hw->DataToggle = Toggle;
    Hw->NakReload = (Type == EHC_INT_TRANSFER_SYNC || Type == EHC_INT_TRANSFER_ASYNC) ? 0 : QH_NAK_RELOAD;
    
    if (DevSpeed != USB_SPEED_HIGH) {
        Hw->Status |= QTD_STAT_DO_SS;
    }
    
    if (Type == EHC_CTRL_TRANSFER) {
        Hw->DtCtrl = 1;
        if (DevSpeed != USB_SPEED_HIGH) {
            Hw->CtrlEp = 1;
        }
    } else if (Type == EHC_INT_TRANSFER_SYNC || Type == EHC_INT_TRANSFER_ASYNC) {
        if (DevSpeed == USB_SPEED_HIGH) {
            Hw->SMask = QH_MICROFRAME_0;
        } else {
            Hw->SMask = QH_MICROFRAME_1;
            Hw->CMask = QH_MICROFRAME_3 | QH_MICROFRAME_4 | QH_MICROFRAME_5;
        }
    } else if (Type == EHC_BULK_TRANSFER && DevSpeed == USB_SPEED_HIGH && Urb->Ep.Direction == USB_DIR_OUT) {
        Hw->Status |= QTD_STAT_DO_PING;
    }
    
    if (EhciCreateQtds(Ctx, Urb) != SUCCESS) {
        EhciFreeQh(Ctx, Urb->Qh);
        MemoryFree(Urb);
        return NULLPTR;
    }
    
    return Urb;
}

static void EhciFreeUrb(EhciContext *Ctx, EhciUrb *Urb) {
    if (!Urb) return;
    
    if (Urb->Qh) {
        EhciFreeQtds(Ctx, &Urb->Qh->Qtds);
        EhciFreeQh(Ctx, Urb->Qh);
    }
    
    MemoryFree(Urb);
}

static BOOL EhciCheckUrbResult(EhciContext *Ctx, EhciUrb *Urb) {
    ListHead *Pos;
    EhciQtd *Qtd;
    EhciQtdHw *Hw;
    UINT8 State;
    BOOL Finished = TRUE;
    
    Urb->Completed = 0;
    Urb->Result = 0;
    
    if (EhciIsHalt(Ctx) || EhciIsSysError(Ctx)) {
        Urb->Result |= 5;  // EFI_USB_ERR_SYSTEM
        goto ON_EXIT;
    }
    
    ListForEach(Pos, &Urb->Qh->Qtds) {
        Qtd = ListEntry(Pos, EhciQtd, Link);
        Hw = &Qtd->Hw;
        State = (UINT8)Hw->Status;
        
        if (State & QTD_STAT_HALTED) {
            if ((State & QTD_STAT_ERR_MASK) == 0) {
                Urb->Result |= 1;  // EFI_USB_ERR_STALL
            }
            if (State & QTD_STAT_BABBLE_ERR) Urb->Result |= 2;  // EFI_USB_ERR_BABBLE
            if (State & QTD_STAT_BUFF_ERR) Urb->Result |= 3;    // EFI_USB_ERR_BUFFER
            if ((State & QTD_STAT_TRANS_ERR) && Hw->ErrCnt == 0) {
                Urb->Result |= 4;  // EFI_USB_ERR_TIMEOUT
            }
            Finished = TRUE;
            goto ON_EXIT;
        } else if (State & QTD_STAT_ACTIVE) {
            Urb->Result |= 0x10;  // EFI_USB_ERR_NOTEXECUTE
            Finished = FALSE;
            goto ON_EXIT;
        } else {
            if (Hw->Pid != QTD_PID_SETUP) {
                Urb->Completed += Qtd->DataLen - Hw->TotalBytes;
            }
            
            // Short packet read
            if (Hw->TotalBytes != 0 && Hw->Pid == QTD_PID_INPUT) {
                UINT64 Phys = (UINT64)(UINTPTR)VirtToPhysPtr(Ctx->ShortReadStop);
                if (Hw->AltNext == QTD_LINK(Phys, FALSE)) {
                    Finished = TRUE;
                    goto ON_EXIT;
                }
            }
        }
    }
    
ON_EXIT:
    Urb->DataToggle = (UINT8)Urb->Qh->Hw.DataToggle;
    return Finished;
}

static INT EhciExecTransfer(EhciContext *Ctx, EhciUrb *Urb, UINT32 TimeoutMs) {
    UINT64 StartTicks = TimerTicks();
    UINT64 TimeoutTicks = (UINT64)TimeoutMs * TimerFreq() / 1000;
    BOOL Finished = FALSE;
    BOOL InfiniteLoop = (TimeoutMs == 0);
    
    if (TimeoutTicks == 0 && !InfiniteLoop) TimeoutTicks = 1;
    
    while (InfiniteLoop || (TimerTicks() - StartTicks) < TimeoutTicks) {
        Finished = EhciCheckUrbResult(Ctx, Urb);
        if (Finished) break;
        TimerUdelay(100);
    }
    
    if (!Finished) return TIMEOUT;
    if (Urb->Result != 0) return DEVICE_ERROR;
    return SUCCESS;
}

// ============ Schedule Management ============
static void EhciLinkQhToAsync(EhciContext *Ctx, EhciQh *Qh) {
    EhciQh *Head = Ctx->ReclaimHead;
    UINT64 Phys;
    
    Qh->NextQh = Head->NextQh;
    Head->NextQh = Qh;
    
    Phys = (UINT64)(UINTPTR)VirtToPhysPtr(Qh->NextQh);
    Qh->Hw.HorizonLink = QH_LINK(Phys, EHC_TYPE_QH, FALSE);
    
    Phys = (UINT64)(UINTPTR)VirtToPhysPtr(Head->NextQh);
    Head->Hw.HorizonLink = QH_LINK(Phys, EHC_TYPE_QH, FALSE);
}

static void EhciUnlinkQhFromAsync(EhciContext *Ctx, EhciQh *Qh) {
    EhciQh *Head = Ctx->ReclaimHead;
    UINT64 Phys;
    
    Head->NextQh = Qh->NextQh;
    Qh->NextQh = NULLPTR;
    
    Phys = (UINT64)(UINTPTR)VirtToPhysPtr(Head->NextQh);
    Head->Hw.HorizonLink = QH_LINK(Phys, EHC_TYPE_QH, FALSE);
    
    // Ring doorbell to synchronize
    EhciSetOpBit(Ctx, EHC_USBCMD_OFFSET, USBCMD_IAAD);
    TimerMdelay(1);
}

static void EhciLinkQhToPeriodic(EhciContext *Ctx, EhciQh *Qh) {
    for (UINTN Index = 0; Index < EHC_FRAME_LEN; Index += Qh->Interval) {
        EhciQh *Next = (EhciQh*)Ctx->PeriodicListHost[Index];
        EhciQh *Prev = NULLPTR;
        UINT64 Phys;
        
        while (Next && Next->Interval > Qh->Interval) {
            Prev = Next;
            Next = Next->NextQh;
        }
        
        if (Next == Qh) continue;
        
        if (Next && Next->Interval == Qh->Interval) {
            Qh->NextQh = Next->NextQh;
            Prev = Next;
            Next = Next->NextQh;
            Prev->NextQh = Qh;
            
            Phys = (UINT64)(UINTPTR)VirtToPhysPtr(Prev->NextQh);
            Prev->Hw.HorizonLink = QH_LINK(Phys, EHC_TYPE_QH, FALSE);
            break;
        }
        
        if (Qh->NextQh == NULLPTR) {
            Qh->NextQh = Next;
            Phys = (UINT64)(UINTPTR)VirtToPhysPtr(Next);
            Qh->Hw.HorizonLink = QH_LINK(Phys, EHC_TYPE_QH, FALSE);
        }
        
        Phys = (UINT64)(UINTPTR)VirtToPhysPtr(Qh);
        if (Prev == NULLPTR) {
            Ctx->PeriodicList[Index] = QH_LINK(Phys, EHC_TYPE_QH, FALSE);
            Ctx->PeriodicListHost[Index] = (UINTN)Qh;
        } else {
            Prev->NextQh = Qh;
            Prev->Hw.HorizonLink = QH_LINK(Phys, EHC_TYPE_QH, FALSE);
        }
    }
}

static void EhciUnlinkQhFromPeriodic(EhciContext *Ctx, EhciQh *Qh) {
    for (UINTN Index = 0; Index < EHC_FRAME_LEN; Index += Qh->Interval) {
        EhciQh *Prev = NULLPTR;
        EhciQh *This = (EhciQh*)Ctx->PeriodicListHost[Index];
        
        while (This && This != Qh) {
            Prev = This;
            This = This->NextQh;
        }
        
        if (!This) continue;
        
        if (Prev == NULLPTR) {
            Ctx->PeriodicList[Index] = Qh->Hw.HorizonLink;
            Ctx->PeriodicListHost[Index] = (UINTN)Qh->NextQh;
        } else {
            Prev->NextQh = Qh->NextQh;
            Prev->Hw.HorizonLink = Qh->Hw.HorizonLink;
        }
    }
}

// ============ Port Management ============
static void EhciPortPowerOn(EhciContext *Ctx, UINT32 Port) {
    if (!(Ctx->HcSParams & HCSP_PPC)) return;
    
    UINT32 Portsc = EhciReadOp(Ctx, EHC_PORT_STAT_OFFSET + Port * 4);
    if (!(Portsc & PORTSC_POWER)) {
        Portsc |= PORTSC_POWER;
        EhciWriteOp(Ctx, EHC_PORT_STAT_OFFSET + Port * 4, Portsc);
        TimerMdelay(20);
    }
}

static void EhciResetPort(EhciContext *Ctx, UINT32 Port) {
    UINT32 Portsc = EhciReadOp(Ctx, EHC_PORT_STAT_OFFSET + Port * 4);
    Portsc |= PORTSC_RESET;
    Portsc &= ~PORTSC_ENABLED;
    EhciWriteOp(Ctx, EHC_PORT_STAT_OFFSET + Port * 4, Portsc);
    TimerMdelay(10);
    
    Portsc &= ~PORTSC_RESET;
    EhciWriteOp(Ctx, EHC_PORT_STAT_OFFSET + Port * 4, Portsc);
    TimerMdelay(20);
}

static UINT8 EhciGetPortSpeed(EhciContext *Ctx, UINT32 Port) {
    UINT32 Portsc = EhciReadOp(Ctx, EHC_PORT_STAT_OFFSET + Port * 4);
    
    if (Portsc & PORTSC_LINESTATE_K) return USB_SPEED_LOW;
    if (Portsc & PORTSC_ENABLED) return USB_SPEED_HIGH;
    return USB_SPEED_FULL;
}

static void EhciScanPorts(EhciContext *Ctx) {
    for (UINT32 I = 0; I < Ctx->Ports; I++) {
        UINT32 Portsc = EhciReadOp(Ctx, EHC_PORT_STAT_OFFSET + I * 4);
        BOOL Connected = (Portsc & PORTSC_CONN) != 0;
        
        // Clear change bits
        if (Portsc & PORTSC_CONN_CHANGE) {
            EhciWriteOp(Ctx, EHC_PORT_STAT_OFFSET + I * 4, Portsc | PORTSC_CONN_CHANGE);
        }
        if (Portsc & PORTSC_ENABLE_CHANGE) {
            EhciWriteOp(Ctx, EHC_PORT_STAT_OFFSET + I * 4, Portsc | PORTSC_ENABLE_CHANGE);
        }
        
        if (Connected && !Ctx->PortsInfo[I].Connected) {
            UINT8 Speed = EhciGetPortSpeed(Ctx, I);
            
            ConsolePrint("[EHCI] Port %d: device connected (speed=%s)\n", I,
                         Speed == USB_SPEED_HIGH ? "High" : (Speed == USB_SPEED_LOW ? "Low" : "Full"));
            
            Ctx->PortsInfo[I].Connected = TRUE;
            Ctx->PortsInfo[I].Speed = Speed;
            
            if (Speed == USB_SPEED_HIGH) {
                EhciResetPort(Ctx, I);
                
                UsbDevice *Dev = UsbDeviceAlloc(Ctx->Hcd);
                if (Dev) {
                    Dev->Address = 0;
                    Dev->Speed = Speed;
                    Dev->Hcd = Ctx->Hcd;
                    
                    if (UsbEnumeration(Dev) == SUCCESS) {
                        UsbDeviceAdd(Ctx->Hcd, Dev);
                        Ctx->PortsInfo[I].Device = Dev;
                    } else {
                        UsbDeviceFree(Dev);
                    }
                }
            }
        } else if (!Connected && Ctx->PortsInfo[I].Connected) {
            ConsolePrint("[EHCI] Port %d: device disconnected\n", I);
            if (Ctx->PortsInfo[I].Device) {
                UsbDeviceRemove(Ctx->PortsInfo[I].Device);
                Ctx->PortsInfo[I].Device = NULLPTR;
            }
            Ctx->PortsInfo[I].Connected = FALSE;
        }
    }
}

// ============ Controller Initialization ============
static INT EhciControllerReset(EhciContext *Ctx) {
    if (!EhciIsHalt(Ctx)) {
        EhciClearOpBit(Ctx, EHC_USBCMD_OFFSET, USBCMD_RUN);
        
        UINT32 Timeout = 10000;
        while (Timeout-- && !EhciIsHalt(Ctx)) {
            TimerUdelay(100);
        }
        if (Timeout == 0) return TIMEOUT;
    }
    
    EhciSetOpBit(Ctx, EHC_USBCMD_OFFSET, USBCMD_RESET);
    TimerMdelay(1);
    
    UINT32 Timeout = 10000;
    while (Timeout-- && (EhciReadOp(Ctx, EHC_USBCMD_OFFSET) & USBCMD_RESET)) {
        TimerUdelay(100);
    }
    
    return (Timeout == 0) ? TIMEOUT : SUCCESS;
}

static INT EhciControllerInit(EhciContext *Ctx) {
    INT Ret;
    UINT64 Phys;
    
    Ret = EhciControllerReset(Ctx);
    if (Ret != SUCCESS) return Ret;
    
    // Allocate periodic frame list
    Ctx->PeriodicList = (UINT32*)EhciAllocDmaBuffer(Ctx, PAGE_SIZE, PAGE_SIZE, &Phys);
    if (!Ctx->PeriodicList) return NO_MEMORY;
    Ctx->PeriodicListPhys = (UINT32)Phys;
    Ctx->PeriodicListHost = (UINTN*)MemoryAllocate(EHC_FRAME_LEN * sizeof(UINTN));
    
    for (INT I = 0; I < EHC_FRAME_LEN; I++) {
        Ctx->PeriodicList[I] = EHCI_LINK_TERMINATE;
        Ctx->PeriodicListHost[I] = 0;
    }
    
    EhciWriteOp(Ctx, EHC_FRAME_BASE_OFFSET, Ctx->PeriodicListPhys);
    EhciWriteOp(Ctx, EHC_CTRLDSSEG_OFFSET, 0);
    
    // Create Reclamation Header
    EhciEndpoint Ep;
    MemSet(&Ep, 0, sizeof(Ep));
    Ep.DevAddr = 0;
    Ep.EpAddr = 1;
    Ep.Direction = USB_DIR_IN;
    Ep.DevSpeed = USB_SPEED_HIGH;
    Ep.MaxPacket = 64;
    Ep.Type = EHC_BULK_TRANSFER;
    Ep.PollRate = 1;
    
    Ctx->ReclaimHead = EhciAllocQh(Ctx);
    if (!Ctx->ReclaimHead) return NO_MEMORY;
    
    Ctx->ReclaimHead->Hw.ReclaimHead = 1;
    Ctx->ReclaimHead->Hw.HorizonLink = QH_LINK((UINT64)(UINTPTR)VirtToPhysPtr(Ctx->ReclaimHead), EHC_TYPE_QH, FALSE);
    Ctx->ReclaimHead->NextQh = Ctx->ReclaimHead;
    Ctx->ReclaimHead->Interval = 1;
    
    Phys = (UINT64)(UINTPTR)VirtToPhysPtr(Ctx->ReclaimHead);
    EhciWriteOp(Ctx, EHC_ASYNC_HEAD_OFFSET, EHC_LOW_32BIT(Phys));
    
    // Create ShortReadStop QTD
    Ctx->ShortReadStop = EhciAllocQtd(Ctx);
    if (!Ctx->ShortReadStop) return NO_MEMORY;
    Ctx->ShortReadStop->Hw.Status = QTD_STAT_HALTED;
    
    // Create dummy periodic QH
    Ep.EpAddr = 2;
    Ep.Type = EHC_INT_TRANSFER_SYNC;
    Ctx->PeriodOne = EhciAllocQh(Ctx);
    if (!Ctx->PeriodOne) return NO_MEMORY;
    Ctx->PeriodOne->Hw.Status = QTD_STAT_HALTED;
    Ctx->PeriodOne->Interval = 1;
    
    // Initialize frame list with PeriodOne
    for (INT I = 0; I < EHC_FRAME_LEN; I++) {
        Phys = (UINT64)(UINTPTR)VirtToPhysPtr(Ctx->PeriodOne);
        Ctx->PeriodicList[I] = QH_LINK(Phys, EHC_TYPE_QH, FALSE);
        Ctx->PeriodicListHost[I] = (UINTN)Ctx->PeriodOne;
    }
    
    // Start controller
    EhciSetOpBit(Ctx, EHC_USBCMD_OFFSET, USBCMD_RUN);
    EhciSetOpBit(Ctx, EHC_USBCMD_OFFSET, USBCMD_ENABLE_PERIOD);
    EhciSetOpBit(Ctx, EHC_USBCMD_OFFSET, USBCMD_ENABLE_ASYNC);
    EhciWriteOp(Ctx, EHC_USBINTR_OFFSET, 0);
    
    // Route all ports to EHCI
    EhciSetOpBit(Ctx, EHC_CONFIG_FLAG_OFFSET, CONFIGFLAG_ROUTE_EHC);
    
    // Power on ports
    for (UINT32 I = 0; I < Ctx->Ports; I++) {
        EhciPortPowerOn(Ctx, I);
    }
    
    TimerMdelay(EHC_ROOT_PORT_RECOVERY_STALL);
    
    Ctx->Running = TRUE;
    ConsolePrint("[EHCI] Controller running, %d ports\n", Ctx->Ports);
    
    return SUCCESS;
}

// ============ USB HCD Callbacks ============
static INT EhciControlTransfer(UsbDevice *Dev, UsbSetupPacket *Setup, UINT8 *Data, UINT32 DataLen) {
    EhciContext *Ctx = GEhci;
    EhciUrb *Urb;
    INT Ret;
    
    if (!Ctx || !Dev || !Setup) return NO_OBJECT;
    
    Urb = EhciCreateUrb(Ctx, Dev->Address, 0, Dev->Speed, 0, Dev->MaxPacketSize0,
                        0, 0, EHC_CTRL_TRANSFER, Setup, Data, DataLen,
                        NULLPTR, NULLPTR, 0);
    if (!Urb) return NO_MEMORY;
    
    EhciLinkQhToAsync(Ctx, Urb->Qh);
    Ret = EhciExecTransfer(Ctx, Urb, 2000);
    EhciUnlinkQhFromAsync(Ctx, Urb->Qh);
    
    EhciFreeUrb(Ctx, Urb);
    return Ret;
}

static INT EhciBulkTransfer(UsbDevice *Dev, UINT8 Endpoint, UINT8 *Data, UINT32 DataLen) {
    EhciContext *Ctx = GEhci;
    EhciUrb *Urb;
    UINT8 HubAddr = 0, HubPort = 0;
    INT Ret;
    
    if (!Ctx || !Dev || !Data || DataLen == 0) return NO_OBJECT;
    
    Urb = EhciCreateUrb(Ctx, Dev->Address, Endpoint, Dev->Speed, 0, 512,
                        HubAddr, HubPort, EHC_BULK_TRANSFER, NULLPTR, Data, DataLen,
                        NULLPTR, NULLPTR, 0);
    if (!Urb) return NO_MEMORY;
    
    EhciLinkQhToAsync(Ctx, Urb->Qh);
    Ret = EhciExecTransfer(Ctx, Urb, 5000);
    EhciUnlinkQhFromAsync(Ctx, Urb->Qh);
    
    EhciFreeUrb(Ctx, Urb);
    return Ret;
}

typedef struct {
    UsbTransfer *Transfer;
    EhciUrb *Urb;
    ListHead Node;
} EhciAsyncInt;

static NOPTR EhciPollAsyncInterrupts(EhciContext *Ctx);

static INT EhciGetEndpointInterval(UsbDevice *Dev, UINT8 EpAddr, UINT8 *Interval) {
    UINT32 I;

    for (I = 0; I < Dev->EndpointCount; I++) {
        UINT8 Addr = Dev->EpDesc[I].BEndpointAddress & 0x0F;
        if (Addr == EpAddr) {
            *Interval = Dev->EpDesc[I].BInterval;
            if (*Interval == 0) {
                *Interval = 1;
            }
            return SUCCESS;
        }
    }
    *Interval = 4;
    return NOT_FOUND;
}

static NOPTR EhciAsyncIntComplete(UINT8 *Data, UINTN Length, NOPTR *Context, UINT32 Result) {
    EhciAsyncInt *Entry = (EhciAsyncInt *)Context;
    UsbTransfer *Transfer;

    (NOPTR)Data;
    (NOPTR)Length;
    if (!Entry || !Entry->Transfer) {
        return;
    }

    Transfer = Entry->Transfer;
    Transfer->Status = (Result == 0) ? SUCCESS : DEVICE_ERROR;
    Transfer->ActualLength = (INT)Length;
    if (Transfer->Callback) {
        Transfer->Callback(Transfer);
    }
}

static INT EhciStartAsyncInterrupt(EhciContext *Ctx, UsbTransfer *Transfer) {
    EhciAsyncInt *Entry;
    UsbDevice *Dev;
    UINT8 Interval;
    UINT8 EpAddr;
    EhciUrb *Urb;
    INT Ret;

    Dev = Transfer->Device;
    if (!Dev) {
        return NO_OBJECT;
    }

    EpAddr = Transfer->Endpoint;
    if (Transfer->Direction == USB_DIR_IN) {
        EpAddr |= 0x80;
    }

    Ret = EhciGetEndpointInterval(Dev, Transfer->Endpoint, &Interval);
    (NOPTR)Ret;

    Entry = (EhciAsyncInt *)MemoryAllocate(sizeof(EhciAsyncInt));
    if (!Entry) {
        return NO_MEMORY;
    }

    Urb = EhciCreateUrb(Ctx, Dev->Address, EpAddr, Dev->Speed, 0, 8,
                        0, 0, EHC_INT_TRANSFER_ASYNC, NULLPTR,
                        Transfer->Buffer, Transfer->Length,
                        EhciAsyncIntComplete, Entry, Interval);
    if (!Urb) {
        MemoryFree(Entry);
        return NO_MEMORY;
    }

    Entry->Transfer = Transfer;
    Entry->Urb = Urb;
    ListInit(&Entry->Node);
    ListAddTail(&Ctx->AsyncIntTransfers, &Entry->Node);

    EhciLinkQhToPeriodic(Ctx, Urb->Qh);
    EhciSetOpBit(Ctx, EHC_USBINTR_OFFSET, EHCI_INTR_USB | EHCI_INTR_ERROR);
    EhciPollAsyncInterrupts(Ctx);
    return SUCCESS;
}

static NOPTR EhciPollAsyncInterrupts(EhciContext *Ctx) {
    ListHead *Pos;
    ListHead *Next;

    ListForEachSafe(Pos, Next, &Ctx->AsyncIntTransfers) {
        EhciAsyncInt *Entry = ListEntry(Pos, EhciAsyncInt, Node);
        if (Entry->Urb && EhciCheckUrbResult(Ctx, Entry->Urb)) {
            EhciAsyncIntComplete(Entry->Urb->Data, Entry->Urb->Completed,
                                 Entry, Entry->Urb->Result);
            Entry->Urb->Qh->Hw.Status &= ~QTD_STAT_HALTED;
        }
    }
}

static INT EhciHcdSubmitTransfer(UsbHcd *Hcd, UsbTransfer *Transfer) {
    EhciContext *Ctx = (EhciContext *)Hcd->Private;
    UsbSetupPacket *Setup;
    INT Ret;

    if (!Ctx || !Transfer || !Transfer->Device) {
        return NO_OBJECT;
    }

    switch (Transfer->Type) {
    case USB_TRANSFER_CONTROL:
        Setup = (UsbSetupPacket *)Transfer->Context;
        if (!Setup) {
            return INCORRECT_VALUE;
        }
        Ret = EhciControlTransfer(Transfer->Device, Setup,
                                  Transfer->Buffer, Transfer->Length);
        Transfer->Status = Ret;
        return Ret;

    case USB_TRANSFER_BULK:
        Ret = EhciBulkTransfer(Transfer->Device,
                               (UINT8)((Transfer->Direction == USB_DIR_IN) ?
                                       (Transfer->Endpoint | 0x80) : Transfer->Endpoint),
                               Transfer->Buffer, Transfer->Length);
        Transfer->Status = Ret;
        return Ret;

    case USB_TRANSFER_INTERRUPT:
        if (Transfer->Callback) {
            Ret = EhciStartAsyncInterrupt(Ctx, Transfer);
            if (Ret == SUCCESS) {
                Transfer->Status = 0;
            }
            return Ret;
        }
        Ret = EhciBulkTransfer(Transfer->Device,
                               (UINT8)(Transfer->Endpoint | 0x80),
                               Transfer->Buffer, Transfer->Length);
        Transfer->Status = Ret;
        return Ret;

    default:
        Transfer->Status = NOT_IMPLEMENTED;
        return NOT_IMPLEMENTED;
    }
}

static UsbHcdOps GEhciHcdOps = {
    .SubmitTransfer = EhciHcdSubmitTransfer,
    .IrqHandler = (NOPTR (*)(NOPTR))EhciIrqHandler,
};

// ============ IRQ Handler ============
NOPTR EhciIrqHandler(NOPTR) {
    EhciContext *Ctx = GEhci;
    UINT32 Status;
    
    if (!Ctx || !Ctx->Running) {
        ApicEoi();
        return;
    }
    
    Status = EhciReadOp(Ctx, EHC_USBSTS_OFFSET);
    EhciWriteOp(Ctx, EHC_USBSTS_OFFSET, Status);
    
    if (Status & EHCI_INTR_PORT_CHANGE) {
        EhciScanPorts(Ctx);
    }

    if (Status & (EHCI_INTR_USB | EHCI_INTR_ERROR)) {
        EhciPollAsyncInterrupts(Ctx);
    }
    
    ApicEoi();
    if (Ctx->Gsi) {
        IoapicEoi(Ctx->Gsi);
    }
}

// ============ IRQ Setup ============
static INT EhciSetupIrq(EhciContext *Ctx, PciDevice *PciDev) {
    UINT32 Flags;
    
    Ctx->Irq = PciDev->InterruptLine;
    
    if (IoapicGetOverride(Ctx->Irq, &Ctx->Gsi, &Flags) != SUCCESS) {
        Ctx->Gsi = Ctx->Irq;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }
    
    Ctx->Vector = 48 + (Ctx->Gsi % 16);
    
    if (IoapicRedirectIrq(Ctx->Gsi, Ctx->Vector, ApicGetId(), Flags) != SUCCESS) {
        ConsolePrint("[EHCI] Failed to redirect IRQ %d\n", Ctx->Irq);
        return IO_ERROR;
    }
    
    IdtSetGate(Ctx->Vector, EhciIrqHandler, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IoapicUnmaskIrq(Ctx->Gsi);
    
    ConsolePrint("[EHCI] IRQ %d (GSI %d) -> vector %d\n", Ctx->Irq, Ctx->Gsi, Ctx->Vector);
    
    return SUCCESS;
}

// ============ PCI Detection ============
INT EhciInit(PciDevice *PciDev) {
    EhciContext *Ctx;
    UsbHcd *Hcd;
    UINT32 MmioBase;
    UINT8 CapLength;
    INT Ret;
    
    if (!PciDev) return NO_OBJECT;
    
    ConsolePrint("[EHCI] Found controller at %02X:%02X.%X (%04X:%04X)\n",
                 PciDev->Bus, PciDev->Slot, PciDev->Function,
                 PciDev->VendorId, PciDev->DeviceId);
    
    PciEnableBusmaster(PciDev);
    PciEnable(PciDev);
    
    MmioBase = PciDev->Bars[0] & ~0xF;
    if (!MmioBase) {
        ConsolePrint("[EHCI] No MMIO BAR\n");
        return NO_OBJECT;
    }
    
    Ctx = (EhciContext*)MemoryAllocate(sizeof(EhciContext));
    if (!Ctx) return NO_MEMORY;
    MemSet(Ctx, 0, sizeof(EhciContext));
    
    Ctx->CapRegs = (volatile UINT8*)(UINTPTR)MmioBase;
    CapLength = MmioRead8(Ctx->CapRegs);
    Ctx->OpRegs = (volatile UINT8*)((UINTPTR)Ctx->CapRegs + CapLength);
    Ctx->CapLength = CapLength;
    Ctx->HcSParams = EhciReadCap(Ctx, EHC_HCSPARAMS_OFFSET);
    Ctx->HcCParams = EhciReadCap(Ctx, EHC_HCCPARAMS_OFFSET);
    Ctx->Ports = Ctx->HcSParams & HCSP_NPORTS;
    
    if (Ctx->HcCParams & HCCP_64BIT) {
        Ctx->Support64BitDma = TRUE;
        ConsolePrint("[EHCI] 64-bit DMA supported\n");
    }
    
    ListInit(&Ctx->AsyncIntTransfers);
    for (UINT32 I = 0; I < EHCI_MAX_PORTS; I++) {
        Ctx->PortsInfo[I].Connected = FALSE;
    }
    
    Hcd = (UsbHcd*)MemoryAllocate(sizeof(UsbHcd));
    if (!Hcd) {
        MemoryFree(Ctx);
        return NO_MEMORY;
    }
    
    MemSet(Hcd, 0, sizeof(UsbHcd));
    SnPrintf(Hcd->Name, sizeof(Hcd->Name), "ehci");
    Hcd->VendorId = PciDev->VendorId;
    Hcd->DeviceId = PciDev->DeviceId;
    Hcd->MmioBase = MmioBase;
    Hcd->Irq = PciDev->InterruptLine;
    Hcd->Ops = &GEhciHcdOps;
    Hcd->Private = Ctx;
    
    Ctx->Hcd = Hcd;
    Ctx->PciDev = PciDev;
    GEhci = Ctx;
    
    Ret = EhciControllerInit(Ctx);
    if (Ret != SUCCESS) {
        ConsolePrint("[EHCI] Controller init failed: %d\n", Ret);
        MemoryFree(Hcd);
        MemoryFree(Ctx);
        return Ret;
    }
    
    EhciSetupIrq(Ctx, PciDev);
    
    if (UsbHcdRegister(Hcd) != SUCCESS) {
        ConsolePrint("[EHCI] Failed to register HCD\n");
        MemoryFree(Hcd);
        MemoryFree(Ctx);
        return DEVICE_ERROR;
    }
    
    EhciScanPorts(Ctx);
    
    KDriverRegister(KDriverGenerateStruct("Ehci", DCL1, TRUE, NULLPTR, NULLPTR));
    
    return SUCCESS;
}

NOPTR EhciProbeAll(NOPTR) {
    PciDevice *Dev = PciGetFirst();
    
    while (Dev) {
        if (Dev->ClassCode == EHCI_CLASS_CODE &&
            Dev->SubClass == EHCI_SUBCLASS &&
            Dev->ProgIf == EHCI_PROG_IF) {
            EhciInit(Dev);
        }
        Dev = PciGetNext(Dev);
    }
}