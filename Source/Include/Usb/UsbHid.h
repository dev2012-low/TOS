#pragma once

#include <Usb/Usb.h>

// HID Requests
#define USB_HID_GET_REPORT   0x01
#define USB_HID_GET_IDLE     0x02
#define USB_HID_GET_PROTOCOL 0x03
#define USB_HID_SET_REPORT   0x09
#define USB_HID_SET_IDLE     0x0A
#define USB_HID_SET_PROTOCOL 0x0B

// HID Descriptor Types
#define USB_DESC_HID         0x21
#define USB_DESC_REPORT      0x22
#define USB_DESC_PHYSICAL    0x23

typedef struct {
    UINT8  BLength;
    UINT8  BDescriptorType;
    UINT16 BcdHID;
    UINT8  BCountryCode;
    UINT8  BNumDescriptors;
    UINT8  BReportDescriptorType;
    UINT16 WDescriptorLength;
} ATTRIBUTE(packed) UsbHidDescriptor;

typedef struct {
    UsbDevice *Dev;
    UsbTransfer *IntInTransfer;
    UINT8 *Buffer;
    UINT32 BufferLen;
    BOOL IsKeyboard;
    UINT8 PrevBuffer[8];
} UsbHidDevice;

void UsbHidDriverInit();
