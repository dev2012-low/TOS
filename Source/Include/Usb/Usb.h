#pragma once

#include <Kernel/Types.h>
#include <Kernel/List.h>

// ==================== USB CONSTANTS ====================

#define USB_1_0         0x0100
#define USB_1_1         0x0110
#define USB_2_0         0x0200
#define USB_3_0         0x0300

#define USB_SPEED_LOW       0
#define USB_SPEED_FULL      1
#define USB_SPEED_HIGH      2
#define USB_SPEED_SUPER     3

#define USB_TRANSFER_CONTROL      0
#define USB_TRANSFER_ISOCHRONOUS  1
#define USB_TRANSFER_BULK         2
#define USB_TRANSFER_INTERRUPT    3

#define USB_DIR_OUT        0
#define USB_DIR_IN         1

#define USB_ENDPOINT_CONTROL    0
#define USB_ENDPOINT_ISOCH      1
#define USB_ENDPOINT_BULK       2
#define USB_ENDPOINT_INTERRUPT  3

#define USB_CLASS_AUDIO         0x01
#define USB_CLASS_COMM          0x02
#define USB_CLASS_HID           0x03
#define USB_CLASS_PHYSICAL      0x05
#define USB_CLASS_IMAGE         0x06
#define USB_CLASS_PRINTER       0x07
#define USB_CLASS_MASS_STORAGE  0x08
#define USB_CLASS_HUB           0x09
#define USB_CLASS_CDC_DATA      0x0A
#define USB_CLASS_SMART_CARD    0x0B
#define USB_CLASS_CONTENT_SEC   0x0D
#define USB_CLASS_VIDEO         0x0E
#define USB_CLASS_PERSONAL_HC   0x0F
#define USB_CLASS_VENDOR_SPEC   0xFF

#define USB_HID_SUBCLASS_BOOT   0x01
#define USB_HID_PROTOCOL_KEYBOARD 0x01
#define USB_HID_PROTOCOL_MOUSE    0x02

#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_CLEAR_FEATURE       0x01
#define USB_REQ_SET_FEATURE         0x03
#define USB_REQ_SET_ADDRESS         0x05
#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_DESCRIPTOR      0x07
#define USB_REQ_GET_CONFIGURATION   0x08
#define USB_REQ_SET_CONFIGURATION   0x09
#define USB_REQ_GET_INTERFACE       0x0A
#define USB_REQ_SET_INTERFACE       0x0B
#define USB_REQ_SYNCH_FRAME         0x0C

#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIGURATION  0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_DEVICE_QUALIFIER 0x06
#define USB_DESC_OTHER_SPEED    0x07
#define USB_DESC_INTERFACE_POWER 0x08
#define USB_DESC_HUB            0x29

#define USB_RECIP_DEVICE       0x00
#define USB_RECIP_INTERFACE    0x01
#define USB_RECIP_ENDPOINT     0x02
#define USB_RECIP_OTHER        0x03

#define USB_FEAT_ENDPOINT_HALT  0
#define USB_FEAT_DEVICE_REMOTE_WAKEUP 1
#define USB_FEAT_ENDPOINT_STALL 0

#define USB_MAX_ENDPOINTS      16

// Mass storage subclass/protocol
#define USB_MS_SUBCLASS_SCSI   0x06
#define USB_MS_PROTO_BOT       0x50

#define USB_NOERROR         0
#define USB_ERR_STALL       1
#define USB_ERR_BABBLE      2
#define USB_ERR_BUFFER      3
#define USB_ERR_TIMEOUT     4
#define USB_ERR_SYSTEM      5

// ==================== USB DESCRIPTORS ====================

typedef struct {
    UINT8 BLength;
    UINT8 BDescriptorType;
    UINT16 BcdUSB;
    UINT8 BDeviceClass;
    UINT8 BDeviceSubClass;
    UINT8 BDeviceProtocol;
    UINT8 BMaxPacketSize0;
    UINT16 IdVendor;
    UINT16 IdProduct;
    UINT16 BcdDevice;
    UINT8 IManufacturer;
    UINT8 IProduct;
    UINT8 ISerialNumber;
    UINT8 BNumConfigurations;
} ATTRIBUTE(packed) UsbDeviceDescriptor;

typedef struct {
    UINT8 BLength;
    UINT8 BDescriptorType;
    UINT16 WTotalLength;
    UINT8 BNumInterfaces;
    UINT8 BConfigurationValue;
    UINT8 IConfiguration;
    UINT8 BmAttributes;
    UINT8 BMaxPower;
} ATTRIBUTE(packed) UsbConfigDescriptor;

typedef struct {
    UINT8 BLength;
    UINT8 BDescriptorType;
    UINT8 BInterfaceNumber;
    UINT8 BAlternateSetting;
    UINT8 BNumEndpoints;
    UINT8 BInterfaceClass;
    UINT8 BInterfaceSubClass;
    UINT8 BInterfaceProtocol;
    UINT8 IInterface;
} ATTRIBUTE(packed) UsbInterfaceDescriptor;

typedef struct {
    UINT8 BLength;
    UINT8 BDescriptorType;
    UINT8 BEndpointAddress;
    UINT8 BmAttributes;
    UINT16 WMaxPacketSize;
    UINT8 BInterval;
} ATTRIBUTE(packed) UsbEndpointDescriptor;

typedef struct {
    UINT8 BLength;
    UINT8 BDescriptorType;
    UINT8 BNbPorts;
    UINT16 WHubCharacteristics;
    UINT8 BPowerOnToPowerGood;
    UINT8 BHubContrCurrent;
    UINT8 DeviceRemovable[16];
} ATTRIBUTE(packed) UsbHubDescriptor;

typedef struct ATTRIBUTE(packed) {
    UINT8  BmRequestType;
    UINT8  BRequest;
    UINT16 WValue;
    UINT16 WIndex;
    UINT16 WLength;
} UsbSetupPacket;

typedef UsbSetupPacket USB_DEVICE_REQUEST;

// ==================== USB TRANSFER ====================

typedef struct UsbTransfer {
    UINT8 Endpoint;
    UINT8 Direction;
    UINT8 Type;
    UINT32 Length;
    UINT8 *Buffer;
    UINT32 TimeoutMs;
    volatile INT Status;
    INT ActualLength;
    NOPTR (*Callback)(struct UsbTransfer *Transfer);
    NOPTR *Context;
    struct ListHead Node;
    struct UsbDevice *Device;
} UsbTransfer;

// ==================== USB DEVICE ====================

struct UsbDriver;

typedef struct UsbDevice {
    struct UsbHcd *Hcd;
    UINT8 Address;
    UINT8 SlotId;
    UINT8 Speed;
    UINT16 VendorId;
    UINT16 ProductId;
    UINT16 BcdDevice;
    UINT8 DeviceClass;
    UINT8 DeviceSubclass;
    UINT8 DeviceProtocol;
    UINT8 MaxPacketSize0;

    UsbDeviceDescriptor *DeviceDesc;
    UsbConfigDescriptor *ConfigDesc;
    NOPTR *ConfigData;
    UINT32 ConfigLen;

    UINT8 Configuration;
    UINT8 Interface;
    UINT8 AlternateSetting;

    UsbEndpointDescriptor EpDesc[USB_MAX_ENDPOINTS];
    UINT32 EndpointCount;

    struct UsbDriver *Driver;
    NOPTR *DriverData;

    struct UsbDevice *Parent;
    UINT8 HubPort;
    struct ListHead Childs;
    struct ListHead Node;
    struct ListHead HcdNode;
} UsbDevice;

typedef struct UsbEndpoint {
    UINT8 Address;
    UINT8 Attributes;
    UINT16 MaxPacketSize;
    UINT8 Interval;
    UINT8 Type;
    UINT8 Direction;
    UsbDevice *Device;
    struct ListHead TransferQueue;
    BOOL Halted;
} UsbEndpoint;

typedef struct UsbDriver {
    CHAR Name[32];
    UINT16 VendorId;
    UINT16 ProductId;
    UINT8 DeviceClass;
    UINT8 DeviceSubclass;
    UINT8 DeviceProtocol;

    INT (*Probe)(UsbDevice *Dev);
    INT (*Disconnect)(UsbDevice *Dev);
    INT (*Reset)(UsbDevice *Dev);
    INT (*Suspend)(UsbDevice *Dev);
    INT (*Resume)(UsbDevice *Dev);

    struct ListHead Node;
} UsbDriver;

typedef struct UsbHcdOps {
    INT (*Init)(struct UsbHcd *Hcd);
    INT (*Shutdown)(struct UsbHcd *Hcd);
    INT (*Reset)(struct UsbHcd *Hcd);

    INT (*SubmitTransfer)(struct UsbHcd *Hcd, UsbTransfer *Transfer);
    INT (*CancelTransfer)(struct UsbHcd *Hcd, UsbTransfer *Transfer);

    INT (*PortReset)(struct UsbHcd *Hcd, UINT8 Port);
    INT (*EnableSlot)(struct UsbHcd *Hcd, UsbDevice *Dev, UINT8 Port);
    INT (*DisableSlot)(struct UsbHcd *Hcd, UsbDevice *Dev);

    NOPTR (*IrqHandler)(NOPTR);
} UsbHcdOps;

typedef struct UsbHcd {
    CHAR Name[32];
    UINT16 VendorId;
    UINT16 DeviceId;
    UINT32 MmioBase;
    UINT8 Irq;
    UINT32 Gsi;
    UINT8 Vector;
    NOPTR *Regs;

    UsbHcdOps *Ops;
    NOPTR *Private;

    struct ListHead Devices;
    UINT32 DeviceCount;
    UINT8 NextAddress;
    BOOL Running;

    struct ListHead Node;
} UsbHcd;

typedef struct ATTRIBUTE(packed) {
    UINT8  BLength;
    UINT8  BDescriptorType;
    UINT16 BcdUSB;
    UINT8  BDeviceClass;
    UINT8  BDeviceSubClass;
    UINT8  BDeviceProtocol;
    UINT8  BMaxPacketSize0;
    UINT16 IdVendor;
    UINT16 IdProduct;
    UINT16 BcdDevice;
    UINT8  IManufacturer;
    UINT8  IProduct;
    UINT8  ISerialNumber;
    UINT8  BNumConfigurations;
} USB_DEVICE_DESCRIPTOR;

// ==================== USB CORE API ====================

NOPTR UsbInit(NOPTR);
INT UsbHcdRegister(UsbHcd *Hcd);
INT UsbDriverRegister(UsbDriver *Drv);

UsbDevice *UsbDeviceAlloc(UsbHcd *Hcd);
NOPTR UsbDeviceFree(UsbDevice *Dev);
INT UsbDeviceAdd(UsbHcd *Hcd, UsbDevice *Dev);
INT UsbDeviceRemove(UsbDevice *Dev);
UsbDevice *UsbDeviceFind(UINT16 VendorId, UINT16 ProductId);

INT UsbControlTransfer(UsbDevice *Dev, UINT8 RequestType, UINT8 Request,
                       UINT16 Value, UINT16 Index, UINT16 Length, NOPTR *Data);
INT UsbBulkTransfer(UsbDevice *Dev, UINT8 Endpoint, NOPTR *Data, UINT32 Length);

INT UsbGetDescriptor(UsbDevice *Dev, UINT8 Type, UINT8 Index, NOPTR *Buf, UINT16 Len);
INT UsbGetDeviceDescriptor(UsbDevice *Dev, UsbDeviceDescriptor *Desc);
INT UsbGetStringDescriptor(UsbDevice *Dev, UINT8 Index, CHAR *Buf, UINT16 Len);

INT UsbEnumeration(UsbDevice *Dev);
INT UsbSetAddress(UsbDevice *Dev, UINT8 Address);
INT UsbSetConfiguration(UsbDevice *Dev, UINT8 Configuration);
INT UsbParseConfiguration(UsbDevice *Dev, UINT8 *Data, UINT32 Len);
INT UsbBindDriver(UsbDevice *Dev);

INT UsbHubDriverInit(NOPTR);
INT UsbStorageDriverInit(NOPTR);

static inline const CHAR *UsbSpeedString(UINT8 Speed) {
    switch (Speed) {
        case USB_SPEED_LOW: return "Low";
        case USB_SPEED_FULL: return "Full";
        case USB_SPEED_HIGH: return "High";
        case USB_SPEED_SUPER: return "Super";
        default: return "Unknown";
    }
}

static inline const CHAR *UsbClassString(UINT8 Class) {
    switch (Class) {
        case USB_CLASS_AUDIO: return "Audio";
        case USB_CLASS_COMM: return "Communication";
        case USB_CLASS_HID: return "HID";
        case USB_CLASS_MASS_STORAGE: return "Mass Storage";
        case USB_CLASS_HUB: return "Hub";
        case USB_CLASS_PRINTER: return "Printer";
        case USB_CLASS_VENDOR_SPEC: return "Vendor Specific";
        default: return "Unknown";
    }
}
