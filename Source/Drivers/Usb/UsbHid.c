#include <Usb/UsbHid.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Console.h>
#include <Kernel/Return.h>
#include <Ps2Keyboard.h>

// HID Usage ID to PS/2 Scancode (Set 1)
static const UINT8 HidToPs2[] = {
    0, 0, 0, 0, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, // 0x00-0x0F
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C, // 0x10-0x1F
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x1C, 0x01, 0x0E, 0x0F, // 0x20-0x2F
    0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3A, // 0x30-0x3F
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58              // 0x40-0x4B
};

extern NOPTR Ps2HandleScanByte(UINT8 Byte);

static NOPTR UsbHidProcessKeyboard(UsbHidDevice *Hid, UINT8 *Buf) {
    // Handle modifiers (Shift, Ctrl, Alt, GUI)
    UINT8 Mod = Buf[0];
    UINT8 PrevMod = Hid->PrevBuffer[0];
    
    // Простая обработка модификаторов (Left Ctrl, Shift, Alt, GUI)
    UINT8 ModScancodes[] = {0x1D, 0x2A, 0x38, 0x5B, 0x9D, 0x36, 0xB8, 0x5C};
    for (INT I = 0; I < 8; I++) {
        if ((Mod & (1 << I)) && !(PrevMod & (1 << I))) {
            Ps2HandleScanByte(ModScancodes[I]);
        } else if (!(Mod & (1 << I)) && (PrevMod & (1 << I))) {
            Ps2HandleScanByte(ModScancodes[I] | 0x80);
        }
    }

    // Handle keys
    for (INT I = 2; I < 8; I++) {
        UINT8 Key = Buf[I];
        if (Key == 0) continue;

        BOOL Found = FALSE;
        for (INT J = 2; J < 8; J++) {
            if (Hid->PrevBuffer[J] == Key) {
                Found = TRUE;
                break;
            }
        }

        if (!Found) {
            if (Key < sizeof(HidToPs2)) {
                UINT8 Scancode = HidToPs2[Key];
                if (Scancode) Ps2HandleScanByte(Scancode);
            }
        }
    }

    for (INT I = 2; I < 8; I++) {
        UINT8 Key = Hid->PrevBuffer[I];
        if (Key == 0) continue;

        BOOL Found = FALSE;
        for (INT J = 2; J < 8; J++) {
            if (Buf[J] == Key) {
                Found = TRUE;
                break;
            }
        }

        if (!Found) {
            if (Key < sizeof(HidToPs2)) {
                UINT8 Scancode = HidToPs2[Key];
                if (Scancode) Ps2HandleScanByte(Scancode | 0x80);
            }
        }
    }
    
    MemCpy(Hid->PrevBuffer, Buf, 8);
    Ps2KeyboardDispatchEvents();
}

static NOPTR UsbHidCallback(UsbTransfer *Transfer) {
    UsbHidDevice *Hid = (UsbHidDevice *)Transfer->Context;

    if (Transfer->Status == SUCCESS) {
        if (Hid->IsKeyboard) {
            UsbHidProcessKeyboard(Hid, Hid->Buffer);
        }
    }

    Transfer->Status = 0;
    if (Hid->Dev && Hid->Dev->Hcd && Hid->Dev->Hcd->Ops->SubmitTransfer) {
        Hid->Dev->Hcd->Ops->SubmitTransfer(Hid->Dev->Hcd, Transfer);
    }
}

static INT UsbHidProbe(UsbDevice *Dev) {
    // Check if it's a HID device
    if (Dev->DeviceClass != USB_CLASS_HID && Dev->DeviceClass != 0) {
        return INCORRECT_VALUE;
    }

    BOOL IsKbd = (Dev->DeviceProtocol == USB_HID_PROTOCOL_KEYBOARD);
    BOOL IsMouse = (Dev->DeviceProtocol == USB_HID_PROTOCOL_MOUSE);

    if (!IsKbd && !IsMouse) {
        return INCORRECT_VALUE;
    }

    UsbHidDevice *Hid = (UsbHidDevice *)MemoryAllocate(sizeof(UsbHidDevice));
    if (!Hid) return NO_MEMORY;
    
    MemSet(Hid, 0, sizeof(UsbHidDevice));
    Hid->Dev = Dev;
    Hid->IsKeyboard = IsKbd;
    Hid->BufferLen = IsKbd ? 8 : 4;
    Hid->Buffer = (UINT8 *)MemoryAllocate(Hid->BufferLen);
    if (!Hid->Buffer) {
        MemoryFree(Hid);
        return NO_MEMORY;
    }
    Dev->DriverData = Hid;

    // Set Boot Protocol
    UsbControlTransfer(Dev, 0x21, USB_HID_SET_PROTOCOL, 0, Dev->Interface, 0, NULLPTR);
    // Set Idle to 0
    UsbControlTransfer(Dev, 0x21, USB_HID_SET_IDLE, 0, Dev->Interface, 0, NULLPTR);

    Hid->IntInTransfer = (UsbTransfer *)MemoryAllocate(sizeof(UsbTransfer));
    if (!Hid->IntInTransfer) {
        MemoryFree(Hid->Buffer);
        MemoryFree(Hid);
        return NO_MEMORY;
    }
    MemSet(Hid->IntInTransfer, 0, sizeof(UsbTransfer));
    
    Hid->IntInTransfer->Device = Dev;
    Hid->IntInTransfer->Type = USB_TRANSFER_INTERRUPT;
    Hid->IntInTransfer->Direction = USB_DIR_IN;
    Hid->IntInTransfer->Buffer = Hid->Buffer;
    Hid->IntInTransfer->Length = Hid->BufferLen;
    Hid->IntInTransfer->Callback = UsbHidCallback;
    Hid->IntInTransfer->Context = Hid;

    for (UINT32 I = 0; I < Dev->EndpointCount; I++) {
        if ((Dev->EpDesc[I].BEndpointAddress & 0x80) && 
            (Dev->EpDesc[I].BmAttributes & 0x03) == USB_ENDPOINT_INTERRUPT) {
            Hid->IntInTransfer->Endpoint = Dev->EpDesc[I].BEndpointAddress & 0x0F;
            break;
        }
    }

    if (Hid->IntInTransfer->Endpoint == 0) {
        MemoryFree(Hid->IntInTransfer);
        MemoryFree(Hid->Buffer);
        MemoryFree(Hid);
        return INCORRECT_VALUE;
    }

    Dev->Hcd->Ops->SubmitTransfer(Dev->Hcd, Hid->IntInTransfer);
    ConsolePrint("[USB HID] %s initialized\n", IsKbd ? "Keyboard" : "Mouse");

    return SUCCESS;
}

static INT UsbHidDisconnect(UsbDevice *Dev) {
    UsbHidDevice *Hid = (UsbHidDevice *)Dev->DriverData;
    if (Hid) {
        if (Hid->IntInTransfer) MemoryFree(Hid->IntInTransfer);
        if (Hid->Buffer) MemoryFree(Hid->Buffer);
        MemoryFree(Hid);
    }
    return SUCCESS;
}

static UsbDriver GHidDriver = {
    .Name = "USB HID Driver",
    .DeviceClass = USB_CLASS_HID,
    .Probe = UsbHidProbe,
    .Disconnect = UsbHidDisconnect
};

void UsbHidDriverInit() {
    UsbDriverRegister(&GHidDriver);
}
