#include <Usb/Usb.h>
#include <Memory/Allocator.h>
#include <Console.h>
#include <Kernel/Return.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Drive/Drive.h>
#include <Time/Timer.h>

#define USB_MS_CBW_SIGNATURE 0x43425355
#define USB_MS_CSW_SIGNATURE 0x53425355

typedef struct {
    UINT32 Signature;
    UINT32 Tag;
    UINT32 DataTransferLength;
    UINT8 Flags;
    UINT8 Lun;
    UINT8 CdbLength;
    UINT8 Cdb[16];
} ATTRIBUTE(packed) UsbMsCbw;

typedef struct {
    UINT32 Signature;
    UINT32 Tag;
    UINT32 DataResidue;
    UINT8 Status;
} ATTRIBUTE(packed) UsbMsCsw;

typedef struct {
    UsbDevice *Dev;
    UINT8 EpIn;
    UINT8 EpOut;
    UINT32 Tag;
    UINT32 BlockSize;
    UINT64 TotalBlocks;
    Drive *Drive;
} UsbStoragePriv;

static UsbDriver GUsbStorageDriver;
static UINT32 GUsbDriveCount = 0;

static INT UsbStorageBotCommand(UsbDevice *Dev, UsbStoragePriv *Priv,
                                UINT8 *Cdb, UINT8 CdbLen,
                                UINT8 *Data, UINT32 DataLen, BOOL DataIn) {
    UsbMsCbw Cbw;
    UsbMsCsw Csw;
    INT Ret;

    MemSet(&Cbw, 0, sizeof(Cbw));
    Cbw.Signature = USB_MS_CBW_SIGNATURE;
    Cbw.Tag = ++Priv->Tag;
    Cbw.DataTransferLength = DataLen;
    Cbw.Flags = DataIn ? 0x80 : 0x00;
    Cbw.Lun = 0;
    Cbw.CdbLength = CdbLen;
    MemCpy(Cbw.Cdb, Cdb, CdbLen);

    Ret = UsbBulkTransfer(Dev, Priv->EpOut, (NOPTR*)&Cbw, 31);
    if (Ret < 0) return Ret;

    if (DataLen > 0 && Data) {
        Ret = UsbBulkTransfer(Dev, DataIn ? Priv->EpIn : Priv->EpOut, Data, DataLen);
        if (Ret < 0) return Ret;
    }

    Ret = UsbBulkTransfer(Dev, Priv->EpIn, (NOPTR*)&Csw, sizeof(Csw));
    if (Ret < 0) return Ret;

    if (Csw.Signature != USB_MS_CSW_SIGNATURE || Csw.Status != 0) {
        RETURN(DEVICE_ERROR);
    }

    RETURN(SUCCESS);
}

static INT UsbStorageRead(Drive *Drive, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    UsbStoragePriv *Priv = (UsbStoragePriv*)Drive->Priv;
    UINT8 Cdb[10];
    MemSet(Cdb, 0, sizeof(Cdb));
    Cdb[0] = 0x28; // READ(10)
    Cdb[2] = (Lba >> 24) & 0xFF; Cdb[3] = (Lba >> 16) & 0xFF;
    Cdb[4] = (Lba >> 8) & 0xFF;  Cdb[5] = Lba & 0xFF;
    Cdb[7] = (Count >> 8) & 0xFF; Cdb[8] = Count & 0xFF;
    return UsbStorageBotCommand(Priv->Dev, Priv, Cdb, 10, (UINT8*)Buffer, Count * Priv->BlockSize, TRUE);
}

static INT UsbStorageWrite(Drive *Drive, UINT64 Lba, UINT32 Count, const NOPTR *Buffer) {
    UsbStoragePriv *Priv = (UsbStoragePriv*)Drive->Priv;
    UINT8 Cdb[10];
    MemSet(Cdb, 0, sizeof(Cdb));
    Cdb[0] = 0x2A; // WRITE(10)
    Cdb[2] = (Lba >> 24) & 0xFF; Cdb[3] = (Lba >> 16) & 0xFF;
    Cdb[4] = (Lba >> 8) & 0xFF;  Cdb[5] = Lba & 0xFF;
    Cdb[7] = (Count >> 8) & 0xFF; Cdb[8] = Count & 0xFF;
    return UsbStorageBotCommand(Priv->Dev, Priv, Cdb, 10, (UINT8*)Buffer, Count * Priv->BlockSize, FALSE);
}

static INT UsbStorageReadCapacity(UsbDevice *Dev, UsbStoragePriv *Priv) {
    UINT8 Cdb[10];
    UINT8 Resp[8];
    MemSet(Cdb, 0, sizeof(Cdb));
    Cdb[0] = 0x25; // READ CAPACITY(10)
    if (UsbStorageBotCommand(Dev, Priv, Cdb, 10, Resp, sizeof(Resp), TRUE) != SUCCESS) RETURN(DEVICE_ERROR);
    Priv->TotalBlocks = ((UINT32)Resp[0] << 24) | ((UINT32)Resp[1] << 16) | ((UINT32)Resp[2] << 8) | Resp[3];
    Priv->TotalBlocks++;
    Priv->BlockSize = ((UINT32)Resp[4] << 24) | ((UINT32)Resp[5] << 16) | ((UINT32)Resp[6] << 8) | Resp[7];
    return SUCCESS;
}

static BOOL UsbStorageFindBulkEps(UsbDevice *Dev, UINT8 *EpIn, UINT8 *EpOut) {
    for (UINT32 I = 0; I < Dev->EndpointCount; I++) {
        UsbEndpointDescriptor *Ep = &Dev->EpDesc[I];
        if ((Ep->BmAttributes & 0x03) == USB_ENDPOINT_BULK) {
            if (Ep->BEndpointAddress & 0x80) *EpIn = Ep->BEndpointAddress;
            else *EpOut = Ep->BEndpointAddress;
        }
    }
    return (*EpIn != 0 && *EpOut != 0);
}

static INT UsbStorageProbe(UsbDevice *Dev) {
    if (Dev->DeviceClass != USB_CLASS_MASS_STORAGE) RETURN(NO_OBJECT);
    UsbStoragePriv *Priv = (UsbStoragePriv*)MemoryAllocate(sizeof(UsbStoragePriv));
    if (!Priv) RETURN(NO_MEMORY);
    MemSet(Priv, 0, sizeof(UsbStoragePriv));
    Priv->Dev = Dev;
    if (!UsbStorageFindBulkEps(Dev, &Priv->EpIn, &Priv->EpOut)) {
        MemoryFree(Priv);
        RETURN(NO_OBJECT);
    }
    Dev->DriverData = Priv;
    TimerMdelay(100);
    if (UsbStorageReadCapacity(Dev, Priv) != SUCCESS) {
        MemoryFree(Priv);
        RETURN(DEVICE_ERROR);
    }
    Drive *D = (Drive*)MemoryAllocate(sizeof(Drive));
    if (D) {
        MemSet(D, 0, sizeof(Drive));
        SnPrintf(D->Name, sizeof(D->Name), "USB%u", GUsbDriveCount++);
        D->Type = DRIVE_TYPE_USB; D->SectorSize = Priv->BlockSize;
        D->TotalSectors = Priv->TotalBlocks; D->Priv = Priv;
        D->Read = UsbStorageRead; D->Write = UsbStorageWrite;
        Priv->Drive = D; DriveRegister(D);
        ConsolePrint("[USB-MS] Registered as %s\n", D->Name);
    }
    RETURN(SUCCESS);
}

static INT UsbStorageDisconnect(UsbDevice *Dev) {
    if (Dev && Dev->DriverData) {
        UsbStoragePriv *Priv = (UsbStoragePriv*)Dev->DriverData;
        if (Priv->Drive) MemoryFree(Priv->Drive);
        MemoryFree(Priv);
    }
    RETURN(SUCCESS);
}

INT UsbStorageDriverInit(NOPTR) {
    MemSet(&GUsbStorageDriver, 0, sizeof(GUsbStorageDriver));
    StrCpy(GUsbStorageDriver.Name, "usb-storage");
    GUsbStorageDriver.DeviceClass = USB_CLASS_MASS_STORAGE;
    GUsbStorageDriver.Probe = UsbStorageProbe;
    GUsbStorageDriver.Disconnect = UsbStorageDisconnect;
    return UsbDriverRegister(&GUsbStorageDriver);
}
