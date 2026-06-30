#include <Usb/Usb.h>
#include <Memory/Allocator.h>
#include <Memory/PhysAlloc.h>
#include <Console.h>
#include <Kernel/Return.h>
#include <Time/Timer.h>

#define USB_HUB_PORT_POWER   8
#define USB_HUB_PORT_RESET   4
#define USB_HUB_PORT_ENABLE  1

static UsbDriver GUsbHubDriver;

static INT UsbHubGetDescriptor(UsbDevice *Dev, UsbHubDescriptor *HubDesc) {
    return UsbControlTransfer(Dev,
        USB_DIR_IN | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
        (UINT16)((USB_DESC_HUB << 8) | 0), 0, sizeof(UsbHubDescriptor), HubDesc);
}

static INT UsbHubSetPortFeature(UsbDevice *Dev, UINT8 Port, UINT8 Feature) {
    return UsbControlTransfer(Dev,
        USB_DIR_OUT | USB_RECIP_OTHER, USB_REQ_SET_FEATURE,
        Feature, Port, 0, NULLPTR);
}

static INT UsbHubClearPortFeature(UsbDevice *Dev, UINT8 Port, UINT8 Feature) {
    return UsbControlTransfer(Dev,
        USB_DIR_OUT | USB_RECIP_OTHER, USB_REQ_CLEAR_FEATURE,
        Feature, Port, 0, NULLPTR);
}

static INT UsbHubGetPortStatus(UsbDevice *Dev, UINT8 Port, UINT16 *Status) {
    return UsbControlTransfer(Dev,
        USB_DIR_IN | USB_RECIP_OTHER, USB_REQ_GET_STATUS,
        0, Port, 2, Status);
}

static INT UsbHubResetPort(UsbDevice *Dev, UINT8 Port) {
    INT Ret = UsbHubSetPortFeature(Dev, Port, USB_HUB_PORT_RESET);
    if (Ret < 0) {
        return Ret;
    }
    TimerMdelay(20);
    return UsbHubClearPortFeature(Dev, Port, USB_HUB_PORT_RESET);
}

static INT UsbHubEnumeratePort(UsbDevice *Hub, UINT8 Port) {
    UsbDevice *Child;
    UINT16 Status = 0;
    INT Ret;

    Ret = UsbHubGetPortStatus(Hub, Port, &Status);
    if (Ret < 0 || !(Status & 0x0001)) {
        return Ret;
    }

    ConsolePrint("[USB-HUB] Port %d: device connected (status=0x%04x)\n", Port, Status);

    Ret = UsbHubResetPort(Hub, Port);
    if (Ret < 0) {
        ConsolePrint("[USB-HUB] Port %d reset failed\n", Port);
        return Ret;
    }

    Child = UsbDeviceAlloc(Hub->Hcd);
    if (!Child) {
        RETURN(NO_MEMORY);
    }

    Child->Parent = Hub;
    Child->HubPort = Port;
    Child->Speed = (UINT8)((Status >> 9) & 0x03);
    if (Child->Speed == 3) {
        Child->Speed = USB_SPEED_SUPER;
    } else if (Child->Speed == 2) {
        Child->Speed = USB_SPEED_HIGH;
    } else if (Child->Speed == 1) {
        Child->Speed = USB_SPEED_LOW;
    } else {
        Child->Speed = USB_SPEED_FULL;
    }

    ListAddTail(&Hub->Childs, &Child->Node);

    if (UsbEnumeration(Child) == SUCCESS) {
        UsbDeviceAdd(Hub->Hcd, Child);
        ConsolePrint("[USB-HUB] Port %d enumerated\n", Port);
        RETURN(SUCCESS);
    }

    ListDel(&Child->Node);
    UsbDeviceFree(Child);
    RETURN(GENERAL_ERROR);
}

static INT UsbHubProbe(UsbDevice *Dev) {
    UsbHubDescriptor HubDesc;
    UINT8 Port;
    INT Ret;

    if (!Dev || Dev->DeviceClass != USB_CLASS_HUB) {
        RETURN(NO_OBJECT);
    }

    Ret = UsbHubGetDescriptor(Dev, &HubDesc);
    if (Ret < 0) {
        ConsolePrint("[USB-HUB] Failed to read hub descriptor\n");
        RETURN(GENERAL_ERROR);
    }

    ConsolePrint("[USB-HUB] %d ports, power good=%dms\n",
                 HubDesc.BNbPorts, HubDesc.BPowerOnToPowerGood * 2);

    for (Port = 1; Port <= HubDesc.BNbPorts; Port++) {
        UsbHubSetPortFeature(Dev, Port, USB_HUB_PORT_POWER);
    }
    TimerMdelay(HubDesc.BPowerOnToPowerGood * 2);

    for (Port = 1; Port <= HubDesc.BNbPorts; Port++) {
        UsbHubEnumeratePort(Dev, Port);
    }

    Dev->DriverData = Dev;
    RETURN(SUCCESS);
}

static INT UsbHubDisconnect(UsbDevice *Dev) {
    (void)Dev;
    RETURN(SUCCESS);
}

INT UsbHubDriverInit(NOPTR) {
    MemSet(&GUsbHubDriver, 0, sizeof(GUsbHubDriver));
    StrCpy(GUsbHubDriver.Name, "usb-hub");
    GUsbHubDriver.DeviceClass = USB_CLASS_HUB;
    GUsbHubDriver.Probe = UsbHubProbe;
    GUsbHubDriver.Disconnect = UsbHubDisconnect;
    return UsbDriverRegister(&GUsbHubDriver);
}
