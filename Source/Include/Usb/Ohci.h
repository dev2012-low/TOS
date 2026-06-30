#pragma once

#include <Usb/Usb.h>
#include <Kernel/Types.h>
#include <Pci.h>

#define OHCI_PCI_CLASS      0x0C
#define OHCI_PCI_SUBCLASS   0x03
#define OHCI_PCI_PROG_IF    0x10

// ==================== OHCI REGISTERS ====================
#define OHCI_REVISION       0x00
#define OHCI_CONTROL        0x04
#define OHCI_COMMAND_STATUS 0x08
#define OHCI_INTERRUPT_STATUS 0x0C
#define OHCI_INTERRUPT_ENABLE 0x10
#define OHCI_INTERRUPT_DISABLE 0x14
#define OHCI_HCCA           0x18
#define OHCI_PERIOD_CURRENT_ED 0x1C
#define OHCI_CONTROL_HEAD_ED 0x20
#define OHCI_CONTROL_CURRENT_ED 0x24
#define OHCI_BULK_HEAD_ED   0x28
#define OHCI_BULK_CURRENT_ED 0x2C
#define OHCI_DONE_HEAD      0x30
#define OHCI_FM_INTERVAL    0x34
#define OHCI_FM_REMAINING   0x38
#define OHCI_FM_NUMBER      0x3C
#define OHCI_PERIOD_START   0x40
#define OHCI_LS_THRESHOLD   0x44
#define OHCI_RH_DESCRIPTOR_A 0x48
#define OHCI_RH_DESCRIPTOR_B 0x4C
#define OHCI_RH_STATUS      0x50
#define OHCI_RH_PORT_STATUS(N) (0x54 + (N) * 4)

// OHCI_CONTROL bits
#define OHCI_CTRL_CBSR      (3 << 0)
#define OHCI_CTRL_PLE       (1 << 2)
#define OHCI_CTRL_IE        (1 << 3)
#define OHCI_CTRL_CLE       (1 << 4)
#define OHCI_CTRL_BLE       (1 << 5)
#define OHCI_CTRL_HCFS      (3 << 6)
#define OHCI_CTRL_IR        (1 << 8)
#define OHCI_CTRL_RWC       (1 << 9)
#define OHCI_CTRL_RWE       (1 << 10)

// OHCI_COMMAND_STATUS bits
#define OHCI_CMD_HCR        (1 << 0)
#define OHCI_CMD_CLF        (1 << 1)
#define OHCI_CMD_BLF        (1 << 2)
#define OHCI_CMD_OCR        (1 << 3)
#define OHCI_CMD_SOC        (3 << 16)

// OHCI_INTERRUPT bits
#define OHCI_INTR_SO        (1 << 0)
#define OHCI_INTR_WDH       (1 << 1)
#define OHCI_INTR_SF        (1 << 2)
#define OHCI_INTR_RD        (1 << 3)
#define OHCI_INTR_UE        (1 << 4)
#define OHCI_INTR_FNO       (1 << 5)
#define OHCI_INTR_RHSC      (1 << 6)
#define OHCI_INTR_OC        (1 << 30)
#define OHCI_INTR_MIE       (1 << 31)

// RH_DESCRIPTOR_A bits
#define OHCI_RHA_NDP(x)     ((x) & 0xFF)
#define OHCI_RHA_PSM        (1 << 8)
#define OHCI_RHA_NPS        (1 << 9)
#define OHCI_RHA_DT         (1 << 10)
#define OHCI_RHA_OCPM       (1 << 11)
#define OHCI_RHA_NOCP       (1 << 12)
#define OHCI_RHA_POTPGT(X)  (((X) >> 24) & 0xFF)

// RH_PORT_STATUS bits
#define OHCI_RHPS_CCS       (1 << 0)
#define OHCI_RHPS_PES       (1 << 1)
#define OHCI_RHPS_PSS       (1 << 2)
#define OHCI_RHPS_POCI      (1 << 3)
#define OHCI_RHPS_PRS       (1 << 4)
#define OHCI_RHPS_PPS       (1 << 8)
#define OHCI_RHPS_LSDA      (1 << 9)
#define OHCI_RHPS_CSC       (1 << 16)
#define OHCI_RHPS_PESC      (1 << 17)
#define OHCI_RHPS_PSSC      (1 << 18)
#define OHCI_RHPS_OCIC      (1 << 19)
#define OHCI_RHPS_PRSC      (1 << 20)

// ED (Endpoint Descriptor) flags
#define OHCI_ED_SKIP        (1 << 14)
#define OHCI_ED_INACTIVE    (1 << 15)

// TD (Transfer Descriptor) flags
#define OHCI_TD_BUFFER_ROUNDING (1 << 24)
#define OHCI_TD_SETUP       (0 << 18)
#define OHCI_TD_OUT         (1 << 18)
#define OHCI_TD_IN          (2 << 18)
#define OHCI_TD_NO_DELAY    (3 << 18)
#define OHCI_TD_TOGGLE_0    (0 << 18)
#define OHCI_TD_TOGGLE_1    (1 << 18)
#define OHCI_TD_TOGGLE_FROM_ED (2 << 18)
#define OHCI_TD_TOGGLE_UPDATE (3 << 18)

#define OHCI_TD_CC_NO_ERROR 0
#define OHCI_TD_CC_CRC      1
#define OHCI_TD_CC_BIT_STUFFING 2
#define OHCI_TD_CC_DATA_TOGGLE 3
#define OHCI_TD_CC_STALL    4
#define OHCI_TD_CC_DEVICE_NOT_RESPONDING 5
#define OHCI_TD_CC_PID_FAILURE 6
#define OHCI_TD_CC_UNEXPECTED_PID 7
#define OHCI_TD_CC_DATA_OVERRUN 8
#define OHCI_TD_CC_DATA_UNDERRUN 9
#define OHCI_TD_CC_BUFFER_OVERRUN 12
#define OHCI_TD_CC_BUFFER_UNDERRUN 13

// ==================== STRUCTURES ====================

// HCCA (Host Controller Communication Area)
typedef struct ATTRIBUTE(packed) ATTRIBUTE(aligned(256)) {
    UINT32 InterruptTable[32];
    UINT16 FrameNumber;
    UINT16 Pad1;
    UINT32 DoneHead;
    UINT8 Reserved[120];
} OhciHcca;

// ED (Endpoint Descriptor)
typedef struct ATTRIBUTE(packed) ATTRIBUTE(aligned(16)) OhciEd {
    UINT32 Flags;
    UINT32 TailP;
    UINT32 HeadP;
    UINT32 NextEd;
} OhciEd;

// TD (Transfer Descriptor)
typedef struct ATTRIBUTE(packed) ATTRIBUTE(aligned(16)) OhciTd {
    UINT32 Flags;
    UINT32 Cbp;      // Current Buffer Pointer
    UINT32 NextTd;
    UINT32 Be;       // Buffer End
    NOPTR (*Callback)(struct OhciTd *Td, UINT32 Condition);
    NOPTR *Context;
    NOPTR *Urb;
} OhciTd;

// ==================== OHCI CONTEXT ====================

typedef struct OhciCtx {
    UsbHcd *Hcd;
    volatile NOPTR *Regs;
    
    OhciHcca *Hcca;
    UINT32 HccaPhys;
    BOOL MsiEnabled;
    
    UINT32 ControlHead;
    UINT32 BulkHead;
    
    UINT32 Ports;
    UINT32 Gsi;
    UINT8 Vector;
    UINT8 Irq;
    BOOL Running;
    
    PciDevice *PciDev;
} OhciCtx;

// ==================== FUNCTION PROTOTYPES ====================

INT OhciInit(PciDevice *PciDev);
NOPTR OhciProbeAll(NOPTR);