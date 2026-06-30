#include <Gdl/Gdl.h>
#include <Lib/String.h>
#include <Memory/Allocator.h>

static ListHead GdlDevices;
static UINT32 GdlDeviceCount = 0;
static GdlDevice *GdlPrimaryDevice = NULLPTR;

NOPTR GdlInit(NOPTR) {
    ListInit(&GdlDevices);  // Инициализируем список
    GdlDeviceCount = 0;
    GdlPrimaryDevice = NULLPTR;
    
    GdlGemInit();
}

INT GdlDeviceRegister(GdlDevice *Dev) {
    if (!Dev) return GDL_ERR_INVALID_PARAM;
    
    ListAddTail(&GdlDevices, &Dev->Node);
    GdlDeviceCount++;
    
    if (!GdlPrimaryDevice) {
        GdlPrimaryDevice = Dev;
    }
    
    return GDL_OK;
}

INT GdlDeviceUnregister(GdlDevice *Dev) {
    if (!Dev) return GDL_ERR_INVALID_PARAM;
    
    ListDel(&Dev->Node);
    GdlDeviceCount--;
    
    if (GdlPrimaryDevice == Dev) {
        GdlPrimaryDevice = NULLPTR;
        if (!ListEmpty(&GdlDevices)) {
            GdlPrimaryDevice = ListEntry(GdlDevices.Next, GdlDevice, Node);
        }
    }

    return GDL_OK;
}

GdlDevice *GdlDeviceFind(UINT32 VendorId, UINT32 DeviceId) {
    ListHead *Pos;
    ListForEach(Pos, &GdlDevices) {
        GdlDevice *Dev = ListEntry(Pos, GdlDevice, Node);
        if (Dev->VendorId == VendorId && Dev->DeviceId == DeviceId) {
            return Dev;
        }
    }
    return NULLPTR;
}

GdlDevice *GdlDeviceFindByPci(PciDevice *PciDev) {
    if (!PciDev) return NULLPTR;
    
    ListHead *Pos;
    ListForEach(Pos, &GdlDevices) {
        GdlDevice *Dev = ListEntry(Pos, GdlDevice, Node);
        if (Dev->PciDev == PciDev) {
            return Dev;
        }
    }
    return NULLPTR;
}

GdlDevice *GdlGetPrimaryDevice(NOPTR) {
    return GdlPrimaryDevice;
}