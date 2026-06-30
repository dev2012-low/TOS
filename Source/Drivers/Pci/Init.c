#include <Pci.h>
#include <Kernel/Types.h>
#include <Kernel/KDriver.h>

EXTERN(UINT16, PciReadVendor(UINT8 Bus, UINT8 Slot, UINT8 Func));
EXTERN(NOPTR, PciScanBus(UINT8 Bus));

NOPTR PciInit(NOPTR) {
    //Checking the multifunction of the host bridge (0:0:0)
    PciDevice TempDev = {.Bus = 0, .Slot = 0, .Function = 0};
    UINT8 HeaderType = (PciRead(&TempDev, 0x0C) >> 16) & 0xFF;
    
    if (HeaderType & 0x80) {
        // Multiple host controllers
        for (UINT8 Func = 0; Func < 8; Func++) {
            if (PciReadVendor(0, 0, Func) != PCI_INVALID_VENDOR) {
                PciScanBus(Func);
            }
        }
    } else {
        // Single host controller
        PciScanBus(0);
    }

    KDriverRegister(KDriverGenerateStruct("PCI", DCL0, TRUE, NULLPTR, NULLPTR));
}     