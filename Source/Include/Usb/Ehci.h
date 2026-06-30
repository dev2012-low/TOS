// Include/Usb/Ehci.h
#pragma once

#include <Usb/Usb.h>
#include <Kernel/Types.h>
#include <Pci.h>

// PCI IDs
#define EHCI_CLASS_CODE        0x0C
#define EHCI_SUBCLASS          0x03
#define EHCI_PROG_IF           0x20

// Capability registers
#define EHC_CAPLENGTH_OFFSET    0x00
#define EHC_HCSPARAMS_OFFSET    0x04
#define EHC_HCCPARAMS_OFFSET    0x08

// HCSPARAMS bits
#define HCSP_NPORTS             0x0F
#define HCSP_PPC                (1 << 4)

// HCCPARAMS bits
#define HCCP_64BIT              (1 << 0)
#define HCCP_EECP_MASK          0xFF00

// Operational registers
#define EHC_USBCMD_OFFSET       0x00
#define EHC_USBSTS_OFFSET       0x04
#define EHC_USBINTR_OFFSET      0x08
#define EHC_FRINDEX_OFFSET      0x0C
#define EHC_CTRLDSSEG_OFFSET    0x10
#define EHC_FRAME_BASE_OFFSET   0x14
#define EHC_ASYNC_HEAD_OFFSET   0x18
#define EHC_CONFIG_FLAG_OFFSET  0x40
#define EHC_PORT_STAT_OFFSET    0x44

// USBCMD bits
#define USBCMD_RUN              (1 << 0)
#define USBCMD_RESET            (1 << 1)
#define USBCMD_ENABLE_PERIOD    (1 << 4)
#define USBCMD_ENABLE_ASYNC     (1 << 5)
#define USBCMD_IAAD             (1 << 6)

// USBSTS bits
#define USBSTS_IAA              (1 << 5)
#define USBSTS_HALT             (1 << 12)
#define USBSTS_SYS_ERROR        (1 << 2)
#define USBSTS_PERIOD_ENABLED   (1 << 14)
#define USBSTS_ASYNC_ENABLED    (1 << 15)
#define USBSTS_INTACK_MASK      0x003F

// USBINTR bits
#define EHCI_INTR_USB           (1 << 0)
#define EHCI_INTR_ERROR         (1 << 1)
#define EHCI_INTR_PORT_CHANGE   (1 << 2)
#define EHCI_INTR_HALT          (1 << 12)

// PORTSC bits
#define PORTSC_CONN             (1 << 0)
#define PORTSC_CONN_CHANGE      (1 << 1)
#define PORTSC_ENABLED          (1 << 2)
#define PORTSC_ENABLE_CHANGE    (1 << 3)
#define PORTSC_OVERCUR          (1 << 4)
#define PORTSC_OVERCUR_CHANGE   (1 << 5)
#define PORSTSC_RESUME          (1 << 6)
#define PORTSC_SUSPEND          (1 << 7)
#define PORTSC_RESET            (1 << 8)
#define PORTSC_LINESTATE_K      (1 << 10)
#define PORTSC_LINESTATE_J      (1 << 11)
#define PORTSC_POWER            (1 << 12)
#define PORTSC_OWNER            (1 << 13)
#define PORTSC_CHANGE_MASK      0x2A

// CONFIGFLAG bits
#define CONFIGFLAG_ROUTE_EHC    (1 << 0)

// Link termination
#define EHCI_LINK_TERMINATE     0x01

// Max ports
#define EHCI_MAX_PORTS          16

// QH/QTD constants
#define EHC_FRAME_LEN           1024
#define EHC_TYPE_QH             0x02
#define QH_NAK_RELOAD           3
#define QTD_MAX_ERR             3
#define QTD_MAX_BUFFER          5
#define QTD_BUF_LEN             4096
#define QTD_BUF_MASK            0x0FFF

// PID values
#define QTD_PID_OUTPUT          0x00
#define QTD_PID_INPUT           0x01
#define QTD_PID_SETUP           0x02

// QTD Status bits
#define QTD_STAT_DO_PING        (1 << 0)
#define QTD_STAT_DO_SS          (1 << 1)
#define QTD_STAT_TRANS_ERR      (1 << 3)
#define QTD_STAT_BABBLE_ERR     (1 << 4)
#define QTD_STAT_BUFF_ERR       (1 << 5)
#define QTD_STAT_HALTED         (1 << 6)
#define QTD_STAT_ACTIVE         (1 << 7)
#define QTD_STAT_ERR_MASK       (QTD_STAT_TRANS_ERR | QTD_STAT_BABBLE_ERR | QTD_STAT_BUFF_ERR)

// S-Mask/C-Mask for interrupt transfers (microframe bits)
#define QH_MICROFRAME_0         0x01
#define QH_MICROFRAME_1         0x02
#define QH_MICROFRAME_2         0x04
#define QH_MICROFRAME_3         0x08
#define QH_MICROFRAME_4         0x10
#define QH_MICROFRAME_5         0x20
#define QH_MICROFRAME_6         0x40
#define QH_MICROFRAME_7         0x80

// Transfer types
#define EHC_CTRL_TRANSFER       0x01
#define EHC_BULK_TRANSFER       0x02
#define EHC_INT_TRANSFER_SYNC   0x04
#define EHC_INT_TRANSFER_ASYNC  0x08

// Timeouts
#define EHC_1_MICROSECOND       1
#define EHC_1_MILLISECOND       (1000 * EHC_1_MICROSECOND)
#define EHC_RESET_TIMEOUT       (1 * 1000 * 1000)      // 1 second
#define EHC_GENERIC_TIMEOUT     (10 * 1000)            // 10 ms
#define EHC_ROOT_PORT_RECOVERY_STALL (20 * 1000)      // 20 ms

// Helpers
#define EHC_LOW_32BIT(Addr)     ((UINT32)((UINTN)(Addr) & 0xFFFFFFFF))
#define EHC_HIGH_32BIT(Addr)    ((UINT32)((UINT64)(UINTN)(Addr) >> 32))
#define EHC_BIT_IS_SET(Data, Bit) (((Data) & (Bit)) != 0)

#define QH_LINK(Addr, Type, Term) \
    ((UINT32)((EHC_LOW_32BIT(Addr) & 0xFFFFFFE0) | (Type) | ((Term) ? 1 : 0)))

#define QTD_LINK(Addr, Term)    QH_LINK((Addr), 0, (Term))

#define EHCI_IS_DATAIN(EpAddr)  (((EpAddr) & 0x80) != 0)

// Hardware structures (must be 32-byte aligned!)
typedef struct ATTRIBUTE(packed) ATTRIBUTE(aligned(32)) {
    UINT32 NextQtd;
    UINT32 AltNext;
    
    UINT32 Status     : 8;
    UINT32 Pid        : 2;
    UINT32 ErrCnt     : 2;
    UINT32 CurPage    : 3;
    UINT32 Ioc        : 1;
    UINT32 TotalBytes : 15;
    UINT32 DataToggle : 1;
    
    UINT32 Page[5];
    UINT32 PageHigh[5];
} EhciQtdHw;

typedef struct ATTRIBUTE(packed) ATTRIBUTE(aligned(32)) {
    UINT32 HorizonLink;
    
    UINT32 DeviceAddr   : 7;
    UINT32 Inactive     : 1;
    UINT32 EpNum        : 4;
    UINT32 EpSpeed      : 2;
    UINT32 DtCtrl       : 1;
    UINT32 ReclaimHead  : 1;
    UINT32 MaxPacketLen : 11;
    UINT32 CtrlEp       : 1;
    UINT32 NakReload    : 4;
    
    UINT32 SMask        : 8;
    UINT32 CMask        : 8;
    UINT32 HubAddr      : 7;
    UINT32 PortNum      : 7;
    UINT32 Multiplier   : 2;
    
    UINT32 CurQtd;
    UINT32 NextQtd;
    UINT32 AltQtd;
    
    UINT32 Status     : 8;
    UINT32 Pid        : 2;
    UINT32 ErrCnt     : 2;
    UINT32 CurPage    : 3;
    UINT32 Ioc        : 1;
    UINT32 TotalBytes : 15;
    UINT32 DataToggle : 1;
    
    UINT32 Page[5];
    UINT32 PageHigh[5];
} EhciQhHw;

// Forward declarations
typedef struct EhciQtd EhciQtd;
typedef struct EhciQh EhciQh;
typedef struct EhciUrb EhciUrb;
typedef struct EhciContext EhciContext;

// Software QTD
struct EhciQtd {
    EhciQtdHw Hw;
    UINT32 Signature;
    ListHead Link;
    UINT8 *Data;
    UINTN DataLen;
};

// Software QH
struct EhciQh {
    EhciQhHw Hw;
    UINT32 Signature;
    EhciQh *NextQh;
    ListHead Qtds;
    UINTN Interval;
};

// USB Endpoint info
typedef struct {
    UINT8 DevAddr;
    UINT8 EpAddr;
    UINT8 Direction;
    UINT8 DevSpeed;
    UINTN MaxPacket;
    UINT8 HubAddr;
    UINT8 HubPort;
    UINT8 Toggle;
    UINTN Type;
    UINTN PollRate;
} EhciEndpoint;

// URB structure
struct EhciUrb {
    UINT32 Signature;
    ListHead Link;
    
    EhciEndpoint Ep;
    UsbSetupPacket *Request;
    NOPTR *RequestPhy;
    NOPTR *RequestMap;
    UINT8 *Data;
    UINTN DataLen;
    NOPTR *DataPhy;
    NOPTR *DataMap;
    
    EhciQh *Qh;
    
    UINT32 Result;
    UINTN Completed;
    UINT8 DataToggle;
    
    NOPTR (*Callback)(UINT8 *Data, UINTN Length, NOPTR *Context, UINT32 Result);
    NOPTR *Context;
};

// Port info structure
typedef struct {
    UINT32 Num;
    BOOL Connected;
    BOOL Enabled;
    UINT8 Speed;
    UsbDevice *Device;
} EhciPort;

// EHCI Context
struct EhciContext {
    UsbHcd *Hcd;
    PciDevice *PciDev;
    
    volatile UINT8 *CapRegs;
    volatile UINT8 *OpRegs;
    
    UINT32 CapLength;
    UINT32 HcSParams;
    UINT32 HcCParams;
    UINT32 Ports;
    UINT32 CompanionPorts;
    UINT32 DebugPortNum;
    BOOL Support64BitDma;
    BOOL Running;
    
    // Schedules
    UINT32 *PeriodicList;
    UINT32 PeriodicListPhys;
    UINTN *PeriodicListHost;
    EhciQh *PeriodOne;
    EhciQh *ReclaimHead;
    EhciQtd *ShortReadStop;
    
    // Ports
    EhciPort PortsInfo[EHCI_MAX_PORTS];
    
    // Async transfers
    ListHead AsyncIntTransfers;
    EhciUrb *PendingUrb;
    
    // IRQ
    UINT8 Irq;
    UINT32 Gsi;
    UINT8 Vector;
};

// Function prototypes
INT EhciInit(PciDevice *PciDev);
NOPTR EhciProbeAll(NOPTR);
NOPTR EhciIrqHandler(NOPTR);