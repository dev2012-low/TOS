#include <Drive/Pata.h>
#include <Asm/Io.h>
#include <Lib/String.h>
#include <Kernel/Idt.h>
#include <Ioapic.h>
#include <Kernel/Types.h>
#include <Kernel/Ints.h>
#include <Kernel/KDriver.h>
#include <Apic.h>
#include <Kernel/Return.h>
#include <Memory/PhysAlloc.h>
#include <Kernel/Scheduler.h>
#include <Kernel/SpinLock.h>
#include <Kernel/Paging.h>
#include <Time/Timer.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Task.h>

/*
 * ============================================================================
 * DMA Constants
 * ============================================================================
 */
#define PATA_CMD_READ_DMA       0xC8
#define PATA_CMD_WRITE_DMA      0xCA
#define PATA_CMD_READ_DMA_EXT   0x25
#define PATA_CMD_WRITE_DMA_EXT  0x35
#define PATA_CMD_PACKET         0xA0

// Bus Master Registers (offset from BAR4)
#define BMCR        0x00    // Command Register
#define BMSR        0x02    // Status Register
#define BMIDETBL    0x04    // PRDT Address (physical)

#define BMCR_START  (1 << 0)
#define BMCR_READ   (1 << 3)  // 0 = write, 1 = read
#define BMCR_RESET  (1 << 2)

#define BMSR_DRQ    (1 << 2)
#define BMSR_ERROR  (1 << 1)
#define BMSR_INTR   (1 << 0)

// PRDT entry
typedef struct {
    UINT32 PhysAddr;
    UINT16 ByteCount;
    UINT16 EndOfTable;
} ATTRIBUTE(packed) PrdtEntry;

#define PRDT_ENTRY_COUNT 16
#define PRDT_EOT         0x8000

// Max sector count per DMA transfer (PRDT_ENTRY_COUNT * 0xFFFE / 512)
#define MAX_DMA_SECTORS   (PRDT_ENTRY_COUNT * 63)

/*
 * ============================================================================
 * Request Queue
 * ============================================================================
 */
typedef enum {
    PATA_OP_NONE,
    PATA_OP_READ,
    PATA_OP_WRITE
} PataOperation;

typedef struct PataRequest {
    struct PataRequest *Next;
    PataOperation Op;
    UINT64 Lba;
    UINT32 Count;
    NOPTR *Buffer;
    UINT32 Result;
    BOOL Completed;
    SpinLock Lock;
    KTask *WaitingThread;
} PataRequest;

/*
 * ============================================================================
 * Global State
 * ============================================================================
 */
static volatile PataDrive* PrimaryIrqDrive = NULLPTR;
static volatile PataDrive* SecondaryIrqDrive = NULLPTR;

static PataRequest *PrimaryRequestQueue = NULLPTR;
static PataRequest *PrimaryRequestTail = NULLPTR;
static PataRequest *SecondaryRequestQueue = NULLPTR;
static PataRequest *SecondaryRequestTail = NULLPTR;

static SpinLock GPrimaryQueueLock;
static SpinLock GSecondaryQueueLock;

/*
 * ============================================================================
 * Helper Functions
 * ============================================================================
 */
static inline NOPTR IoDelay(UINT16 CtrlPort) {
    Inb(CtrlPort + PATA_ALTSTATUS);
    Inb(CtrlPort + PATA_ALTSTATUS);
    Inb(CtrlPort + PATA_ALTSTATUS);
    Inb(CtrlPort + PATA_ALTSTATUS);
}

NOPTR PataDisableInterrupts(UINT16 CtrlPort) {
    Outb(CtrlPort + PATA_CONTROL, 0x02);
}

NOPTR PataEnableInterrupts(UINT16 CtrlPort) {
    Outb(CtrlPort + PATA_CONTROL, 0x00);
}

static INT WaitBsyClear(UINT16 BasePort, UINT16 CtrlPort, UINT32 Timeout) {
    for (UINT32 I = 0; I < Timeout; ++I) {
        UINT8 S = Inb(BasePort + PATA_STATUS);
        if (!(S & PATA_STATUS_BSY))
            return 0;
        if ((I & 0xFF) == 0)
            IoDelay(CtrlPort);
    }
    return -TIMEOUT;
}

static INT WaitDrqOrErr(UINT16 BasePort, UINT16 CtrlPort, UINT32 Timeout) {
    for (UINT32 I = 0; I < Timeout; ++I) {
        UINT8 S = Inb(BasePort + PATA_STATUS);
        if (S & PATA_STATUS_ERR)
            return -DEVICE_ERROR;
        if (!(S & PATA_STATUS_BSY) && (S & PATA_STATUS_DRQ))
            return 0;
        if ((I & 0xFF) == 0)
            IoDelay(CtrlPort);
    }
    return -TIMEOUT;
}

static INT CheckErrAndClear(UINT16 BasePort) {
    UINT8 S = Inb(BasePort + PATA_STATUS);
    if (S & PATA_STATUS_ERR) {
        Inb(BasePort + PATA_ERROR);
        return -DEVICE_ERROR;
    }
    return 0;
}

static NOPTR SelectDeviceAndDelay(UINT16 Base, UINT16 Ctrl, UINT8 Drive, INT LbaFlag, UINT8 HeadHigh4) {
    UINT8 Value = (LbaFlag ? 0xE0 : 0xA0) | ((Drive & 1) << 4) | (HeadHigh4 & 0x0F);
    Outb(Base + PATA_SELECT, Value);
    IoDelay(Ctrl);
}

static UINT64 IdentWordsToUINT64(const UINT16 Ident[256], INT W) {
    UINT64 V = 0;
    V |= (UINT64)Ident[W + 0];
    V |= (UINT64)Ident[W + 1] << 16;
    V |= (UINT64)Ident[W + 2] << 32;
    V |= (UINT64)Ident[W + 3] << 48;
    return V;
}

static inline INT IsAligned2(const NOPTR *Ptr) {
    return (((UINTPTR)Ptr) & 1u) == 0;
}

static NOPTR ReadSectorWordsTo(UINT16 Base, UINT16 AlignedWords[256]) {
    for (INT I = 0; I < 256; ++I)
        AlignedWords[I] = Inw(Base + PATA_DATA);
}

/*
 * ============================================================================
 * DMA PRDT Management
 * ============================================================================
 */
static PrdtEntry* PataAllocPrdt(NOPTR *PhysBuffer, UINT32 SectorCount, UINT16 *PrdtPhys) {
    UINT32 EntriesNeeded = (SectorCount + 62) / 63;  // 63 sectors max per PRDT entry
    if (EntriesNeeded > PRDT_ENTRY_COUNT)
        return NULLPTR;
    
    // Allocate PRDT (one page)
    PrdtEntry *Prdt = (PrdtEntry*)PhysAllocAllocatePage(PhysAllocGet());
    if (!Prdt)
        return NULLPTR;
    
    *PrdtPhys = (UINT16)(UINTPTR)VirtToPhysPtr(Prdt);
    
    // Fill PRDT entries
    UINT32 RemainingSectors = SectorCount;
    UINT32 CurrentSector = 0;
    
    for (UINT32 I = 0; I < EntriesNeeded; I++) {
        UINT32 SectorsThisEntry = (RemainingSectors > 63) ? 63 : RemainingSectors;
        UINT32 BytesThisEntry = SectorsThisEntry * 512;
        
        UINT64 PhysAddr = (UINT64)VirtToPhysPtr((UINT8*)PhysBuffer + CurrentSector * 512);
        
        Prdt[I].PhysAddr = (UINT32)PhysAddr;
        Prdt[I].ByteCount = BytesThisEntry - 1;  // 0-based
        Prdt[I].EndOfTable = (I == EntriesNeeded - 1) ? PRDT_EOT : 0;
        
        RemainingSectors -= SectorsThisEntry;
        CurrentSector += SectorsThisEntry;
    }
    
    return Prdt;
}

/*
 * ============================================================================
 * LBA Register Setup (reused from PIO)
 * ============================================================================
 */
static NOPTR SetupLba28Regs(UINT16 Base, UINT16 Ctrl, UINT32 Lba, UINT8 Count, UINT8 Drive) {
    SelectDeviceAndDelay(Base, Ctrl, Drive, 1, (UINT8)((Lba >> 24) & 0x0F));
    Outb(Base + PATA_NSECT, Count);
    Outb(Base + PATA_SECTOR, (UINT8)(Lba & 0xFF));
    Outb(Base + PATA_LCYL, (UINT8)((Lba >> 8) & 0xFF));
    Outb(Base + PATA_HCYL, (UINT8)((Lba >> 16) & 0xFF));
}

static NOPTR SetupLba48Regs(UINT16 Base, UINT16 Ctrl, UINT64 Lba, UINT16 Count, UINT8 Drive) {
    SelectDeviceAndDelay(Base, Ctrl, Drive, 1, (UINT8)((Lba >> 24) & 0x0F));
    IoDelay(Ctrl);

    Outb(Base + PATA_NSECT, (UINT8)((Count >> 8) & 0xFF));
    Outb(Base + PATA_SECTOR, (UINT8)((Lba >> 24) & 0xFF));
    Outb(Base + PATA_LCYL, (UINT8)((Lba >> 32) & 0xFF));
    Outb(Base + PATA_HCYL, (UINT8)((Lba >> 40) & 0xFF));
    Outb(Base + PATA_NSECT, (UINT8)(Count & 0xFF));
    Outb(Base + PATA_SECTOR, (UINT8)(Lba & 0xFF));
    Outb(Base + PATA_LCYL, (UINT8)((Lba >> 8) & 0xFF));
    Outb(Base + PATA_HCYL, (UINT8)((Lba >> 16) & 0xFF));
}

/*
 * ============================================================================
 * PIO Fallback (when DMA fails)
 * ============================================================================
 */
static INT PataPioReadSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    UINT8 *UserBuf = (UINT8 *)Buffer;
    UINT32 Remaining = Count;
    UINT16 TmpSectorWords[256];

    while (Remaining--) {
        if (Drive->SupportsLba48 && (Lba > 0x0FFFFFFF)) {
            SetupLba48Regs(Drive->BasePort, Drive->CtrlPort, Lba, 1, Drive->Drive);
            Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_READ_SECTORS_EXT);
        } else {
            UINT32 Lba32 = (UINT32)Lba;
            SetupLba28Regs(Drive->BasePort, Drive->CtrlPort, Lba32, 1, Drive->Drive);
            Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_READ_SECTORS);
        }

        if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
            return -TIMEOUT;
        if (WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
            return -DEVICE_ERROR;

        UINT16 *Dest = (UINT16*)(UserBuf + (Count - Remaining - 1) * 512);
        if (IsAligned2(Dest)) {
            for (INT I = 0; I < 256; I++)
                Dest[I] = Inw(Drive->BasePort + PATA_DATA);
        } else {
            ReadSectorWordsTo(Drive->BasePort, TmpSectorWords);
            MemCpy(Dest, TmpSectorWords, 512);
        }
        
        Lba++;
    }
    return 0;
}

static INT PataPioWriteSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, const NOPTR *Buffer) {
    const UINT8 *UserBuf = (const UINT8 *)Buffer;
    UINT32 Remaining = Count;
    UINT16 TmpSectorWords[256];

    while (Remaining--) {
        if (Drive->SupportsLba48 && (Lba > 0x0FFFFFFF)) {
            SetupLba48Regs(Drive->BasePort, Drive->CtrlPort, Lba, 1, Drive->Drive);
            Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_WRITE_SECTORS_EXT);
        } else {
            UINT32 Lba32 = (UINT32)Lba;
            SetupLba28Regs(Drive->BasePort, Drive->CtrlPort, Lba32, 1, Drive->Drive);
            Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_WRITE_SECTORS);
        }

        if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
            return -TIMEOUT;
        if (WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
            return -DEVICE_ERROR;

        const UINT16 *Src = (const UINT16*)(UserBuf + (Count - Remaining - 1) * 512);
        if (IsAligned2(Src)) {
            for (INT I = 0; I < 256; I++)
                Outw(Drive->BasePort + PATA_DATA, Src[I]);
        } else {
            MemCpy(TmpSectorWords, Src, 512);
            for (INT I = 0; I < 256; I++)
                Outw(Drive->BasePort + PATA_DATA, TmpSectorWords[I]);
        }

        if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
            return -TIMEOUT;
        if (CheckErrAndClear(Drive->BasePort) != 0)
            return -DEVICE_ERROR;
        
        Lba++;
    }
    return 0;
}

/*
 * ============================================================================
 * DMA Operations
 * ============================================================================
 */
static INT PataDmaTransfer(PataDrive *Drive, PataOperation Op, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    if (!Drive->BusMasterBase || Count == 0)
        return -DEVICE_INVALID;
    
    if (Count > MAX_DMA_SECTORS)
        return -INCORRECT_VALUE;
    
    // Allocate PRDT
    UINT16 PrdtPhys;
    PrdtEntry *Prdt = PataAllocPrdt(Buffer, Count, &PrdtPhys);
    if (!Prdt)
        return -NO_MEMORY;
    
    // Reset DMA controller
    Outb(Drive->BusMasterBase + BMCR, BMCR_RESET);
    IoDelay(Drive->CtrlPort);
    
    // Set PRDT address
    Outl(Drive->BusMasterBase + BMIDETBL, PrdtPhys);
    
    // Setup LBA registers
    BOOL UseLba48 = Drive->SupportsLba48 && (Lba > 0x0FFFFFFF);
    UINT8 Command;
    
    if (UseLba48) {
        SetupLba48Regs(Drive->BasePort, Drive->CtrlPort, Lba, (UINT16)Count, Drive->Drive);
        Command = (Op == PATA_OP_READ) ? PATA_CMD_READ_DMA_EXT : PATA_CMD_WRITE_DMA_EXT;
    } else {
        SetupLba28Regs(Drive->BasePort, Drive->CtrlPort, (UINT32)Lba, (UINT8)Count, Drive->Drive);
        Command = (Op == PATA_OP_READ) ? PATA_CMD_READ_DMA : PATA_CMD_WRITE_DMA;
    }
    
    // Start DMA command
    Outb(Drive->BasePort + PATA_COMMAND, Command);
    IoDelay(Drive->CtrlPort);
    
    // Start DMA engine
    UINT8 Bmcr = BMCR_START;
    if (Op == PATA_OP_READ)
        Bmcr |= BMCR_READ;
    Outb(Drive->BusMasterBase + BMCR, Bmcr);
    
    // Wait for IRQ (completed in IRQ handler)
    
    return 0;  // Async, completes on IRQ
}

/*
 * ============================================================================
 * Request Queue Management
 * ============================================================================
 */
static NOPTR PataQueueRequest(PataDrive *Drive, PataRequest *Req) {
    SpinLock *Lock;
    PataRequest **Queue;
    PataRequest **Tail;
    
    if (Drive->Channel == PATA_CHANNEL_PRIMARY) {
        Lock = &GPrimaryQueueLock;
        Queue = &PrimaryRequestQueue;
        Tail = &PrimaryRequestTail;
    } else {
        Lock = &GSecondaryQueueLock;
        Queue = &SecondaryRequestQueue;
        Tail = &SecondaryRequestTail;
    }
    
    SpinLockAcquire(Lock);
    Req->Next = NULLPTR;
    if (*Tail) {
        (*Tail)->Next = Req;
    } else {
        *Queue = Req;
    }
    *Tail = Req;
    SpinLockRelease(Lock);
}

static PataRequest* PataDequeueRequest(PataDrive *Drive) {
    SpinLock *Lock;
    PataRequest **Queue;
    PataRequest **Tail;
    
    if (Drive->Channel == PATA_CHANNEL_PRIMARY) {
        Lock = &GPrimaryQueueLock;
        Queue = &PrimaryRequestQueue;
        Tail = &PrimaryRequestTail;
    } else {
        Lock = &GSecondaryQueueLock;
        Queue = &SecondaryRequestQueue;
        Tail = &SecondaryRequestTail;
    }
    
    SpinLockAcquire(Lock);
    PataRequest *Req = *Queue;
    if (Req) {
        *Queue = Req->Next;
        if (!*Queue)
            *Tail = NULLPTR;
    }
    SpinLockRelease(Lock);
    
    return Req;
}

/*
 * ============================================================================
 * IRQ completion (process completed requests)
 * ============================================================================
 */
static NOPTR PataCompleteIrqRequest(PataDrive *Drive) {
    if (!Drive) {
        return;
    }
    
    PataRequest *Req = PataDequeueRequest(Drive);
    if (!Req)
        return;
    
    // Check DMA status
    UINT8 Bmsr = Inb(Drive->BusMasterBase + BMSR);
    UINT8 Status = Inb(Drive->BasePort + PATA_STATUS);
    
    if ((Bmsr & BMSR_ERROR) || (Status & PATA_STATUS_ERR)) {
        // DMA failed, fallback to PIO
        if (Req->Op == PATA_OP_READ) {
            Req->Result = PataPioReadSectors(Drive, Req->Lba, Req->Count, Req->Buffer);
        } else {
            Req->Result = PataPioWriteSectors(Drive, Req->Lba, Req->Count, Req->Buffer);
        }
    } else {
        Req->Result = 0;
    }
    
    // Clear interrupt status
    Outb(Drive->BusMasterBase + BMSR, Bmsr | BMSR_INTR | BMSR_ERROR);
    
    Req->Completed = TRUE;
    
    if (Req->WaitingThread) {
        SchedulerWakeup(Req->WaitingThread);
    }
    
    return;
}

/*
 * ============================================================================
 * IRQ Handlers
 * ============================================================================
 */
NOPTR PataPrimaryIrqHandler(NOPTR) {
    if (PrimaryIrqDrive) {
        UINT8 Status = Inb(PATA_BASE_PRIMARY + PATA_STATUS);
        (NOPTR)Status;

        PrimaryIrqDrive->IrqPending = 1;
        PrimaryIrqDrive->IrqCount++;
    }

    PataCompleteIrqRequest((PataDrive *)PrimaryIrqDrive);
    ApicEoi();
}

NOPTR PataSecondaryIrqHandler(NOPTR) {
    if (SecondaryIrqDrive) {
        UINT8 Status = Inb(PATA_BASE_SECONDARY + PATA_STATUS);
        (NOPTR)Status;

        SecondaryIrqDrive->IrqPending = 1;
        SecondaryIrqDrive->IrqCount++;
    }

    PataCompleteIrqRequest((PataDrive *)SecondaryIrqDrive);
    ApicEoi();
}

/*
 * ============================================================================
 * Public API - Async I/O
 * ============================================================================
 */
INT PataReadSectorsAsync(PataDrive *Drive, UINT64 Lba, UINT32 Count, 
                          NOPTR *Buffer, PataRequest *Req) {
    if (!Drive || !Buffer || Count == 0)
        return -DEVICE_INVALID;
    
    if (Drive->TotalSectors && Lba + Count > Drive->TotalSectors)
        return -DEVICE_INVALID;
    
    MemSet(Req, 0, sizeof(PataRequest));
    Req->Op = PATA_OP_READ;
    Req->Lba = Lba;
    Req->Count = Count;
    Req->Buffer = Buffer;
    Req->Completed = FALSE;
    Req->Result = -1;
    Req->WaitingThread = SchedulerGetCurrent();
    SpinLockInit(&Req->Lock);
    
    // Try DMA first
    INT Result = PataDmaTransfer(Drive, PATA_OP_READ, Lba, Count, Buffer);
    if (Result != 0) {
        // DMA not available, use PIO synchronously
        Req->Result = PataPioReadSectors(Drive, Lba, Count, Buffer);
        Req->Completed = TRUE;
        return Req->Result;
    }
    
    // Queue request for IRQ completion
    PataQueueRequest(Drive, Req);
    return 0;  // Async, check Req->Completed later
}

INT PataWriteSectorsAsync(PataDrive *Drive, UINT64 Lba, UINT32 Count,
                           const NOPTR *Buffer, PataRequest *Req) {
    if (!Drive || !Buffer || Count == 0)
        return -DEVICE_INVALID;
    
    if (Drive->TotalSectors && Lba + Count > Drive->TotalSectors)
        return -DEVICE_INVALID;
    
    MemSet(Req, 0, sizeof(PataRequest));
    Req->Op = PATA_OP_WRITE;
    Req->Lba = Lba;
    Req->Count = Count;
    Req->Buffer = (NOPTR*)Buffer;
    Req->Completed = FALSE;
    Req->Result = -1;
    SpinLockInit(&Req->Lock);
    
    INT Result = PataDmaTransfer(Drive, PATA_OP_WRITE, Lba, Count, (NOPTR*)Buffer);
    if (Result != 0) {
        Req->Result = PataPioWriteSectors(Drive, Lba, Count, Buffer);
        Req->Completed = TRUE;
        return Req->Result;
    }
    
    PataQueueRequest(Drive, Req);
    return 0;
}
INT PataWaitRequest(PataRequest *Req, UINT32 TimeoutMs) {
    if (!Req)
        return -DEVICE_INVALID;
    
    UINT64 StartTicks = TimerTicks();
    UINT64 TimeoutTicks = (UINT64)TimeoutMs * TimerFreq() / 1000;
    
    while (!Req->Completed) {
        // Проверяем таймаут
        if (TimeoutMs > 0 && (TimerTicks() - StartTicks) >= TimeoutTicks) {
            return -TIMEOUT;
        }
        
        // Если запрос ещё не завершён - засыпаем
        if (!Req->Completed) {
            SchedulerSleep(&Req->Lock);
        }
    }
    
    return Req->Result;
}

/*
 * ============================================================================
 * Synchronous API (compatible with old code)
 * ============================================================================
 */
INT PataReadSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    PataRequest Req;
    INT Result = PataReadSectorsAsync(Drive, Lba, Count, Buffer, &Req);
    if (Result != 0)
        return Result;
    
    return PataWaitRequest(&Req, 5000);
}

INT PataWriteSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, const NOPTR *Buffer) {
    PataRequest Req;
    INT Result = PataWriteSectorsAsync(Drive, Lba, Count, Buffer, &Req);
    if (Result != 0)
        return Result;
    
    return PataWaitRequest(&Req, 5000);
}

/*
 * ============================================================================
 * IDENTIFY (unchanged, kept from original)
 * ============================================================================
 */
INT PataIdentify(PataDrive *Drive, UINT16 IdentBuffer[256]) {
    if (!Drive || !IdentBuffer)
        RETURN(DEVICE_INVALID);

    Drive->Type = PATA_TYPE_NONE;
    Drive->SupportsLba48 = 0;
    Drive->SectorSize = 512;
    Drive->TotalSectors = 0;
    Drive->IrqPending = 0;
    Drive->IrqCount = 0;

    /*
 * Device Selection (CHS)
 */
    SelectDeviceAndDelay(Drive->BasePort, Drive->CtrlPort, Drive->Drive, 0, 0);

    /*
 * Sending IDENTIFY
 */
    Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_IDENTIFY);
    IoDelay(Drive->CtrlPort);

    UINT8 Status = Inb(Drive->BasePort + PATA_STATUS);
    if (Status == 0)
        RETURN(DEVICE_ERROR);

    /*
 * If ERR - possible ATAPI
 */
    if (Status & PATA_STATUS_ERR) {
        UINT8 Cl = Inb(Drive->BasePort + PATA_LCYL);
        UINT8 Ch = Inb(Drive->BasePort + PATA_HCYL);
        if ((Cl == 0x14 && Ch == 0xEB) || (Cl == 0x69 && Ch == 0x96)) {
            Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_IDENTIFY_PACKET);
            if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != SUCCESS)
                RETURN(TIMEOUT);
            if (CheckErrAndClear(Drive->BasePort) != SUCCESS)
                RETURN(DEVICE_ERROR);
            if (WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != SUCCESS)
                RETURN(TIMEOUT);
            for (INT I = 0; I < 256; ++I)
                IdentBuffer[I] = Inw(Drive->BasePort + PATA_DATA);
            Drive->Type = PATA_TYPE_ATAPI;
            Drive->SectorSize = 2048;
            Drive->TotalSectors = 0;
            return SUCCESS;
        } else {
            RETURN(DEVICE_ERROR);
        }
    }

    if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != SUCCESS)
        RETURN(TIMEOUT);
    INT Rc = WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS);
    if (Rc != SUCCESS)
        return Rc;

    for (INT I = 0; I < 256; ++I)
        IdentBuffer[I] = Inw(Drive->BasePort + PATA_DATA);

    if (IdentBuffer[0] == 0)
        RETURN(DEVICE_ERROR);

    Drive->Type = PATA_TYPE_ATA;

    /*
 * LBA28 (words 60-61)
 */
    UINT32 Lba28 = ((UINT32)IdentBuffer[61] << 16) | IdentBuffer[60];
    Drive->TotalSectors = Lba28;

    /*
 * Checking LBA48 (word 83 bit 10)
 */
    if (IdentBuffer[83] & (1u << 10)) {
        Drive->SupportsLba48 = 1;
        UINT64 Lba48 = IdentWordsToUINT64(IdentBuffer, 100);
        if (Lba48 != 0)
            Drive->TotalSectors = Lba48;
    } else {
        Drive->SupportsLba48 = 0;
    }

    Drive->SectorSize = 512;
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Initialization
 * ============================================================================
 */
static INT PataDetectDMA(PataDrive *Drive, PciDevice *PciPata) {
    if (!PciPata || PciPata->BarSizes[4] == 0)
        return -NO_OBJECT;
    
    // BAR4 contains Bus Master Registers
    UINT32 Bar4 = (UINT32)Drive->PciDev->Bars[4];
    
    if (Bar4 & 1) {
        // I/O space
        Drive->BusMasterBase = Bar4 & 0xFFFC;
    } else {
        // Memory-mapped (rare for IDE)
        Drive->BusMasterBase = 0;
        return -NOT_SUPPORTED;
    }
    
    // Test DMA by resetting controller
    Outb(Drive->BusMasterBase + BMCR, BMCR_RESET);
    IoDelay(Drive->CtrlPort);
    
    UINT8 Test = Inb(Drive->BusMasterBase + BMCR);
    if (Test & ~BMCR_RESET) {
        // DMA appears to work
        return 0;
    }
    
    return -DEVICE_ERROR;
}

INT PataInit(PataDrive *Drive, PataChannel Channel, UINT8 Drive2) {
    if (!Drive || Drive2 > 1)
        return -DEVICE_INVALID;
    
    // Setup base ports (same as your original)
    if (Channel == PATA_CHANNEL_PRIMARY) {
        Drive->BasePort = PATA_BASE_PRIMARY;
        Drive->CtrlPort = PATA_CTRL_PRIMARY;
        PrimaryIrqDrive = Drive;
    } else {
        Drive->BasePort = PATA_BASE_SECONDARY;
        Drive->CtrlPort = PATA_CTRL_SECONDARY;
        SecondaryIrqDrive = Drive;
    }
    
    PataEnableInterrupts(Drive->CtrlPort);
    Drive->Drive = Drive2 & 1;
    Drive->Channel = Channel;
    Drive->BusMasterBase = 0;
    
    // Setup IRQ
    if (Channel == PATA_CHANNEL_PRIMARY)
        Drive->Irq = 14;
    else
        Drive->Irq = 15;
    
    // Find PCI device and enable bus mastering
    PciDevice* PciPata = PciFindClass(0x01, 0x01);
    if (PciPata) {
        Drive->Irq = PciPata->InterruptLine;
        Drive->PciDev = PciPata;
        PciEnableBusmaster(PciPata);
        
        // Detect DMA capabilities
        PataDetectDMA(Drive, PciPata);
    }
    
    // IOAPIC routing (same as your original)
    UINT32 Gsi, Flags;
    if (IoapicGetOverride(Drive->Irq, &Gsi, &Flags) != 0) {
        Gsi = Drive->Irq;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }
    
    UINT8 Vector = 32 + Drive->Irq;
    IoapicRedirectIrq(Gsi, Vector, ApicGetId(), Flags);
    IoapicUnmaskIrq(Gsi);
    
    // Setup IDT gate
    if (Channel == PATA_CHANNEL_PRIMARY) {
        IdtSetGate(Drive->Irq, PataPrimaryIrq, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    } else {
        IdtSetGate(Drive->Irq, PataSecondaryIrq, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    }
    
    // Initialize queues
    SpinLockInit(&GPrimaryQueueLock);
    SpinLockInit(&GSecondaryQueueLock);
    
    // Identify the drive
    UINT16 Ident[256];
    INT Rc = PataIdentify(Drive, Ident);
    if (Rc != 0)
        return Rc;
    
    KDriverRegister(KDriverGenerateStruct("PataDrive", DCL1, TRUE, NULLPTR, NULLPTR));
    
    return 0;
}

INT IdeWaitIrq(PataDrive *Drive, UINT32 TimeoutMs) {
    if (!Drive)
        return -DEVICE_INVALID;
    
    Drive->IrqPending = 0;
    
    for (UINT32 I = 0; I < TimeoutMs * 1000; I++) {
        if (Drive->IrqPending) {
            Drive->IrqPending = 0;
            return 0;
        }
        TimerUdelay(1);
    }
    
    return -TIMEOUT;
}

UINT8 IdeGetIrqCount(PataDrive *Drive) {
    return Drive ? Drive->IrqCount : 0;
}
