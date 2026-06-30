#include <Usb/Usb.h>
#include <Memory/Allocator.h>
#include <Memory/PhysAlloc.h>
#include <Lib/String.h>
#include <Console.h>
#include <Kernel/Return.h>
#include <Asm/Cpu.h>
#include <Time/Timer.h>
#include <Usb/UsbHid.h>

static struct ListHead GUsbHcds;
static struct ListHead GUsbDrivers;
static struct ListHead GUsbDevices;
static UINT32 GUsbInitRefcount = 0;

static INT UsbWaitTransfer(UsbTransfer *Transfer) {
    UINT32 I;
    UINT32 Max = Transfer->TimeoutMs ? Transfer->TimeoutMs * 1000 : 1000000;

    for (I = 0; I < Max && Transfer->Status == 0; I++) {
        CpuPause();
    }

    if (Transfer->Status == 0) {
        Transfer->Status = TIMEOUT;
    }
    return Transfer->Status;
}

NOPTR UsbInit(NOPTR) {
    if (GUsbInitRefcount++ > 0) {
        return;
    }

    ListInit(&GUsbHcds);
    ListInit(&GUsbDrivers);
    ListInit(&GUsbDevices);

    UsbHubDriverInit();
    UsbStorageDriverInit();
    UsbHidDriverInit();
}

INT UsbHcdRegister(UsbHcd *Hcd) {
    if (!Hcd || !Hcd->Ops) {
        RETURN(NO_OBJECT);
    }

    ListInit(&Hcd->Devices);
    Hcd->NextAddress = 1;
    ListAddTail(&GUsbHcds, &Hcd->Node);

    if (Hcd->Ops->Init) {
        if (Hcd->Ops->Init(Hcd) != SUCCESS) {
            ConsolePrint("[USB] Failed to init HCD %s\n", Hcd->Name);
            ListDel(&Hcd->Node);
            RETURN(INCORRECT_VALUE);
        }
    }
    RETURN(SUCCESS);
}

INT UsbBindDriver(UsbDevice *Dev) {
    struct ListHead *Pos;

    if (!Dev || Dev->Driver) {
        RETURN(SUCCESS);
    }

    ListForEach(Pos, &GUsbDrivers) {
        UsbDriver *Drv = ListEntry(Pos, UsbDriver, Node);

        if (Drv->VendorId && Drv->VendorId != Dev->VendorId) {
            continue;
        }
        if (Drv->ProductId && Drv->ProductId != Dev->ProductId) {
            continue;
        }
        if (Drv->DeviceClass && Drv->DeviceClass != Dev->DeviceClass) {
            continue;
        }
        if (Drv->DeviceSubclass && Drv->DeviceSubclass != Dev->DeviceSubclass) {
            continue;
        }
        if (Drv->DeviceProtocol && Drv->DeviceProtocol != Dev->DeviceProtocol) {
            continue;
        }

        if (Drv->Probe && Drv->Probe(Dev) == SUCCESS) {
            Dev->Driver = Drv;
            RETURN(SUCCESS);
        }
    }

    ConsolePrint("[USB] No driver for %04X:%04X class=%02X\n",
                 Dev->VendorId, Dev->ProductId, Dev->DeviceClass);
    RETURN(NO_OBJECT);
}

INT UsbDriverRegister(UsbDriver *Drv) {
    struct ListHead *Pos;

    if (!Drv) {
        RETURN(NO_OBJECT);
    }

    ListAddTail(&GUsbDrivers, &Drv->Node);

    ListForEach(Pos, &GUsbDevices) {
        UsbDevice *Dev = ListEntry(Pos, UsbDevice, Node);
        UsbBindDriver(Dev);
    }

    RETURN(SUCCESS);
}

UsbDevice *UsbDeviceAlloc(UsbHcd *Hcd) {
    UsbDevice *Dev = (UsbDevice*)MemoryAllocate(sizeof(UsbDevice));
    if (!Dev) {
        return NULLPTR;
    }

    MemSet(Dev, 0, sizeof(UsbDevice));
    Dev->Hcd = Hcd;
    Dev->Address = 0;
    Dev->MaxPacketSize0 = 8;
    ListInit(&Dev->Childs);
    return Dev;
}

NOPTR UsbDeviceFree(UsbDevice *Dev) {
    struct ListHead *Pos;
    struct ListHead *Tmp;

    if (!Dev) {
        return;
    }

    ListForEachSafe(Pos, Tmp, &Dev->Childs) {
        UsbDevice *Child = ListEntry(Pos, UsbDevice, Node);
        UsbDeviceRemove(Child);
    }

    if (Dev->DeviceDesc) {
        MemoryFree(Dev->DeviceDesc);
    }
    if (Dev->ConfigData) {
        MemoryFree(Dev->ConfigData);
    }

    MemoryFree(Dev);
}

INT UsbDeviceAdd(UsbHcd *Hcd, UsbDevice *Dev) {
    if (!Hcd || !Dev) {
        RETURN(NO_OBJECT);
    }

    if (Dev->Address == 0) {
        Dev->Address = Hcd->NextAddress++;
        if (Hcd->NextAddress > 127) {
            Hcd->NextAddress = 1;
        }
    }

    ListAddTail(&GUsbDevices, &Dev->Node);
    ListAddTail(&Hcd->Devices, &Dev->HcdNode);
    Hcd->DeviceCount++;

    UsbBindDriver(Dev);
    RETURN(SUCCESS);
}

INT UsbDeviceRemove(UsbDevice *Dev) {
    if (!Dev) {
        RETURN(NO_OBJECT);
    }

    if (Dev->Driver && Dev->Driver->Disconnect) {
        Dev->Driver->Disconnect(Dev);
    }
    Dev->Driver = NULLPTR;

    ListDel(&Dev->Node);
    if (Dev->Hcd) {
        ListDel(&Dev->HcdNode);
        Dev->Hcd->DeviceCount--;
    }

    UsbDeviceFree(Dev);
    RETURN(SUCCESS);
}

UsbDevice *UsbDeviceFind(UINT16 VendorId, UINT16 ProductId) {
    struct ListHead *Pos;

    ListForEach(Pos, &GUsbDevices) {
        UsbDevice *Dev = ListEntry(Pos, UsbDevice, Node);
        if (Dev->VendorId == VendorId && Dev->ProductId == ProductId) {
            return Dev;
        }
    }
    return NULLPTR;
}

INT UsbControlTransfer(UsbDevice *Dev, UINT8 RequestType, UINT8 Request,
                       UINT16 Value, UINT16 Index, UINT16 Length, NOPTR *Data) {
    UsbSetupPacket *SetupPage;
    UsbTransfer *Transfer;
    INT Ret;

    if (!Dev || !Dev->Hcd || !Dev->Hcd->Ops || !Dev->Hcd->Ops->SubmitTransfer) {
        RETURN(NO_OBJECT);
    }

    SetupPage = (UsbSetupPacket*)PhysAllocAllocatePage(PhysAllocGet());
    if (!SetupPage) {
        RETURN(NO_MEMORY);
    }

    Transfer = (UsbTransfer*)MemoryAllocate(sizeof(UsbTransfer));
    if (!Transfer) {
        PhysAllocFreePage(PhysAllocGet(), (NOPTR*)SetupPage);
        RETURN(NO_MEMORY);
    }

    MemSet(Transfer, 0, sizeof(UsbTransfer));
    SetupPage->BmRequestType = RequestType;
    SetupPage->BRequest = Request;
    SetupPage->WValue = Value;
    SetupPage->WIndex = Index;
    SetupPage->WLength = Length;

    Transfer->Device = Dev;
    Transfer->Endpoint = 0;
    Transfer->Direction = (RequestType & USB_DIR_IN) ? USB_DIR_IN : USB_DIR_OUT;
    Transfer->Type = USB_TRANSFER_CONTROL;
    Transfer->Length = Length;
    Transfer->Buffer = (UINT8*)Data;
    Transfer->TimeoutMs = 2000;
    Transfer->Status = 0;
    Transfer->Context = SetupPage;

    Ret = Dev->Hcd->Ops->SubmitTransfer(Dev->Hcd, Transfer);
    if (Ret == SUCCESS) {
        Ret = UsbWaitTransfer(Transfer);
    }

    PhysAllocFreePage(PhysAllocGet(), (NOPTR*)SetupPage);
    MemoryFree(Transfer);
    RETURN(Ret);
}

INT UsbBulkTransfer(UsbDevice *Dev, UINT8 Endpoint, NOPTR *Data, UINT32 Length) {
    UsbTransfer *Transfer;
    INT Ret;

    if (!Dev || !Dev->Hcd || !Dev->Hcd->Ops || !Dev->Hcd->Ops->SubmitTransfer) {
        RETURN(NO_OBJECT);
    }

    Transfer = (UsbTransfer*)MemoryAllocate(sizeof(UsbTransfer));
    if (!Transfer) {
        RETURN(NO_MEMORY);
    }

    MemSet(Transfer, 0, sizeof(UsbTransfer));
    Transfer->Device = Dev;
    Transfer->Endpoint = Endpoint & 0x7F;
    Transfer->Direction = (Endpoint & 0x80) ? USB_DIR_IN : USB_DIR_OUT;
    Transfer->Type = USB_TRANSFER_BULK;
    Transfer->Length = Length;
    Transfer->Buffer = (UINT8*)Data;
    Transfer->TimeoutMs = 5000;
    Transfer->Status = 0;

    Ret = Dev->Hcd->Ops->SubmitTransfer(Dev->Hcd, Transfer);
    if (Ret == SUCCESS) {
        Ret = UsbWaitTransfer(Transfer);
    }

    MemoryFree(Transfer);
    RETURN(Ret);
}

INT UsbGetDescriptor(UsbDevice *Dev, UINT8 Type, UINT8 Index, NOPTR *Buf, UINT16 Len) {
    return UsbControlTransfer(Dev,
        USB_DIR_IN | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR,
        (UINT16)((Type << 8) | Index), 0, Len, Buf);
}

INT UsbGetDeviceDescriptor(UsbDevice *Dev, UsbDeviceDescriptor *Desc) {
    INT Ret = UsbGetDescriptor(Dev, USB_DESC_DEVICE, 0, Desc, sizeof(UsbDeviceDescriptor));
    if (Ret < 0) {
        return Ret;
    }

    Dev->VendorId = Desc->IdVendor;
    Dev->ProductId = Desc->IdProduct;
    Dev->DeviceClass = Desc->BDeviceClass;
    Dev->DeviceSubclass = Desc->BDeviceSubClass;
    Dev->DeviceProtocol = Desc->BDeviceProtocol;
    Dev->MaxPacketSize0 = Desc->BMaxPacketSize0;
    Dev->BcdDevice = Desc->BcdDevice;

    RETURN(SUCCESS);
}

INT UsbGetStringDescriptor(UsbDevice *Dev, UINT8 Index, CHAR *Buf, UINT16 Len) {
    UINT8 Raw[256];
    UINT16 I;
    INT Ret;

    if (!Buf || Len == 0) {
        RETURN(INCORRECT_VALUE);
    }

    Ret = UsbGetDescriptor(Dev, USB_DESC_STRING, Index, Raw, sizeof(Raw));
    if (Ret < 0) {
        return Ret;
    }
    if (Raw[0] < 2) {
        Buf[0] = '\0';
        RETURN(SUCCESS);
    }

    for (I = 0; I + 1 < Raw[0] && I / 2 + 1 < Len; I += 2) {
        Buf[I / 2] = (CHAR)Raw[I + 2];
    }
    {
        USIZE End = (USIZE)(Raw[0] / 2);
        if (End >= Len) {
            End = Len - 1;
        }
        Buf[End] = '\0';
    }
    RETURN(SUCCESS);
}

INT UsbSetAddress(UsbDevice *Dev, UINT8 Address) {
    INT Ret = UsbControlTransfer(Dev,
        USB_DIR_OUT | USB_RECIP_DEVICE, USB_REQ_SET_ADDRESS,
        Address, 0, 0, NULLPTR);
    if (Ret == SUCCESS) {
        Dev->Address = Address;
        TimerMdelay(2);
    }
    return Ret;
}

INT UsbSetConfiguration(UsbDevice *Dev, UINT8 Configuration) {
    INT Ret = UsbControlTransfer(Dev,
        USB_DIR_OUT | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION,
        Configuration, 0, 0, NULLPTR);
    if (Ret == SUCCESS) {
        Dev->Configuration = Configuration;
    }
    return Ret;
}

INT UsbParseConfiguration(UsbDevice *Dev, UINT8 *Data, UINT32 Len) {
    UINT32 Offset = 0;
    UsbInterfaceDescriptor *Iface = NULLPTR;

    if (!Dev || !Data) {
        RETURN(NO_OBJECT);
    }

    Dev->EndpointCount = 0;

    while (Offset + 2 <= Len) {
        UINT8 DescLen = Data[Offset];
        UINT8 DescType = Data[Offset + 1];

        if (DescLen < 2 || Offset + DescLen > Len) {
            break;
        }

        if (DescType == USB_DESC_INTERFACE && DescLen >= sizeof(UsbInterfaceDescriptor)) {
            Iface = (UsbInterfaceDescriptor*)&Data[Offset];
            Dev->Interface = Iface->BInterfaceNumber;
            if (Dev->DeviceClass == 0) {
                Dev->DeviceClass = Iface->BInterfaceClass;
                Dev->DeviceSubclass = Iface->BInterfaceSubClass;
                Dev->DeviceProtocol = Iface->BInterfaceProtocol;
            }
        } else if (DescType == USB_DESC_ENDPOINT && DescLen >= sizeof(UsbEndpointDescriptor)) {
            if (Dev->EndpointCount < USB_MAX_ENDPOINTS) {
                MemCpy(&Dev->EpDesc[Dev->EndpointCount], &Data[Offset], sizeof(UsbEndpointDescriptor));
                Dev->EndpointCount++;
            }
            (void)Iface;
        }

        Offset += DescLen;
    }

    RETURN(SUCCESS);
}

INT UsbEnumeration(UsbDevice *Dev) {
    UsbDeviceDescriptor Desc;
    UsbConfigDescriptor ConfigHdr;
    UINT8 ConfigBuf[512];
    UINT8 NewAddress;
    INT Ret;

    if (!Dev || !Dev->Hcd) {
        RETURN(NO_OBJECT);
    }

    Dev->Address = 0;
    Dev->MaxPacketSize0 = 8;

    Ret = UsbGetDescriptor(Dev, USB_DESC_DEVICE, 0, &Desc, 8);
    if (Ret < 0) {
        ConsolePrint("[USB] GET_DESCRIPTOR(8) failed\n");
        RETURN(GENERAL_ERROR);
    }
    Dev->MaxPacketSize0 = Desc.BMaxPacketSize0;

    NewAddress = Dev->Hcd->NextAddress;
    if (NewAddress == 0 || NewAddress > 127) {
        NewAddress = 1;
    }

    Ret = UsbSetAddress(Dev, NewAddress);
    if (Ret < 0) {
        ConsolePrint("[USB] SET_ADDRESS failed\n");
        RETURN(GENERAL_ERROR);
    }

    Ret = UsbGetDeviceDescriptor(Dev, &Desc);
    if (Ret < 0) {
        ConsolePrint("[USB] GET_DESCRIPTOR failed\n");
        RETURN(GENERAL_ERROR);
    }

    Dev->DeviceDesc = (UsbDeviceDescriptor*)MemoryAllocate(sizeof(UsbDeviceDescriptor));
    if (Dev->DeviceDesc) {
        MemCpy(Dev->DeviceDesc, &Desc, sizeof(Desc));
    }

    ConsolePrint("[USB] Device %04X:%04X %s addr=%d speed=%s mps0=%d\n",
                 Desc.IdVendor, Desc.IdProduct,
                 UsbClassString(Desc.BDeviceClass),
                 Dev->Address, UsbSpeedString(Dev->Speed), Dev->MaxPacketSize0);

    Ret = UsbGetDescriptor(Dev, USB_DESC_CONFIGURATION, 0, &ConfigHdr, sizeof(ConfigHdr));
    if (Ret < 0) {
        ConsolePrint("[USB] GET_CONFIG header failed\n");
        RETURN(GENERAL_ERROR);
    }

    if (ConfigHdr.WTotalLength > sizeof(ConfigBuf)) {
        ConsolePrint("[USB] Config too large (%d)\n", ConfigHdr.WTotalLength);
        RETURN(GENERAL_ERROR);
    }

    Ret = UsbGetDescriptor(Dev, USB_DESC_CONFIGURATION, 0, ConfigBuf, ConfigHdr.WTotalLength);
    if (Ret < 0) {
        ConsolePrint("[USB] GET_CONFIG failed\n");
        RETURN(GENERAL_ERROR);
    }

    Dev->ConfigLen = ConfigHdr.WTotalLength;
    Dev->ConfigData = MemoryAllocate(ConfigHdr.WTotalLength);
    if (Dev->ConfigData) {
        MemCpy(Dev->ConfigData, ConfigBuf, ConfigHdr.WTotalLength);
    }

    UsbParseConfiguration(Dev, ConfigBuf, ConfigHdr.WTotalLength);

    Ret = UsbSetConfiguration(Dev, ConfigHdr.BConfigurationValue);
    if (Ret < 0) {
        ConsolePrint("[USB] SET_CONFIGURATION failed\n");
        RETURN(GENERAL_ERROR);
    }

    RETURN(SUCCESS);
}
