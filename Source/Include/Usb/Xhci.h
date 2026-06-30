// ============================================================================
// Xhci.h - xHCI драйвер для TOS (адаптирован из EDK2)
// ============================================================================

#pragma once

#include <Usb/Usb.h>
#include <Kernel/Types.h>
#include <Pci.h>
#include <Kernel/SpinLock.h>

// ============================================================================
// PCI IDs
// ============================================================================

#define XHCI_PCI_CLASS          0x0C
#define XHCI_PCI_SUBCLASS       0x03
#define XHCI_PCI_PROG_IF        0x30

// ============================================================================
// Capability Registers Offset
// ============================================================================

#define XHC_CAPLENGTH_OFFSET    0x00
#define XHC_HCSPARAMS1_OFFSET   0x04
#define XHC_HCSPARAMS2_OFFSET   0x08
#define XHC_HCCPARAMS_OFFSET    0x10
#define XHC_DBOFF_OFFSET        0x14
#define XHC_RTSOFF_OFFSET       0x18

// ============================================================================
// Operational Registers Offset
// ============================================================================

#define XHC_USBCMD_OFFSET       0x0000
#define XHC_USBSTS_OFFSET       0x0004
#define XHC_PAGESIZE_OFFSET     0x0008
#define XHC_DNCTRL_OFFSET       0x0014
#define XHC_CRCR_OFFSET         0x0018
#define XHC_DCBAAP_OFFSET       0x0030
#define XHC_CONFIG_OFFSET       0x0038
#define XHC_PORTSC_OFFSET       0x0400
#define XHC_USBINTR_OFFSET      0x000C

// ============================================================================
// Runtime Registers Offset
// ============================================================================

#define XHC_MFINDEX_OFFSET      0x00
#define XHC_IMAN_OFFSET         0x20
#define XHC_IMOD_OFFSET         0x24
#define XHC_ERSTSZ_OFFSET       0x28
#define XHC_ERSTBA_OFFSET       0x30
#define XHC_ERDP_OFFSET         0x38

// ============================================================================
// USBCMD Bits
// ============================================================================

#define XHC_USBCMD_RUN          BIT0
#define XHC_USBCMD_RESET        BIT1
#define XHC_USBCMD_INTE         BIT2
#define XHC_USBCMD_HSEE         BIT3

// ============================================================================
// USBSTS Bits
// ============================================================================

#define XHC_USBSTS_HALT         BIT0
#define XHC_USBSTS_HSE          BIT2
#define XHC_USBSTS_EINT         BIT3
#define XHC_USBSTS_PCD          BIT4
#define XHC_USBSTS_CNR          BIT11

// ============================================================================
// CRCR Bits
// ============================================================================

#define XHC_CRCR_RCS            BIT0
#define XHC_CRCR_CA             BIT2
#define XHC_CRCR_CRR            BIT3

// ============================================================================
// PORTSC Bits
// ============================================================================

#define XHC_PORTSC_CCS          BIT0      // Current Connect Status
#define XHC_PORTSC_PED          BIT1      // Port Enabled/Disabled
#define XHC_PORTSC_OCA          BIT3      // Over-current Active
#define XHC_PORTSC_RESET        BIT4      // Port Reset
#define XHC_PORTSC_PLS          (BIT5|BIT6|BIT7|BIT8)  // Port Link State
#define XHC_PORTSC_PP           BIT9      // Port Power
#define XHC_PORTSC_PS           (BIT10|BIT11|BIT12|BIT13) // Port Speed
#define XHC_PORTSC_LWS          BIT16     // Port Link State Write Strobe
#define XHC_PORTSC_CSC          BIT17     // Connect Status Change
#define XHC_PORTSC_PEC          BIT18     // Port Enabled/Disabled Change
#define XHC_PORTSC_OCC          BIT20     // Over-Current Change
#define XHC_PORTSC_PRC          BIT21     // Port Reset Change

// Port Speed values
#define XHC_PORT_SPEED_FULL     1
#define XHC_PORT_SPEED_LOW      2
#define XHC_PORT_SPEED_HIGH     3
#define XHC_PORT_SPEED_SUPER    4
#define XHC_PORT_SPEED_SUPER_PLUS 5

// ============================================================================
// TRB Types
// ============================================================================

#define TRB_TYPE_NORMAL                 1
#define TRB_TYPE_SETUP_STAGE            2
#define TRB_TYPE_DATA_STAGE             3
#define TRB_TYPE_STATUS_STAGE           4
#define TRB_TYPE_LINK                   6
#define TRB_TYPE_EN_SLOT                9
#define TRB_TYPE_DIS_SLOT               10
#define TRB_TYPE_ADDRESS_DEV            11
#define TRB_TYPE_CON_ENDPOINT           12
#define TRB_TYPE_EVALU_CONTXT           13
#define TRB_TYPE_RESET_ENDPOINT         14
#define TRB_TYPE_STOP_ENDPOINT          15
#define TRB_TYPE_SET_TR_DEQUE           16
#define TRB_TYPE_TRANS_EVENT            32
#define TRB_TYPE_COMMAND_COMPLT_EVENT   33
#define TRB_TYPE_PORT_STATUS_CHANGE_EVENT 34

// ============================================================================
// TRB Completion Codes
// ============================================================================

#define TRB_COMPLETION_SUCCESS                  1
#define TRB_COMPLETION_DATA_BUFFER_ERROR        2
#define TRB_COMPLETION_BABBLE_ERROR             3
#define TRB_COMPLETION_USB_TRANSACTION_ERROR    4
#define TRB_COMPLETION_TRB_ERROR                5
#define TRB_COMPLETION_STALL_ERROR              6
#define TRB_COMPLETION_SHORT_PACKET             13
#define TRB_COMPLETION_STOPPED                  26
#define TRB_COMPLETION_STOPPED_LENGTH_INVALID   27

// ============================================================================
// Endpoint Types
// ============================================================================

#define ED_CONTROL_BIDIR    4
#define ED_BULK_OUT         2
#define ED_BULK_IN          6
#define ED_INTERRUPT_OUT    3
#define ED_INTERRUPT_IN     7

// ============================================================================
// Ring Sizes
// ============================================================================

#define CMD_RING_TRB_NUMBER     256
#define TR_RING_TRB_NUMBER      256
#define EVENT_RING_TRB_NUMBER   512
#define ERST_NUMBER             1

// ============================================================================
// Timeouts (in milliseconds)
// ============================================================================

#define XHC_GENERIC_TIMEOUT     10000
#define XHC_RESET_TIMEOUT       1000
#define XHC_RESET_RECOVERY_DELAY 10  // 10ms delay after reset

// ============================================================================
// DMA Alignment
// ============================================================================

#define XHC_DMA_ALIGNMENT       64
#define XHC_TRB_ALIGNMENT       16

#ifndef BIT0
#define BIT0   (1 << 0)
#define BIT1   (1 << 1)
#define BIT2   (1 << 2)
#define BIT3   (1 << 3)
#define BIT4   (1 << 4)
#define BIT5   (1 << 5)
#define BIT6   (1 << 6)
#define BIT7   (1 << 7)
#define BIT8   (1 << 8)
#define BIT9   (1 << 9)
#define BIT10  (1 << 10)
#define BIT11  (1 << 11)
#define BIT12  (1 << 12)
#define BIT13  (1 << 13)
#define BIT14  (1 << 14)
#define BIT15  (1 << 15)
#define BIT16  (1 << 16)
#define BIT17  (1 << 17)
#define BIT18  (1 << 18)
#define BIT19  (1 << 19)
#define BIT20  (1 << 20)
#define BIT21  (1 << 21)
#define BIT22  (1 << 22)
#define BIT23  (1 << 23)
#define BIT24  (1 << 24)
#define BIT25  (1 << 25)
#define BIT26  (1 << 26)
#define BIT27  (1 << 27)
#define BIT28  (1 << 28)
#define BIT29  (1 << 29)
#define BIT30  (1 << 30)
#define BIT31  (1 << 31)
#endif

// ============================================================================
// Structures
// ============================================================================

#pragma pack(1)

// TRB Template
typedef struct {
    UINT32 Parameter1;
    UINT32 Parameter2;
    UINT32 Status;
    UINT32 CycleBit : 1;
    UINT32 RsvdZ1   : 9;
    UINT32 Type     : 6;
    UINT32 Control  : 16;
} TRB_TEMPLATE;

// Slot Context
typedef struct {
    UINT32 RouteString    : 20;
    UINT32 Speed          : 4;
    UINT32 RsvdZ1         : 1;
    UINT32 MTT            : 1;
    UINT32 Hub            : 1;
    UINT32 ContextEntries : 5;
    UINT32 MaxExitLatency : 16;
    UINT32 RootHubPortNum : 8;
    UINT32 PortNum        : 8;
    UINT32 TTHubSlotId    : 8;
    UINT32 TTPortNum      : 8;
    UINT32 TTT            : 2;
    UINT32 RsvdZ2         : 4;
    UINT32 InterTarget    : 10;
    UINT32 DeviceAddress  : 8;
    UINT32 RsvdZ3         : 19;
    UINT32 SlotState      : 5;
    UINT32 RsvdZ4[4];
} SLOT_CONTEXT;

// Endpoint Context
typedef struct {
    UINT32 EPState          : 3;
    UINT32 RsvdZ1           : 5;
    UINT32 Mult             : 2;
    UINT32 MaxPStreams      : 5;
    UINT32 LSA              : 1;
    UINT32 Interval         : 8;
    UINT32 RsvdZ2           : 8;
    UINT32 RsvdZ3           : 1;
    UINT32 CErr             : 2;
    UINT32 EPType           : 3;
    UINT32 RsvdZ4           : 1;
    UINT32 HID              : 1;
    UINT32 MaxBurstSize     : 8;
    UINT32 MaxPacketSize    : 16;
    UINT32 PtrLo;
    UINT32 PtrHi;
    UINT32 AverageTRBLength : 16;
    UINT32 MaxESITPayload   : 16;
    UINT32 RsvdZ5[4];
} ENDPOINT_CONTEXT;

// Input Control Context
typedef struct {
    UINT32 Dword1;
    UINT32 Dword2;
    UINT32 RsvdZ1[6];
} INPUT_CONTROL_CONTEXT;

// Input Context
typedef struct {
    INPUT_CONTROL_CONTEXT InputControlContext;
    SLOT_CONTEXT Slot;
    ENDPOINT_CONTEXT EP[31];
} INPUT_CONTEXT;

// Device Context
typedef struct {
    SLOT_CONTEXT Slot;
    ENDPOINT_CONTEXT EP[31];
} DEVICE_CONTEXT;

// Event Ring Segment Table Entry
typedef struct {
    UINT32 PtrLo;
    UINT32 PtrHi;
    UINT32 RingTrbSize : 16;
    UINT32 RsvdZ1      : 16;
    UINT32 RsvdZ2;
} EVENT_RING_SEG_TABLE_ENTRY;

// Transfer Ring
typedef struct {
    TRB_TEMPLATE *RingSeg0;
    UINTN TrbNumber;
    TRB_TEMPLATE *RingEnqueue;
    TRB_TEMPLATE *RingDequeue;
    UINT32 RingPCS;
} TRANSFER_RING;

// Event Ring
typedef struct {
    EVENT_RING_SEG_TABLE_ENTRY *ERSTBase;
    TRB_TEMPLATE *EventRingSeg0;
    UINTN TrbNumber;
    TRB_TEMPLATE *EventRingEnqueue;
    TRB_TEMPLATE *EventRingDequeue;
    UINT32 EventRingCCS;
} EVENT_RING;

// Command TRBs
typedef struct {
    UINT32 RsvdZ0;
    UINT32 RsvdZ1;
    UINT32 RsvdZ2;
    UINT32 CycleBit : 1;
    UINT32 RsvdZ3   : 9;
    UINT32 Type     : 6;
    UINT32 RsvdZ4   : 16;
} CMD_ENABLE_SLOT;

typedef struct {
    UINT32 RsvdZ0;
    UINT32 RsvdZ1;
    UINT32 RsvdZ2;
    UINT32 CycleBit : 1;
    UINT32 RsvdZ3   : 9;
    UINT32 Type     : 6;
    UINT32 RsvdZ4   : 8;
    UINT32 SlotId   : 8;
} CMD_DISABLE_SLOT;

typedef struct {
    UINT32 PtrLo;
    UINT32 PtrHi;
    UINT32 RsvdZ1;
    UINT32 CycleBit : 1;
    UINT32 RsvdZ2   : 8;
    UINT32 BSR      : 1;
    UINT32 Type     : 6;
    UINT32 RsvdZ3   : 8;
    UINT32 SlotId   : 8;
} CMD_ADDRESS_DEVICE;

typedef struct {
    UINT32 PtrLo;
    UINT32 PtrHi;
    UINT32 RsvdZ1;
    UINT32 CycleBit : 1;
    UINT32 RsvdZ2   : 8;
    UINT32 DC       : 1;
    UINT32 Type     : 6;
    UINT32 RsvdZ3   : 8;
    UINT32 SlotId   : 8;
} CMD_CONFIG_ENDPOINT;

typedef struct {
    UINT32 RsvdZ0;
    UINT32 RsvdZ1;
    UINT32 RsvdZ2;
    UINT32 CycleBit : 1;
    UINT32 RsvdZ3   : 8;
    UINT32 TSP      : 1;
    UINT32 Type     : 6;
    UINT32 EDID     : 5;
    UINT32 RsvdZ4   : 3;
    UINT32 SlotId   : 8;
} CMD_RESET_ENDPOINT;

typedef struct {
    UINT32 RsvdZ0;
    UINT32 RsvdZ1;
    UINT32 RsvdZ2;
    UINT32 CycleBit : 1;
    UINT32 RsvdZ3   : 9;
    UINT32 Type     : 6;
    UINT32 EDID     : 5;
    UINT32 RsvdZ4   : 2;
    UINT32 SP       : 1;
    UINT32 SlotId   : 8;
} CMD_STOP_ENDPOINT;

typedef struct {
    UINT32 PtrLo;
    UINT32 PtrHi;
    UINT32 RsvdZ1   : 16;
    UINT32 StreamID : 16;
    UINT32 CycleBit : 1;
    UINT32 RsvdZ2   : 9;
    UINT32 Type     : 6;
    UINT32 Endpoint : 5;
    UINT32 RsvdZ3   : 3;
    UINT32 SlotId   : 8;
} CMD_SET_TR_DEQUEUE;

// Transfer TRBs
typedef struct {
    UINT32 TRBPtrLo;
    UINT32 TRBPtrHi;
    UINT32 Length    : 17;
    UINT32 TDSize    : 5;
    UINT32 IntTarget : 10;
    UINT32 CycleBit  : 1;
    UINT32 ENT       : 1;
    UINT32 ISP       : 1;
    UINT32 NS        : 1;
    UINT32 CH        : 1;
    UINT32 IOC       : 1;
    UINT32 IDT       : 1;
    UINT32 RsvdZ1    : 2;
    UINT32 BEI       : 1;
    UINT32 Type      : 6;
    UINT32 RsvdZ2    : 16;
} TRB_NORMAL;

typedef struct {
    UINT32 BmRequestType : 8;
    UINT32 BRequest      : 8;
    UINT32 WValue        : 16;
    UINT32 WIndex        : 16;
    UINT32 WLength       : 16;
    UINT32 Length        : 17;
    UINT32 RsvdZ1        : 5;
    UINT32 IntTarget     : 10;
    UINT32 CycleBit      : 1;
    UINT32 RsvdZ2        : 4;
    UINT32 IOC           : 1;
    UINT32 IDT           : 1;
    UINT32 RsvdZ3        : 3;
    UINT32 Type          : 6;
    UINT32 TRT           : 2;
    UINT32 RsvdZ4        : 14;
} TRB_SETUP;

typedef struct {
    UINT32 TRBPtrLo;
    UINT32 TRBPtrHi;
    UINT32 Length    : 17;
    UINT32 TDSize    : 5;
    UINT32 IntTarget : 10;
    UINT32 CycleBit  : 1;
    UINT32 ENT       : 1;
    UINT32 ISP       : 1;
    UINT32 NS        : 1;
    UINT32 CH        : 1;
    UINT32 IOC       : 1;
    UINT32 IDT       : 1;
    UINT32 RsvdZ1    : 3;
    UINT32 Type      : 6;
    UINT32 DIR       : 1;
    UINT32 RsvdZ2    : 15;
} TRB_DATA;

typedef struct {
    UINT32 RsvdZ1;
    UINT32 RsvdZ2;
    UINT32 RsvdZ3    : 22;
    UINT32 IntTarget : 10;
    UINT32 CycleBit  : 1;
    UINT32 ENT       : 1;
    UINT32 RsvdZ4    : 2;
    UINT32 CH        : 1;
    UINT32 IOC       : 1;
    UINT32 RsvdZ5    : 4;
    UINT32 Type      : 6;
    UINT32 DIR       : 1;
    UINT32 RsvdZ6    : 15;
} TRB_STATUS;

typedef struct {
    UINT32 PtrLo;
    UINT32 PtrHi;
    UINT32 RsvdZ1      : 22;
    UINT32 InterTarget : 10;
    UINT32 CycleBit    : 1;
    UINT32 TC          : 1;
    UINT32 RsvdZ2      : 2;
    UINT32 CH          : 1;
    UINT32 IOC         : 1;
    UINT32 RsvdZ3      : 4;
    UINT32 Type        : 6;
    UINT32 RsvdZ4      : 16;
} TRB_LINK;

// Event TRBs
typedef struct {
    UINT32 TRBPtrLo;
    UINT32 TRBPtrHi;
    UINT32 Length       : 24;
    UINT32 Completecode : 8;
    UINT32 CycleBit     : 1;
    UINT32 RsvdZ1       : 1;
    UINT32 ED           : 1;
    UINT32 RsvdZ2       : 7;
    UINT32 Type         : 6;
    UINT32 EndpointId   : 5;
    UINT32 RsvdZ3       : 3;
    UINT32 SlotId       : 8;
} EVT_TRANSFER;

typedef struct {
    UINT32 TRBPtrLo;
    UINT32 TRBPtrHi;
    UINT32 RsvdZ2       : 24;
    UINT32 Completecode : 8;
    UINT32 CycleBit     : 1;
    UINT32 RsvdZ3       : 9;
    UINT32 Type         : 6;
    UINT32 VFID         : 8;
    UINT32 SlotId       : 8;
} EVT_COMMAND_COMPLETE;

#pragma pack()

// ============================================================================
// URB (USB Request Block)
// ============================================================================

typedef struct URB {
    // Endpoint info
    UINT8 BusAddr;
    UINT8 EpAddr;
    UINT8 Direction;
    UINT8 DevSpeed;
    UINTN MaxPacket;
    UINTN Type;
    
    // Transfer data
    UsbSetupPacket *Request;
    UINT8 *Data;
    UINTN DataLen;
    UINTN Completed;
    UINT32 Result;
    BOOL Finished;
    
    // TRB management
    TRANSFER_RING *Ring;
    TRB_TEMPLATE *TrbStart;
    TRB_TEMPLATE *TrbEnd;
    UINTN TrbNum;
    BOOL StartDone;
    BOOL EndDone;
    
    NOPTR *DataPhys;
    NOPTR *DataMap;
    
    struct URB *Next;
} URB;

// ============================================================================
// Device Slot
// ============================================================================

typedef struct {
    BOOL Enabled;
    UINT8 SlotId;
    UINT32 RouteString;
    UINT32 ParentRouteString;
    UINT8 XhciDevAddr;
    UINT8 BusDevAddr;
    INPUT_CONTEXT *InputContext;
    DEVICE_CONTEXT *OutputContext;
    TRANSFER_RING *EndpointTransferRing[31];
    UsbDevice *UsbDevice;
} DEVICE_SLOT;

// ============================================================================
// xHCI Context
// ============================================================================

typedef struct {
    UsbHcd *Hcd;
    PciDevice *PciDev;
    
    // MMIO
    volatile UINT8 *CapRegs;
    volatile UINT8 *OpRegs;
    volatile UINT8 *Doorbell;
    volatile UINT8 *RuntimeRegs;
    
    // Capability registers
    UINT8 CapLength;
    UINT32 HcSParams1;
    UINT32 HcSParams2;
    UINT32 HcCParams;
    UINT32 DBOff;
    UINT32 RTSOff;
    UINT32 PageSize;
    
    // Extended capabilities
    UINT32 UsbLegSupOffset;
    UINT32 Usb2SupOffset;
    UINT32 Usb3SupOffset;
    
    // Configuration
    UINT32 MaxSlotsEn;
    UINT64 *DCBAA;
    UINT32 MaxScratchpadBufs;
    UINT64 *ScratchpadBufArray;
    
    // Rings
    TRANSFER_RING CmdRing;
    EVENT_RING EventRing;
    
    // Device slots
    DEVICE_SLOT Slots[256];
    
    // IRQ
    UINT8 Irq;
    UINT32 Gsi;
    UINT8 Vector;
    BOOL Running;
    BOOL Support64BitDma;
    BOOL MsiEnabled;
    
    SpinLock Lock;
} XhciContext;

// ============================================================================
// Function Prototypes
// ============================================================================

// Initialization
INT XhciInit(PciDevice *PciDev);
NOPTR XhciProbeAll(NOPTR);

// IRQ Handler
NOPTR XhciIrqHandler(NOPTR);

// ============================================================================
// Inline Helpers
// ============================================================================

static inline BOOL XHCI_IS_DATAIN(UINT8 EpAddr) {
    return (EpAddr & 0x80) != 0;
}

static inline UINT8 XhciEndpointToDci(UINT8 EpAddr, UINT8 Direction) {
    if (EpAddr == 0) return 1;
    return (EpAddr * 2) + (Direction == USB_DIR_IN ? 1 : 0);
}