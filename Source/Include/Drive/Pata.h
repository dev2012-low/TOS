#pragma once

#include <Kernel/Types.h>
#include <Lib/String.h>
#include <Pci.h>

/*
 * Basic ports (compatibility mode)
 */
#define PATA_BASE_PRIMARY 0x1F0
#define PATA_CTRL_PRIMARY 0x3F6
#define PATA_BASE_SECONDARY 0x170
#define PATA_CTRL_SECONDARY 0x376

/*
 * Register offsets relative to base/ctrl
 */
#define PATA_DATA 0x00    /*
 * word I/O
 */
#define PATA_ERROR 0x01   /*
 * reading
 */
#define PATA_FEATURE 0x01 /*
 * record
 */
#define PATA_NSECT 0x02
#define PATA_SECTOR 0x03  /*
 * LBA0
 */
#define PATA_LCYL 0x04    /*
 * LBA1
 */
#define PATA_HCYL 0x05    /*
 * LBA2
 */
#define PATA_SELECT 0x06  /*
 * Drive/Head
 */
#define PATA_STATUS 0x07  /*
 * reading
 */
#define PATA_COMMAND 0x07 /*
 * record
 */

/*
 * ctrl port
 */
#define PATA_ALTSTATUS 0x0 /*
 * reading
 */
#define PATA_CONTROL 0x0   /*
 * record
 */

/*
 * Status bits
 */
#define PATA_STATUS_BSY 0x80
#define PATA_STATUS_DRDY 0x40
#define PATA_STATUS_DRQ 0x08
#define PATA_STATUS_ERR 0x01
#define PATA_STATUS_DF 0x20

/*
 * Teams
 */
#define PATA_CMD_READ_SECTORS 0x20
#define PATA_CMD_WRITE_SECTORS 0x30
#define PATA_CMD_IDENTIFY 0xEC
#define PATA_CMD_IDENTIFY_PACKET 0xA1
#define PATA_CMD_READ_SECTORS_EXT 0x24
#define PATA_CMD_WRITE_SECTORS_EXT 0x34
#define PATA_CMD_CACHE_FLUSH 0xE7
#define PATA_CMD_CACHE_FLUSH_EXT 0xEA

/*
 * Timeout (poll iterations)
 */
#define PATA_TIMEOUT_LOOPS 100000U

typedef enum {
    PATA_CHANNEL_PRIMARY = 0,
    PATA_CHANNEL_SECONDARY = 1
} PataChannel;

typedef enum {
    PATA_TYPE_NONE = 0,
    PATA_TYPE_ATA,
    PATA_TYPE_ATAPI
} PataDeviceType;

typedef struct {
    UINT16 BasePort; /*
 * 0x1F0 / 0x170
 */
    UINT16 CtrlPort; /*
 * 0x3F6 / 0x376
 */
    UINT8 Drive;      /*
 * 0 = master, 1 = slave
 */
    PataChannel Channel;
    PataDeviceType Type;    /*
 * ATA / ATAPI / NONE
 */
    UINT64 TotalSectors; /*
 * number of sectors (512B)
 */
    UINT16 SectorSize;   /*
 * sector size (usually 512)
 */
    INT SupportsLba48;     /*
 * support LBA48 (bool)
 */
    volatile UINT8 IrqPending;
    volatile UINT8 IrqCount;
    UINT8 Irq;  // real IRQ from PCI
    PciDevice* PciDev;  //pointer to PCI device

    UINT16 BusMasterBase;
    
} PataDrive;

INT PataInit(PataDrive *Drive, PataChannel Channel, UINT8 DRive);
INT PataIdentify(PataDrive *Drive, UINT16 IdentBuffer[256]);
INT PataReadSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, NOPTR *buffer);
INT PataWriteSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, const NOPTR *Buffer);

NOPTR PataPrimaryIrqHandler(NOPTR);
NOPTR PataSecondaryIrqHandler(NOPTR);
NOPTR PataEnableInterrupts(UINT16 CtrlPort);
NOPTR PataDisableInterrupts(UINT16 CtrlPort);
INT PataWaitIrq(PataDrive *Drive, UINT32 TimeoutMs);
UINT8 PataGetIrqCount(PataDrive *Drive);

typedef struct PataRequest PataRequest;

INT PataReadSectorsAsync(PataDrive *Drive, UINT64 Lba, UINT32 Count, 
                          NOPTR *Buffer, PataRequest *Req);
INT PataWriteSectorsAsync(PataDrive *Drive, UINT64 Lba, UINT32 Count,
                           const NOPTR *Buffer, PataRequest *Req);
INT PataWaitRequest(PataRequest *Req, UINT32 TimeoutMs);