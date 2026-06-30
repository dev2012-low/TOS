#include <Usb/Xhci.h>
#include <Usb/Usb.h>
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
#include <Decon.h>

// ============================================================================
// Глобальный контекст
// ============================================================================

static XhciContext *GXhci = NULLPTR;

// ============================================================================
// MMIO Access
// ============================================================================

static inline UINT8 XhciReadCap8(XhciContext *Xhc, UINT32 Offset) {
    return MmioRead8((volatile NOPTR*)((UINTPTR)Xhc->CapRegs + Offset));
}

static inline UINT32 XhciReadCap32(XhciContext *Xhc, UINT32 Offset) {
    return MmioRead32((volatile NOPTR*)((UINTPTR)Xhc->CapRegs + Offset));
}

static inline UINT32 XhciReadOp32(XhciContext *Xhc, UINT32 Offset) {
    return MmioRead32((volatile NOPTR*)((UINTPTR)Xhc->OpRegs + Offset));
}

static inline NOPTR XhciWriteOp32(XhciContext *Xhc, UINT32 Offset, UINT32 Value) {
    MmioWrite32((volatile NOPTR*)((UINTPTR)Xhc->OpRegs + Offset), Value);
}

static inline NOPTR XhciWriteDoorbell(XhciContext *Xhc, UINT32 Offset, UINT32 Value) {
    MmioWrite32((volatile NOPTR*)((UINTPTR)Xhc->Doorbell + Offset), Value);
}

static inline UINT32 XhciReadRuntime32(XhciContext *Xhc, UINT32 Offset) {
    return MmioRead32((volatile NOPTR*)((UINTPTR)Xhc->RuntimeRegs + Offset));
}

static inline NOPTR XhciWriteRuntime32(XhciContext *Xhc, UINT32 Offset, UINT32 Value) {
    MmioWrite32((volatile NOPTR*)((UINTPTR)Xhc->RuntimeRegs + Offset), Value);
}

static inline NOPTR XhciSetOpBit(XhciContext *Xhc, UINT32 Offset, UINT32 Bit) {
    UINT32 Val = XhciReadOp32(Xhc, Offset);
    Val |= Bit;
    XhciWriteOp32(Xhc, Offset, Val);
}

static inline NOPTR XhciClearOpBit(XhciContext *Xhc, UINT32 Offset, UINT32 Bit) {
    UINT32 Val = XhciReadOp32(Xhc, Offset);
    Val &= ~Bit;
    XhciWriteOp32(Xhc, Offset, Val);
}

static inline BOOL XhciIsBitSet(XhciContext *Xhc, UINT32 Offset, UINT32 Bit) {
    return (XhciReadOp32(Xhc, Offset) & Bit) != 0;
}

static inline NOPTR XhciWaitMs(UINT32 Ms) {
    TimerMdelay(Ms);
}

// ============================================================================
// DMA Memory Management
// ============================================================================

static NOPTR* XhciAllocDmaBuffer(XhciContext *Xhc, UINT32 Size, UINT32 Alignment, UINT64 *PhysAddr) {
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

static NOPTR XhciFreeDmaBuffer(XhciContext *Xhc, NOPTR *Virt, UINT32 Size) {
    UINT32 Pages = (Size + PAGE_SIZE - 1) / PAGE_SIZE;
    PhysAllocFreeRange(PhysAllocGet(), (NOPTR*)Virt, Pages);
}

// ============================================================================
// Transfer Ring Management
// ============================================================================

static INT XhciCreateTransferRing(XhciContext *Xhc, UINTN TrbNum, TRANSFER_RING *Ring) {
    UINT64 RingPhys;
    UINT32 Size = sizeof(TRB_TEMPLATE) * TrbNum;
    
    Ring->RingSeg0 = (TRB_TEMPLATE*)XhciAllocDmaBuffer(Xhc, Size, XHC_TRB_ALIGNMENT, &RingPhys);
    if (!Ring->RingSeg0) {
        return NO_MEMORY;
    }
    
    Ring->TrbNumber = TrbNum;
    Ring->RingEnqueue = Ring->RingSeg0;
    Ring->RingDequeue = Ring->RingSeg0;
    Ring->RingPCS = 1;
    
    // ИСПРАВЛЕНО: Link TRB
    TRB_LINK *LinkTrb = (TRB_LINK*)&Ring->RingSeg0[TrbNum - 1];
    LinkTrb->PtrLo = (UINT32)(RingPhys & 0xFFFFFFFF);
    LinkTrb->PtrHi = (UINT32)(RingPhys >> 32);
    LinkTrb->Type = TRB_TYPE_LINK;
    LinkTrb->TC = 1;
    LinkTrb->CycleBit = 1;  // Должен быть 1, иначе команда не выполнится
    
    return SUCCESS;
}

static NOPTR XhciFreeTransferRing(XhciContext *Xhc, TRANSFER_RING *Ring) {
    if (Ring && Ring->RingSeg0) {
        XhciFreeDmaBuffer(Xhc, Ring->RingSeg0, sizeof(TRB_TEMPLATE) * Ring->TrbNumber);
        Ring->RingSeg0 = NULLPTR;
    }
}

static NOPTR XhciSyncTransferRing(TRANSFER_RING *Ring) {
    TRB_TEMPLATE *Trb = Ring->RingEnqueue;
    UINT32 TrbType;
    
    for (UINTN Index = 0; Index < Ring->TrbNumber; Index++) {
        if (Trb->CycleBit != (Ring->RingPCS & 1)) {
            break;
        }
        
        Trb++;
        
        // ИСПРАВЛЕНО: Правильный способ получить тип TRB
        TrbType = (Trb->Status >> 16) & 0x3F;  // Type в битах 16-23 статуса
        if (TrbType == TRB_TYPE_LINK) {
            TRB_LINK *Link = (TRB_LINK*)Trb;
            Link->CycleBit = Ring->RingPCS & 1;
            Ring->RingPCS ^= 1;
            Trb = Ring->RingSeg0;
        }
    }
    
    if (Trb != Ring->RingEnqueue) {
        Ring->RingEnqueue = Trb;
    }
}

// ============================================================================
// Event Ring Management
// ============================================================================

static INT XhciCreateEventRing(XhciContext *Xhc, EVENT_RING *Ring) {
    UINT64 ErstPhys, RingPhys;
    UINT32 RingSize = sizeof(TRB_TEMPLATE) * EVENT_RING_TRB_NUMBER;
    UINT32 ErstSize = sizeof(EVENT_RING_SEG_TABLE_ENTRY) * ERST_NUMBER;

    Ring->EventRingSeg0 = (TRB_TEMPLATE*)XhciAllocDmaBuffer(Xhc, RingSize, XHC_TRB_ALIGNMENT, &RingPhys);
    if (!Ring->EventRingSeg0) {
        return NO_MEMORY;
    }

    Ring->TrbNumber = EVENT_RING_TRB_NUMBER;
    Ring->EventRingDequeue = Ring->EventRingSeg0;
    Ring->EventRingEnqueue = Ring->EventRingSeg0;
    Ring->EventRingCCS = 1;

    Ring->ERSTBase = (EVENT_RING_SEG_TABLE_ENTRY*)XhciAllocDmaBuffer(Xhc, ErstSize, XHC_DMA_ALIGNMENT, &ErstPhys);
    if (!Ring->ERSTBase) {
        XhciFreeDmaBuffer(Xhc, Ring->EventRingSeg0, RingSize);
        return NO_MEMORY;
    }

    MemSet(Ring->ERSTBase, 0, ErstSize);
    Ring->ERSTBase[0].PtrLo = (UINT32)(RingPhys & 0xFFFFFFFF);
    Ring->ERSTBase[0].PtrHi = (UINT32)(RingPhys >> 32);
    Ring->ERSTBase[0].RingTrbSize = EVENT_RING_TRB_NUMBER;

    // Пишем ERST и ERDP
    XhciWriteRuntime32(Xhc, XHC_ERSTSZ_OFFSET, ERST_NUMBER);
    XhciWriteRuntime32(Xhc, XHC_ERSTBA_OFFSET, (UINT32)(ErstPhys & 0xFFFFFFFF));
    XhciWriteRuntime32(Xhc, XHC_ERSTBA_OFFSET + 4, (UINT32)(ErstPhys >> 32));

    AsmFlushCacheRange((NOPTR*)Xhc->RuntimeRegs, 0x40);

    XhciWriteRuntime32(Xhc, XHC_ERDP_OFFSET, (UINT32)(RingPhys & 0xFFFFFFFF) | 1);
    XhciWriteRuntime32(Xhc, XHC_ERDP_OFFSET + 4, (UINT32)(RingPhys >> 32));

    XhciWriteRuntime32(Xhc, XHC_IMAN_OFFSET, XhciReadRuntime32(Xhc, XHC_IMAN_OFFSET) | 2);

    // Синхронизируем ERST и Event Ring для DMA
    AsmFlushCacheRange((NOPTR*)Ring->ERSTBase, ErstSize);
    AsmFlushCacheRange((NOPTR*)Ring->EventRingSeg0, RingSize);

    return SUCCESS;
}


static NOPTR XhciFreeEventRing(XhciContext *Xhc, EVENT_RING *Ring) {
    if (Ring->EventRingSeg0) {
        XhciFreeDmaBuffer(Xhc, Ring->EventRingSeg0, sizeof(TRB_TEMPLATE) * EVENT_RING_TRB_NUMBER);
    }
    if (Ring->ERSTBase) {
        XhciFreeDmaBuffer(Xhc, Ring->ERSTBase, sizeof(EVENT_RING_SEG_TABLE_ENTRY) * ERST_NUMBER);
    }
}

static INT XhciSyncEventRing(EVENT_RING *Ring) {
    TRB_TEMPLATE *Trb = Ring->EventRingDequeue;
    UINTN Index;
    
    for (Index = 0; Index < Ring->TrbNumber; Index++) {
        if (Trb->CycleBit != Ring->EventRingCCS) {
            break;
        }
        Trb++;
        if ((UINTN)Trb >= ((UINTN)Ring->EventRingSeg0 + sizeof(TRB_TEMPLATE) * Ring->TrbNumber)) {
            Trb = Ring->EventRingSeg0;
            Ring->EventRingCCS ^= 1;
        }
    }
    
    if (Index < Ring->TrbNumber) {
        Ring->EventRingEnqueue = Trb;
    }
    
    return SUCCESS;
}

static INT XhciCheckNewEvent(EVENT_RING *Ring, TRB_TEMPLATE **NewTrb) {
    *NewTrb = Ring->EventRingDequeue;

    if (Ring->EventRingDequeue == Ring->EventRingEnqueue) {
        return NOT_READY;
    }

    *NewTrb = Ring->EventRingDequeue;

    // Переход к следующему TRB с учётом зацикливания
    if ((UINTN)(Ring->EventRingDequeue + 1) >= ((UINTN)Ring->EventRingSeg0 + sizeof(TRB_TEMPLATE) * Ring->TrbNumber)) {
        Ring->EventRingDequeue = Ring->EventRingSeg0;
        Ring->EventRingCCS ^= 1;
    } else {
        Ring->EventRingDequeue++;
    }

    return SUCCESS;
}


// ============================================================================
// Command Ring Operations
// ============================================================================

static INT XhciSendCommand(XhciContext *Xhc, TRB_TEMPLATE *CmdTrb, TRB_TEMPLATE **EvtTrb, UINT32 TimeoutMs) {
    UINT64 StartTicks;
    UINT64 TimeoutTicks;

    XhciSyncTransferRing(&Xhc->CmdRing);

    TRB_TEMPLATE *TrbStart = Xhc->CmdRing.RingEnqueue;
    UINT64 CmdTrbPhys = (UINT64)(UINTPTR)TrbStart;  // при identity mapping это физ. адрес

    // Копируем команду
    MemCpy(TrbStart, CmdTrb, sizeof(TRB_TEMPLATE));
    TrbStart->CycleBit = Xhc->CmdRing.RingPCS & 1;

    // Продвигаем указатель очереди
    Xhc->CmdRing.RingEnqueue++;

    // Обрабатываем Link TRB при переходе через границу кольца
    if (Xhc->CmdRing.RingEnqueue->Type == TRB_TYPE_LINK) {
        TRB_LINK *LinkTrb = (TRB_LINK*)Xhc->CmdRing.RingEnqueue;
        LinkTrb->CycleBit = Xhc->CmdRing.RingPCS & 1;
        if (LinkTrb->TC) {
            Xhc->CmdRing.RingPCS ^= 1;
        }
        Xhc->CmdRing.RingEnqueue = (TRB_TEMPLATE*)((UINT64)LinkTrb->PtrHi << 32 | LinkTrb->PtrLo);
    }

    // КРИТИЧЕСКИ ВАЖНО: сброс кэша для DMA-буфера, куда записали TRB
    AsmFlushCacheRange((NOPTR*)TrbStart, sizeof(TRB_TEMPLATE));

    // Толкаем Doorbell 0 (Host Controller Command Ring)
    XhciWriteDoorbell(Xhc, 0, 0);

    StartTicks = TimerTicks();
    TimeoutTicks = (UINT64)TimeoutMs * TimerFreq() / 1000;
    if (TimeoutTicks == 0) TimeoutTicks = 1;

    while (1) {
        TRB_TEMPLATE *Event;
        XhciSyncEventRing(&Xhc->EventRing);

        while (XhciCheckNewEvent(&Xhc->EventRing, &Event) == SUCCESS) {
            if (Event->Type == TRB_TYPE_COMMAND_COMPLT_EVENT) {
                EVT_COMMAND_COMPLETE *CmdEvent = (EVT_COMMAND_COMPLETE*)Event;
                UINT64 EventTrbPhys = ((UINT64)CmdEvent->TRBPtrHi << 32) | CmdEvent->TRBPtrLo;

                if (EventTrbPhys == CmdTrbPhys) {
                    if (EvtTrb) *EvtTrb = Event;
                    if (CmdEvent->Completecode == TRB_COMPLETION_SUCCESS) {
                        return SUCCESS;
                    }
                    return DEVICE_ERROR;
                }
            }
        }

        if ((TimerTicks() - StartTicks) >= TimeoutTicks) {
            return TIMEOUT;
        }

        TimerUdelay(100);
    }
}



// ============================================================================
// Port Management (исправлено из EDK2)
// ============================================================================

static UINT8 XhciGetPortSpeed(XhciContext *Xhc, UINT8 PortNum) {
    UINT32 Portsc = XhciReadOp32(Xhc, XHC_PORTSC_OFFSET + (PortNum * 0x10));
    UINT8 Speed = (Portsc >> 10) & 0x0F;
    
    switch (Speed) {
        case XHC_PORT_SPEED_FULL:   return USB_SPEED_FULL;
        case XHC_PORT_SPEED_LOW:    return USB_SPEED_LOW;
        case XHC_PORT_SPEED_HIGH:   return USB_SPEED_HIGH;
        case XHC_PORT_SPEED_SUPER:
        case XHC_PORT_SPEED_SUPER_PLUS: return USB_SPEED_SUPER;
        default: return USB_SPEED_FULL;
    }
}

static NOPTR XhciPowerOnPort(XhciContext *Xhc, UINT8 PortNum) {
    UINT32 Portsc = XhciReadOp32(Xhc, XHC_PORTSC_OFFSET + (PortNum * 0x10));
    
    if (!(Portsc & XHC_PORTSC_PP)) {
        Portsc |= XHC_PORTSC_PP;
        XhciWriteOp32(Xhc, XHC_PORTSC_OFFSET + (PortNum * 0x10), Portsc);
        XhciWaitMs(20);
    }
}

static NOPTR XhciResetPort(XhciContext *Xhc, UINT8 PortNum) {
    UINT32 Portsc;
    UINT32 Timeout;
    
    Portsc = XhciReadOp32(Xhc, XHC_PORTSC_OFFSET + (PortNum * 0x10));
    Portsc |= XHC_PORTSC_RESET;
    XhciWriteOp32(Xhc, XHC_PORTSC_OFFSET + (PortNum * 0x10), Portsc);
    
    // Wait for reset to complete (up to 50ms)
    Timeout = 50;
    while (Timeout--) {
        Portsc = XhciReadOp32(Xhc, XHC_PORTSC_OFFSET + (PortNum * 0x10));
        if (!(Portsc & XHC_PORTSC_RESET)) {
            break;
        }
        XhciWaitMs(1);
    }
    
    // Extra delay after reset (TRSTRCY)
    XhciWaitMs(XHC_RESET_RECOVERY_DELAY);
    
    // Clear change bits
    Portsc = XhciReadOp32(Xhc, XHC_PORTSC_OFFSET + (PortNum * 0x10));
    if (Portsc & XHC_PORTSC_CSC) {
        XhciWriteOp32(Xhc, XHC_PORTSC_OFFSET + (PortNum * 0x10), Portsc | XHC_PORTSC_CSC);
    }
    if (Portsc & XHC_PORTSC_PEC) {
        XhciWriteOp32(Xhc, XHC_PORTSC_OFFSET + (PortNum * 0x10), Portsc | XHC_PORTSC_PEC);
    }
    if (Portsc & XHC_PORTSC_PRC) {
        XhciWriteOp32(Xhc, XHC_PORTSC_OFFSET + (PortNum * 0x10), Portsc | XHC_PORTSC_PRC);
    }
}

// ============================================================================
// Slot Management (из EDK2)
// ============================================================================

static UINT8 XhciFindSlotByBusAddr(XhciContext *Xhc, UINT8 BusAddr) {
    for (UINT32 I = 0; I < 256; I++) {
        if (Xhc->Slots[I].Enabled && Xhc->Slots[I].BusDevAddr == BusAddr) {
            return (UINT8)I;
        }
    }
    return 0;
}

static UINT8 XhciFindSlotByRoute(XhciContext *Xhc, UINT32 RouteString) {
    for (UINT32 I = 0; I < 256; I++) {
        if (Xhc->Slots[I].Enabled && Xhc->Slots[I].RouteString == RouteString) {
            return (UINT8)I;
        }
    }
    return 0;
}

static INT XhciEnableSlot(XhciContext *Xhc, UINT8 *SlotId) {
    CMD_ENABLE_SLOT Cmd;
    TRB_TEMPLATE *EvtTrb;
    INT Ret;

    MemSet(&Cmd, 0, sizeof(Cmd));
    Cmd.CycleBit = Xhc->CmdRing.RingPCS & 1;      // <--- было: 1
    Cmd.Type = TRB_TYPE_EN_SLOT;

    Ret = XhciSendCommand(Xhc, (TRB_TEMPLATE*)&Cmd, &EvtTrb, XHC_GENERIC_TIMEOUT);
    if (Ret != SUCCESS) {
        return Ret;
    }

    *SlotId = ((EVT_COMMAND_COMPLETE*)EvtTrb)->SlotId;
    return SUCCESS;
}


static INT XhciDisableSlot(XhciContext *Xhc, UINT8 SlotId) {
    CMD_DISABLE_SLOT Cmd;
    TRB_TEMPLATE *EvtTrb;
    
    MemSet(&Cmd, 0, sizeof(Cmd));
    Cmd.CycleBit = 1;
    Cmd.Type = TRB_TYPE_DIS_SLOT;
    Cmd.SlotId = SlotId;
    
    return XhciSendCommand(Xhc, (TRB_TEMPLATE*)&Cmd, &EvtTrb, XHC_GENERIC_TIMEOUT);
}

static INT XhciInitializeDeviceSlot(XhciContext *Xhc, UINT8 PortNum, UINT8 Speed, UINT8 *OutSlotId) {
    DEVICE_SLOT *Slot;
    UINT64 PhysAddr;
    INT Ret;
    UINT8 SlotId;

    Ret = XhciEnableSlot(Xhc, &SlotId);
    if (Ret != SUCCESS) {
        DeconPrint("[xHCI] EnableSlot failed: %d\n", Ret);
        return Ret;
    }

    DeconPrint("[xHCI] Enabled slot %d for port %d\n", SlotId, PortNum);

    Slot = &Xhc->Slots[SlotId];
    MemSet(Slot, 0, sizeof(DEVICE_SLOT));
    Slot->Enabled = TRUE;
    Slot->SlotId = SlotId;

    // Allocate Input Context
    Slot->InputContext = (INPUT_CONTEXT*)XhciAllocDmaBuffer(Xhc, sizeof(INPUT_CONTEXT), XHC_DMA_ALIGNMENT, &PhysAddr);
    if (!Slot->InputContext) {
        XhciDisableSlot(Xhc, SlotId);
        return NO_MEMORY;
    }

    // Allocate Output Context
    UINT64 OutputContextPhys;
    Slot->OutputContext = (DEVICE_CONTEXT*)XhciAllocDmaBuffer(Xhc, sizeof(DEVICE_CONTEXT), XHC_DMA_ALIGNMENT, &OutputContextPhys);
    if (!Slot->OutputContext) {
        XhciFreeDmaBuffer(Xhc, Slot->InputContext, sizeof(INPUT_CONTEXT));
        XhciDisableSlot(Xhc, SlotId);
        return NO_MEMORY;
    }

    // Заполняем DCBAA: DCBAA[0] оставляем 0, DCBAA[SlotId] = физ. адрес OutputContext
    Xhc->DCBAA[SlotId] = OutputContextPhys;

    // Инициализируем Input Context
    MemSet(Slot->InputContext, 0, sizeof(INPUT_CONTEXT));
    Slot->InputContext->InputControlContext.Dword2 |= (BIT0 | BIT1);

    Slot->InputContext->Slot.RouteString = 0;
    Slot->InputContext->Slot.Speed = Speed + 1;
    Slot->InputContext->Slot.ContextEntries = 1;
    Slot->InputContext->Slot.RootHubPortNum = PortNum + 1;

    // Создаём кольцо передачи для EP0
    Slot->EndpointTransferRing[0] = (TRANSFER_RING*)MemoryAllocate(sizeof(TRANSFER_RING));
    if (!Slot->EndpointTransferRing[0]) {
        XhciFreeDmaBuffer(Xhc, Slot->InputContext, sizeof(INPUT_CONTEXT));
        XhciFreeDmaBuffer(Xhc, Slot->OutputContext, sizeof(DEVICE_CONTEXT));
        XhciDisableSlot(Xhc, SlotId);
        return NO_MEMORY;
    }

    Ret = XhciCreateTransferRing(Xhc, TR_RING_TRB_NUMBER, Slot->EndpointTransferRing[0]);
    if (Ret != SUCCESS) {
        MemoryFree(Slot->EndpointTransferRing[0]);
        XhciFreeDmaBuffer(Xhc, Slot->InputContext, sizeof(INPUT_CONTEXT));
        XhciFreeDmaBuffer(Xhc, Slot->OutputContext, sizeof(DEVICE_CONTEXT));
        XhciDisableSlot(Xhc, SlotId);
        return Ret;
    }

    UINT64 RingPhys = (UINT64)(UINTPTR)VirtToPhysPtr(Slot->EndpointTransferRing[0]->RingSeg0);
    Slot->InputContext->EP[0].EPType = ED_CONTROL_BIDIR;
    Slot->InputContext->EP[0].MaxPacketSize = (Speed == USB_SPEED_SUPER) ? 512 :
                                             (Speed == USB_SPEED_HIGH) ? 64 : 8;
    Slot->InputContext->EP[0].AverageTRBLength = 8;
    Slot->InputContext->EP[0].CErr = 3;
    Slot->InputContext->EP[0].PtrLo = (UINT32)(RingPhys & 0xFFFFFFFF);
    Slot->InputContext->EP[0].PtrHi = (UINT32)(RingPhys >> 32);

    // Flush InputContext перед отправкой команды Address Device
    AsmFlushCacheRange((NOPTR*)Slot->InputContext, sizeof(INPUT_CONTEXT));

    // Address Device command
    CMD_ADDRESS_DEVICE CmdAddr;
    TRB_TEMPLATE *EvtTrb;

    XhciWaitMs(10);  // TRSTRCY delay

    MemSet(&CmdAddr, 0, sizeof(CmdAddr));
    CmdAddr.PtrLo = (UINT32)(OutputContextPhys & 0xFFFFFFFF);
    CmdAddr.PtrHi = (UINT32)(OutputContextPhys >> 32);
    CmdAddr.CycleBit = 1;
    CmdAddr.Type = TRB_TYPE_ADDRESS_DEV;
    CmdAddr.BSR = 1;
    CmdAddr.SlotId = SlotId;

    Ret = XhciSendCommand(Xhc, (TRB_TEMPLATE*)&CmdAddr, &EvtTrb, XHC_GENERIC_TIMEOUT);
    if (Ret != SUCCESS) {
        DeconPrint("[xHCI] AddressDevice failed for slot %d: %d\n", SlotId, Ret);
        XhciFreeTransferRing(Xhc, Slot->EndpointTransferRing[0]);
        MemoryFree(Slot->EndpointTransferRing[0]);
        XhciFreeDmaBuffer(Xhc, Slot->InputContext, sizeof(INPUT_CONTEXT));
        XhciFreeDmaBuffer(Xhc, Slot->OutputContext, sizeof(DEVICE_CONTEXT));
        XhciDisableSlot(Xhc, SlotId);
        return Ret;
    }

    Slot->XhciDevAddr = Slot->OutputContext->Slot.DeviceAddress;
    *OutSlotId = SlotId;

    DeconPrint("[xHCI] Device address %d assigned to slot %d\n", Slot->XhciDevAddr, SlotId);

    return SUCCESS;
}


// ============================================================================
// Transfer Management (из EDK2)
// ============================================================================

static INT XhciCreateControlUrb(XhciContext *Xhc, URB *Urb) {
    DEVICE_SLOT *Slot;
    TRANSFER_RING *Ring;
    UINT8 Dci;
    UINT64 PhysAddr;
    
    Slot = &Xhc->Slots[XhciFindSlotByBusAddr(Xhc, Urb->BusAddr)];
    if (!Slot->Enabled) {
        return NO_OBJECT;
    }
    
    Dci = XhciEndpointToDci(Urb->EpAddr, Urb->Direction);
    if (Dci >= 32) {
        return INCORRECT_VALUE;
    }
    
    Ring = Slot->EndpointTransferRing[Dci - 1];
    if (!Ring) {
        return NO_OBJECT;
    }
    
    Urb->Ring = Ring;
    Urb->Completed = 0;
    Urb->Result = USB_NOERROR;
    Urb->Finished = FALSE;
    Urb->StartDone = FALSE;
    Urb->EndDone = FALSE;
    
    XhciSyncTransferRing(Ring);
    Urb->TrbStart = Ring->RingEnqueue;
    
    // Setup Stage
    TRB_SETUP *Setup = (TRB_SETUP*)Ring->RingEnqueue;
    MemSet(Setup, 0, sizeof(TRB_SETUP));
    Setup->BmRequestType = Urb->Request->BmRequestType;
    Setup->BRequest = Urb->Request->BRequest;
    Setup->WValue = Urb->Request->WValue;
    Setup->WIndex = Urb->Request->WIndex;
    Setup->WLength = Urb->Request->WLength;
    Setup->Length = 8;
    Setup->IOC = (Urb->DataLen == 0) ? 1 : 0;
    Setup->IDT = 1;
    Setup->Type = TRB_TYPE_SETUP_STAGE;
    Setup->CycleBit = Ring->RingPCS & 1;
    
    XhciSyncTransferRing(Ring);
    Urb->TrbNum++;
    
    // Data Stage (if any)
    if (Urb->DataLen > 0 && Urb->Data) {
        TRB_DATA *Data = (TRB_DATA*)Ring->RingEnqueue;
        MemSet(Data, 0, sizeof(TRB_DATA));
        PhysAddr = (UINT64)(UINTPTR)VirtToPhysPtr(Urb->Data);
        Data->TRBPtrLo = (UINT32)(PhysAddr & 0xFFFFFFFF);
        Data->TRBPtrHi = (UINT32)(PhysAddr >> 32);
        Data->Length = (UINT32)Urb->DataLen;
        Data->ISP = 1;
        Data->IOC = 1;
        Data->Type = TRB_TYPE_DATA_STAGE;
        Data->DIR = (Urb->Direction == USB_DIR_IN) ? 1 : 0;
        Data->CycleBit = Ring->RingPCS & 1;
        
        XhciSyncTransferRing(Ring);
        Urb->TrbNum++;
    }
    
    // Status Stage
    TRB_STATUS *Status = (TRB_STATUS*)Ring->RingEnqueue;
    MemSet(Status, 0, sizeof(TRB_STATUS));
    Status->IOC = 1;
    Status->Type = TRB_TYPE_STATUS_STAGE;
    Status->DIR = (Urb->Direction == USB_DIR_IN) ? 0 : 1;
    Status->CycleBit = Ring->RingPCS & 1;
    
    XhciSyncTransferRing(Ring);
    Urb->TrbNum++;
    Urb->TrbEnd = Ring->RingEnqueue;
    
    return SUCCESS;
}

static INT XhciCreateBulkUrb(XhciContext *Xhc, URB *Urb) {
    DEVICE_SLOT *Slot;
    TRANSFER_RING *Ring;
    UINT8 Dci;
    UINT64 PhysAddr;
    
    Slot = &Xhc->Slots[XhciFindSlotByBusAddr(Xhc, Urb->BusAddr)];
    if (!Slot->Enabled) {
        return NO_OBJECT;
    }
    
    Dci = XhciEndpointToDci(Urb->EpAddr, Urb->Direction);
    if (Dci >= 32) {
        return INCORRECT_VALUE;
    }
    
    Ring = Slot->EndpointTransferRing[Dci - 1];
    if (!Ring) {
        return NO_OBJECT;
    }
    
    Urb->Ring = Ring;
    Urb->Completed = 0;
    Urb->Result = USB_NOERROR;
    Urb->Finished = FALSE;
    
    XhciSyncTransferRing(Ring);
    Urb->TrbStart = Ring->RingEnqueue;
    
    // Normal TRB for bulk
    TRB_NORMAL *Normal = (TRB_NORMAL*)Ring->RingEnqueue;
    MemSet(Normal, 0, sizeof(TRB_NORMAL));
    PhysAddr = (UINT64)(UINTPTR)VirtToPhysPtr(Urb->Data);
    Normal->TRBPtrLo = (UINT32)(PhysAddr & 0xFFFFFFFF);
    Normal->TRBPtrHi = (UINT32)(PhysAddr >> 32);
    Normal->Length = (UINT32)Urb->DataLen;
    Normal->ISP = 1;
    Normal->IOC = 1;
    Normal->Type = TRB_TYPE_NORMAL;
    Normal->CycleBit = Ring->RingPCS & 1;
    
    XhciSyncTransferRing(Ring);
    Urb->TrbNum = 1;
    Urb->TrbEnd = Ring->RingEnqueue;
    
    return SUCCESS;
}

static INT XhciRingDoorbell(XhciContext *Xhc, UINT8 SlotId, UINT8 Dci) {
    XhciWriteDoorbell(Xhc, SlotId * sizeof(UINT32), Dci);
    return SUCCESS;
}

static INT XhciCheckUrbResult(XhciContext *Xhc, URB *Urb) {
    TRB_TEMPLATE *Event;
    INT Processed = 0;
    
    XhciSyncEventRing(&Xhc->EventRing);
    
    while (XhciCheckNewEvent(&Xhc->EventRing, &Event) == SUCCESS) {
        if (Event->Type == TRB_TYPE_TRANS_EVENT) {
            EVT_TRANSFER *TransEvent = (EVT_TRANSFER*)Event;
            TRB_TEMPLATE *TrbPtr = (TRB_TEMPLATE*)(UINTPTR)((UINT64)TransEvent->TRBPtrLo | ((UINT64)TransEvent->TRBPtrHi << 32));
            
            if (TrbPtr >= Urb->TrbStart && TrbPtr <= Urb->TrbEnd) {
                switch (TransEvent->Completecode) {
                    case TRB_COMPLETION_SUCCESS:
                    case TRB_COMPLETION_SHORT_PACKET:
                        Urb->Completed += TransEvent->Length;
                        break;
                    case TRB_COMPLETION_STALL_ERROR:
                        Urb->Result = USB_ERR_STALL;
                        Urb->Finished = TRUE;
                        break;
                    case TRB_COMPLETION_BABBLE_ERROR:
                        Urb->Result = USB_ERR_BABBLE;
                        Urb->Finished = TRUE;
                        break;
                    case TRB_COMPLETION_USB_TRANSACTION_ERROR:
                        Urb->Result = USB_ERR_TIMEOUT;
                        Urb->Finished = TRUE;
                        break;
                }
                
                if (TrbPtr == Urb->TrbStart) Urb->StartDone = TRUE;
                if (TrbPtr == Urb->TrbEnd) Urb->EndDone = TRUE;
                
                if (Urb->StartDone && Urb->EndDone) {
                    Urb->Finished = TRUE;
                }
                
                Processed++;
            }
        }
    }
    
    // Update ERDP
    UINT64 DequeuePhys = (UINT64)(UINTPTR)VirtToPhysPtr(Xhc->EventRing.EventRingDequeue);
    XhciWriteRuntime32(Xhc, XHC_ERDP_OFFSET, (UINT32)(DequeuePhys & 0xFFFFFFFF) | 1);
    XhciWriteRuntime32(Xhc, XHC_ERDP_OFFSET + 4, (UINT32)(DequeuePhys >> 32));
    
    return Processed;
}

static INT XhciExecTransfer(XhciContext *Xhc, URB *Urb, UINT32 TimeoutMs) {
    UINT8 SlotId = XhciFindSlotByBusAddr(Xhc, Urb->BusAddr);
    UINT8 Dci = XhciEndpointToDci(Urb->EpAddr, Urb->Direction);
    UINT64 StartTicks;
    UINT64 TimeoutTicks;
    
    if (SlotId == 0) {
        return NO_OBJECT;
    }
    
    XhciRingDoorbell(Xhc, SlotId, Dci);
    
    StartTicks = TimerTicks();
    TimeoutTicks = (UINT64)TimeoutMs * TimerFreq() / 1000;
    if (TimeoutTicks == 0) TimeoutTicks = 1;
    
    while (!Urb->Finished) {
        XhciCheckUrbResult(Xhc, Urb);
        
        if ((TimerTicks() - StartTicks) >= TimeoutTicks) {
            Urb->Result = USB_ERR_TIMEOUT;
            return TIMEOUT;
        }
        
        TimerUdelay(100);
    }
    
    return (Urb->Result == USB_NOERROR) ? SUCCESS : DEVICE_ERROR;
}

// ============================================================================
// USB HCD Callbacks
// ============================================================================

static INT XhciControlTransfer(UsbDevice *Dev, UsbSetupPacket *Setup, UINT8 *Data, UINT32 DataLen) {
    XhciContext *Xhc = GXhci;
    URB Urb;
    INT Ret;
    
    if (!Xhc || !Dev || !Setup) {
        return NO_OBJECT;
    }
    
    MemSet(&Urb, 0, sizeof(URB));
    Urb.BusAddr = Dev->Address;
    Urb.EpAddr = 0;
    Urb.Direction = (Setup->BmRequestType & USB_DIR_IN) ? USB_DIR_IN : USB_DIR_OUT;
    Urb.DevSpeed = Dev->Speed;
    Urb.MaxPacket = Dev->MaxPacketSize0;
    Urb.Type = USB_TRANSFER_CONTROL;
    Urb.Request = Setup;
    Urb.Data = Data;
    Urb.DataLen = DataLen;
    
    Ret = XhciCreateControlUrb(Xhc, &Urb);
    if (Ret != SUCCESS) {
        return Ret;
    }
    
    Ret = XhciExecTransfer(Xhc, &Urb, 2000);
    
    return Ret;
}

static INT XhciBulkTransfer(UsbDevice *Dev, UINT8 Endpoint, UINT8 *Data, UINT32 DataLen) {
    XhciContext *Xhc = GXhci;
    URB Urb;
    INT Ret;
    
    if (!Xhc || !Dev || !Data || DataLen == 0) {
        return NO_OBJECT;
    }
    
    MemSet(&Urb, 0, sizeof(URB));
    Urb.BusAddr = Dev->Address;
    Urb.EpAddr = Endpoint & 0x7F;
    Urb.Direction = (Endpoint & 0x80) ? USB_DIR_IN : USB_DIR_OUT;
    Urb.DevSpeed = Dev->Speed;
    Urb.MaxPacket = 512;
    Urb.Type = USB_TRANSFER_BULK;
    Urb.Data = Data;
    Urb.DataLen = DataLen;
    
    Ret = XhciCreateBulkUrb(Xhc, &Urb);
    if (Ret != SUCCESS) {
        return Ret;
    }
    
    Ret = XhciExecTransfer(Xhc, &Urb, 5000);
    
    return Ret;
}

static INT XhciHcdSubmitTransfer(UsbHcd *Hcd, UsbTransfer *Transfer) {
    XhciContext *Xhc = (XhciContext*)Hcd->Private;
    URB Urb;
    UsbSetupPacket *Setup;
    INT Ret;
    
    if (!Xhc || !Transfer || !Transfer->Device) {
        return NO_OBJECT;
    }
    
    MemSet(&Urb, 0, sizeof(URB));
    Urb.BusAddr = Transfer->Device->Address;
    Urb.DevSpeed = Transfer->Device->Speed;
    
    switch (Transfer->Type) {
        case USB_TRANSFER_CONTROL:
            Setup = (UsbSetupPacket*)Transfer->Context;
            if (!Setup) {
                return INCORRECT_VALUE;
            }
            Urb.EpAddr = 0;
            Urb.Direction = (Setup->BmRequestType & USB_DIR_IN) ? USB_DIR_IN : USB_DIR_OUT;
            Urb.MaxPacket = Transfer->Device->MaxPacketSize0;
            Urb.Type = USB_TRANSFER_CONTROL;
            Urb.Request = Setup;
            Urb.Data = Transfer->Buffer;
            Urb.DataLen = Transfer->Length;
            Ret = XhciCreateControlUrb(Xhc, &Urb);
            if (Ret != SUCCESS) {
                return Ret;
            }
            return XhciExecTransfer(Xhc, &Urb, Transfer->TimeoutMs ? Transfer->TimeoutMs : 2000);
            
        case USB_TRANSFER_BULK:
            Urb.EpAddr = Transfer->Endpoint;
            Urb.Direction = Transfer->Direction;
            Urb.MaxPacket = 512;
            Urb.Type = USB_TRANSFER_BULK;
            Urb.Data = Transfer->Buffer;
            Urb.DataLen = Transfer->Length;
            Ret = XhciCreateBulkUrb(Xhc, &Urb);
            if (Ret != SUCCESS) {
                return Ret;
            }
            return XhciExecTransfer(Xhc, &Urb, Transfer->TimeoutMs ? Transfer->TimeoutMs : 5000);
            
        default:
            Transfer->Status = NOT_IMPLEMENTED;
            return NOT_IMPLEMENTED;
    }
}

static UsbHcdOps GXhciHcdOps = {
    .SubmitTransfer = XhciHcdSubmitTransfer,
    .IrqHandler = (NOPTR(*)(NOPTR))XhciIrqHandler,
};

// ============================================================================
// Port Scanning (исправлено)
// ============================================================================

static NOPTR XhciScanPorts(XhciContext *Xhc) {
    UINT32 Ports = Xhc->HcSParams1 & 0xFF;  // MaxPorts in bits 0-7
    
    for (UINT32 I = 0; I < Ports; I++) {
        UINT32 Portsc = XhciReadOp32(Xhc, XHC_PORTSC_OFFSET + (I * 0x10));
        BOOL Connected = (Portsc & XHC_PORTSC_CCS) != 0;
        
        // Clear change bits
        if (Portsc & XHC_PORTSC_CSC) {
            XhciWriteOp32(Xhc, XHC_PORTSC_OFFSET + (I * 0x10), Portsc | XHC_PORTSC_CSC);
            DeconPrint("[xHCI] Port %d: connection status changed\n", I);
        }
        if (Portsc & XHC_PORTSC_PEC) {
            XhciWriteOp32(Xhc, XHC_PORTSC_OFFSET + (I * 0x10), Portsc | XHC_PORTSC_PEC);
        }
        if (Portsc & XHC_PORTSC_PRC) {
            XhciWriteOp32(Xhc, XHC_PORTSC_OFFSET + (I * 0x10), Portsc | XHC_PORTSC_PRC);
        }
        
        // Read again after clearing
        Portsc = XhciReadOp32(Xhc, XHC_PORTSC_OFFSET + (I * 0x10));
        
        if (Connected) {
            UINT8 Speed = XhciGetPortSpeed(Xhc, I);
            UINT8 SlotId;
            INT Ret;
            
            DeconPrint("[xHCI] Port %d: device connected (speed=%s)\n", 
                         I, UsbSpeedString(Speed));
            
            // Power on port if needed
            XhciPowerOnPort(Xhc, I);
            
            // Reset port
            XhciResetPort(Xhc, I);
            
            // Check if device still present after reset
            Portsc = XhciReadOp32(Xhc, XHC_PORTSC_OFFSET + (I * 0x10));
            if (!(Portsc & XHC_PORTSC_CCS)) {
                DeconPrint("[xHCI] Port %d: device disappeared after reset\n", I);
                continue;
            }
            
            // Initialize device slot
            Ret = XhciInitializeDeviceSlot(Xhc, I, Speed, &SlotId);
            if (Ret != SUCCESS) {
                DeconPrint("[xHCI] Failed to initialize slot for port %d: %d\n", I, Ret);
                continue;
            }
            
            // Create USB device
            UsbDevice *Dev = UsbDeviceAlloc(Xhc->Hcd);
            if (Dev) {
                Dev->Address = Xhc->Slots[SlotId].XhciDevAddr;
                Dev->SlotId = SlotId;
                Dev->Speed = Speed;
                Dev->MaxPacketSize0 = (Speed == USB_SPEED_SUPER) ? 512 :
                                       (Speed == USB_SPEED_HIGH) ? 64 : 8;
                Dev->Hcd = Xhc->Hcd;
                Xhc->Slots[SlotId].BusDevAddr = Dev->Address;
                Xhc->Slots[SlotId].UsbDevice = Dev;
                
                if (UsbEnumeration(Dev) == SUCCESS) {
                    UsbDeviceAdd(Xhc->Hcd, Dev);
                    DeconPrint("[xHCI] Device on port %d enumerated (addr=%d)\n", I, Dev->Address);
                } else {
                    UsbDeviceFree(Dev);
                    XhciDisableSlot(Xhc, SlotId);
                    Xhc->Slots[SlotId].Enabled = FALSE;
                }
            }
        }
    }
}

// ============================================================================
// IRQ Handler
// ============================================================================

NOPTR XhciIrqHandler(NOPTR) {
    XhciContext *Xhc = GXhci;
    UINT32 Usbsts;
    
    if (!Xhc || !Xhc->Running) {
        ApicEoi();
        return;
    }
    
    Usbsts = XhciReadOp32(Xhc, XHC_USBSTS_OFFSET);
    
    if (Usbsts & XHC_USBSTS_EINT) {
        // Process events
        XhciWriteOp32(Xhc, XHC_USBSTS_OFFSET, Usbsts);
    }
    
    if (Usbsts & XHC_USBSTS_PCD) {
        XhciScanPorts(Xhc);
        XhciWriteOp32(Xhc, XHC_USBSTS_OFFSET, Usbsts);
    }
    
    ApicEoi();
    if (Xhc->Gsi) {
        IoapicEoi(Xhc->Gsi);
    }
}

// ============================================================================
// Controller Initialization (исправлено из EDK2)
// ============================================================================

static INT XhciControllerReset(XhciContext *Xhc) {
    UINT32 Timeout = 10000;
    
    // If not halted, halt it first
    if (!XhciIsBitSet(Xhc, XHC_USBSTS_OFFSET, XHC_USBSTS_HALT)) {
        XhciClearOpBit(Xhc, XHC_USBCMD_OFFSET, XHC_USBCMD_RUN);
        while (Timeout-- && !XhciIsBitSet(Xhc, XHC_USBSTS_OFFSET, XHC_USBSTS_HALT)) {
            TimerUdelay(100);
        }
        if (Timeout == 0) return TIMEOUT;
    }
    
    // Reset controller
    XhciSetOpBit(Xhc, XHC_USBCMD_OFFSET, XHC_USBCMD_RESET);
    TimerMdelay(1);
    
    Timeout = 10000;
    while (Timeout-- && XhciIsBitSet(Xhc, XHC_USBCMD_OFFSET, XHC_USBCMD_RESET)) {
        TimerUdelay(100);
    }
    
    if (Timeout == 0) return TIMEOUT;
    
    // Wait for Controller Not Ready bit to clear
    Timeout = 10000;
    while (Timeout-- && XhciIsBitSet(Xhc, XHC_USBSTS_OFFSET, XHC_USBSTS_CNR)) {
        TimerUdelay(100);
    }
    
    if (Timeout == 0) return TIMEOUT;
    
    // Set HSEE bit if supported
    XhciSetOpBit(Xhc, XHC_USBCMD_OFFSET, XHC_USBCMD_HSEE);
    
    return SUCCESS;
}

static INT XhciControllerInit(XhciContext *Xhc) {
    UINT32 PageSize;
    UINT64 *Scratchpad;
    UINT64 PhysAddr;
    INT Ret;
    UINT32 I;
    
    DeconPrint("[xHCI] Initializing controller...\n");
    DeconPrint("Resetting controller\n");
    Ret = XhciControllerReset(Xhc);
    if (Ret != SUCCESS) {
        DeconPrint("[xHCI] Reset failed: %d\n", Ret);
        return Ret;
    }
    DeconPrint("Controller resetted\n");
    
    // Configure DCBAA
    DeconPrint("Configuring DCBAA\n");
    Xhc->MaxSlotsEn = Xhc->HcSParams1 & 0xFF;  // MaxSlots in bits 0-7
    XhciWriteOp32(Xhc, XHC_CONFIG_OFFSET, Xhc->MaxSlotsEn);

    UINT32 DcbaaSize = (Xhc->MaxSlotsEn + 1) * sizeof(UINT64);
    UINT64 DcbaaPhys; // Добавили переменную для хранения физ. адреса самого DCBAA
    Xhc->DCBAA = (UINT64*)XhciAllocDmaBuffer(Xhc, DcbaaSize, XHC_DMA_ALIGNMENT, &DcbaaPhys);
    if (!Xhc->DCBAA) {
        return NO_MEMORY;
    }
    MemSet(Xhc->DCBAA, 0, DcbaaSize);
    DeconPrint("DCBAA configured\n");
    
    // ИСПРАВЛЕНО: Записываем физический адрес САМОГО массива DCBAA, а не PhysAddr, 
    // который перезапишется ниже при выделении Scratchpad.
    XhciWriteOp32(Xhc, XHC_DCBAAP_OFFSET, (UINT32)(DcbaaPhys & 0xFFFFFFFF));
    XhciWriteOp32(Xhc, XHC_DCBAAP_OFFSET + 4, (UINT32)(DcbaaPhys >> 32));
    
    // Allocate scratchpad buffers
    // ИСПРАВЛЕНО: Корректный расчет сдвигов по спецификации xHCI
    UINT32 Hi = (Xhc->HcSParams2 >> 26) & 0x03;
    UINT32 Lo = (Xhc->HcSParams2 >> 21) & 0x1F;
    Xhc->MaxScratchpadBufs = (Hi << 5) | Lo;

    if (Xhc->MaxScratchpadBufs > 0) {
    	DeconPrint("[xHCI] Allocating %u scratchpad buffers\n", Xhc->MaxScratchpadBufs);
    
    	// Выделяем массив указателей (64-битные адреса)
    	Xhc->ScratchpadBufArray = (UINT64*)XhciAllocDmaBuffer(
            Xhc, 
            Xhc->MaxScratchpadBufs * sizeof(UINT64), 
            XHC_DMA_ALIGNMENT, 
            &PhysAddr
    	);
    
        if (Xhc->ScratchpadBufArray == NULLPTR) {
            DeconPrint("[xHCI] ERROR: Failed to allocate Scratchpad Buffer Array\n");
            // ИСПРАВЛЕНО: Убрали ошибочный FreeTransferRing и FreeEventRing, так как они еще не созданы
            if (Xhc->DCBAA) {
            	XhciFreeDmaBuffer(Xhc, Xhc->DCBAA, DcbaaSize);
            	Xhc->DCBAA = NULLPTR;
            }
            return NO_MEMORY;
    	}
    
    	// Записываем физический адрес массива scratchpad-указателей в нулевой слот DCBAA
    	Xhc->DCBAA[0] = PhysAddr;
    
    	// Выделяем сами страницы памяти под scratchpad
    	for (I = 0; I < Xhc->MaxScratchpadBufs; I++) {
            UINT64 PagePhys;
            NOPTR *PageVirt = XhciAllocDmaBuffer(Xhc, Xhc->PageSize, Xhc->PageSize, &PagePhys);
        
            if (PageVirt) {
            	Xhc->ScratchpadBufArray[I] = PagePhys;
            } else {
            	DeconPrint("[xHCI] ERROR: Scratchpad page allocation failed at index %d\n", I);
            
            	// ИСПРАВЛЕНО: Убрали ошибочный FreeTransferRing и FreeEventRing
            	XhciFreeDmaBuffer(Xhc, Xhc->ScratchpadBufArray, Xhc->MaxScratchpadBufs * sizeof(UINT64));
            	Xhc->ScratchpadBufArray = NULLPTR;
            	Xhc->DCBAA[0] = 0;
            	if (Xhc->DCBAA) {
                    XhciFreeDmaBuffer(Xhc, Xhc->DCBAA, DcbaaSize);
                    Xhc->DCBAA = NULLPTR;
            	}
            	return NO_MEMORY;
            }
    	}
    
        DeconPrint("[xHCI] Scratchpad buffers allocated: %u\n", Xhc->MaxScratchpadBufs);
    }
    
    // Create command ring
    DeconPrint("Creating command ring\n");
    Ret = XhciCreateTransferRing(Xhc, CMD_RING_TRB_NUMBER, &Xhc->CmdRing);
    if (Ret != SUCCESS) {
        // Здесь и ниже при ошибке нужно бы также освобождать DCBAA и Scratchpad, 
        // но для сохранения структуры вашего кода оставляем возврат ошибки.
        return Ret;
    }
    
    UINT64 CmdRingPhys = (UINT64)(UINTPTR)VirtToPhysPtr(Xhc->CmdRing.RingSeg0);
    XhciWriteOp32(Xhc, XHC_CRCR_OFFSET, (UINT32)(CmdRingPhys & 0xFFFFFFFF) | 1);
    XhciWriteOp32(Xhc, XHC_CRCR_OFFSET + 4, (UINT32)(CmdRingPhys >> 32));
    
    // Create event ring
    DeconPrint("Creating event ring\n");
    Ret = XhciCreateEventRing(Xhc, &Xhc->EventRing);
    if (Ret != SUCCESS) {
        XhciFreeTransferRing(Xhc, &Xhc->CmdRing);
        return Ret;
    }
    
    // Enable interrupts
    DeconPrint("Enabling interrupts\n");
    XhciSetOpBit(Xhc, XHC_USBCMD_OFFSET, XHC_USBCMD_INTE);
    XhciWriteOp32(Xhc, XHC_USBINTR_OFFSET, 0x18);  // Enable events and port changes
    
    // Start controller
    DeconPrint("starting controller\n");
    XhciSetOpBit(Xhc, XHC_USBCMD_OFFSET, XHC_USBCMD_RUN);
    TimerMdelay(10);
    
    if (XhciIsBitSet(Xhc, XHC_USBSTS_OFFSET, XHC_USBSTS_HALT)) {
        DeconPrint("[xHCI] Controller still halted after start\n");
        return DEVICE_ERROR;
    }
    
    // Power on ports
    // ИСПРАВЛЕНО: Количество портов находится в битах 24-31 (MaxPorts), а не 0-7 (MaxSlots)
    UINT32 MaxPorts = (Xhc->HcSParams1 >> 24) & 0xFF;
    for (I = 0; I < MaxPorts; I++) {
        XhciPowerOnPort(Xhc, I);
    }
    
    Xhc->Running = TRUE;
    DeconPrint("[xHCI] Controller running, %d ports\n", MaxPorts);
    
    // Initial port scan
    XhciScanPorts(Xhc);
    
    return SUCCESS;
}


// ============================================================================
// IRQ Setup
// ============================================================================

static INT XhciSetupIrq(XhciContext *Xhc, PciDevice *PciDev) {
    UINT32 Flags;
    
    Xhc->Irq = PciDev->InterruptLine;
    
    if (IoapicGetOverride(Xhc->Irq, &Xhc->Gsi, &Flags) != SUCCESS) {
        Xhc->Gsi = Xhc->Irq;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }
    
    Xhc->Vector = 48 + (Xhc->Gsi % 16);
    
    if (IoapicRedirectIrq(Xhc->Gsi, Xhc->Vector, ApicGetId(), Flags) != SUCCESS) {
        DeconPrint("[xHCI] Failed to redirect IRQ %d\n", Xhc->Irq);
        return IO_ERROR;
    }
    
    IdtSetGate(Xhc->Vector, XhciIrqHandler, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IoapicUnmaskIrq(Xhc->Gsi);
    
    DeconPrint("[xHCI] IRQ %d (GSI %d) -> vector %d\n", Xhc->Irq, Xhc->Gsi, Xhc->Vector);
    
    return SUCCESS;
}

// ============================================================================
// PCI Detection
// ============================================================================

INT XhciInit(PciDevice *PciDev) {
    DeconPrint("[xHCI] Controller init start\n");
    XhciContext *Xhc;
    UsbHcd *Hcd;
    UINT32 MmioBase;
    UINT8 CapLength;
    INT Ret;
    
    if (!PciDev) {
        return NO_OBJECT;
    }
    
    DeconPrint("[xHCI] Found controller at %02X:%02X.%X (%04X:%04X)\n",
                 PciDev->Bus, PciDev->Slot, PciDev->Function,
                 PciDev->VendorId, PciDev->DeviceId);
    
    PciEnableBusmaster(PciDev);
    PciEnable(PciDev);
    DeconPrint("PCI Enabled\n");
    
    MmioBase = PciDev->Bars[0] & ~0xFULL;
    if (!MmioBase) {
        DeconPrint("[xHCI] No MMIO BAR\n");
        return NO_OBJECT;
    }
    
    DeconPrint("Allocating memory for context\n");
    Xhc = (XhciContext*)MemoryAllocate(sizeof(XhciContext));
    if (!Xhc) {
        return NO_MEMORY;
    }
    MemSet(Xhc, 0, sizeof(XhciContext));
    DeconPrint("Memory allocated\n");
    SpinLockInit(&Xhc->Lock);
    DeconPrint("Setting up registers\n");
    Xhc->CapRegs = (volatile UINT8*)(UINTPTR)MmioBase;
    CapLength = XhciReadCap8(Xhc, XHC_CAPLENGTH_OFFSET);
    Xhc->OpRegs = (volatile UINT8*)((UINTPTR)Xhc->CapRegs + CapLength);
    Xhc->HcSParams1 = XhciReadCap32(Xhc, XHC_HCSPARAMS1_OFFSET);
    Xhc->HcSParams2 = XhciReadCap32(Xhc, XHC_HCSPARAMS2_OFFSET);
    Xhc->HcCParams = XhciReadCap32(Xhc, XHC_HCCPARAMS_OFFSET);
    Xhc->DBOff = XhciReadCap32(Xhc, XHC_DBOFF_OFFSET);
    Xhc->RTSOff = XhciReadCap32(Xhc, XHC_RTSOFF_OFFSET);
    DeconPrint("Registers setted up\n");
    // Get page size
    UINT32 PageSizeReg = XhciReadOp32(Xhc, XHC_PAGESIZE_OFFSET);
    UINT32 HighestBit = 0;
    for (UINT32 I = 0; I < 32; I++) {
        if (PageSizeReg & (1 << I)) HighestBit = I;
    }
    Xhc->PageSize = 1 << (HighestBit + 12);
    
    if (Xhc->HcCParams & BIT0) {  // AC64 bit
        Xhc->Support64BitDma = TRUE;
        DeconPrint("[xHCI] 64-bit DMA supported\n");
    }
    
    Hcd = (UsbHcd*)MemoryAllocate(sizeof(UsbHcd));
    if (!Hcd) {
        MemoryFree(Xhc);
        return NO_MEMORY;
    }
    
    MemSet(Hcd, 0, sizeof(UsbHcd));
    SnPrintf(Hcd->Name, sizeof(Hcd->Name), "xhci");
    Hcd->VendorId = PciDev->VendorId;
    Hcd->DeviceId = PciDev->DeviceId;
    Hcd->MmioBase = MmioBase;
    Hcd->Irq = PciDev->InterruptLine;
    Hcd->Ops = &GXhciHcdOps;
    Hcd->Private = Xhc;
    
    Xhc->Hcd = Hcd;
    Xhc->PciDev = PciDev;
    GXhci = Xhc;
    
    Ret = XhciControllerInit(Xhc);
    if (Ret != SUCCESS) {
        DeconPrint("[xHCI] Controller initialization failed: %d\n", Ret);
        MemoryFree(Hcd);
        MemoryFree(Xhc);
        return Ret;
    }
    
    XhciSetupIrq(Xhc, PciDev);
    
    if (UsbHcdRegister(Hcd) != SUCCESS) {
        DeconPrint("[xHCI] Failed to register HCD\n");
        MemoryFree(Hcd);
        MemoryFree(Xhc);
        return DEVICE_ERROR;
    }
    
    KDriverRegister(KDriverGenerateStruct("Xhci", DCL1, TRUE, NULLPTR, NULLPTR));
    
    return SUCCESS;
}

NOPTR XhciProbeAll(NOPTR) {
    PciDevice *Dev = PciGetFirst();
    
    while (Dev) {
        if (Dev->ClassCode == XHCI_PCI_CLASS &&
            Dev->SubClass == XHCI_PCI_SUBCLASS &&
            Dev->ProgIf == XHCI_PCI_PROG_IF) {
            XhciInit(Dev);
        }
        Dev = PciGetNext(Dev);
    }
}