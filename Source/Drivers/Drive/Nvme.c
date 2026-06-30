#include <Drive/Nvme.h>
#include <Pci.h>
#include <Memory/PhysAlloc.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Kernel/Return.h>
#include <Kernel/Idt.h>
#include <Kernel/Scheduler.h>
#include <Apic.h>
#include <Ioapic.h>
#include <Asm/Cpu.h>
#include <Console.h>
#include <Kernel/Paging.h>

#define NVME_POLL_STEP_US       100
#define NVME_RESET_MS           500
#define NVME_READY_MS           3000
#define NVME_CMD_TIMEOUT_MS     5000
#define NVME_SECTORS_PER_IO     8
#define NVME_NS_PROBE_MAX       8
#define NVME_NS_PROBE_FAIL_STOP 3

static inline UINT64 NvmeDmaPhys(NOPTR *Virt) {
    return VirtToPhys((UINT64)(UINTPTR)Virt);
}

/* Каждый PRP очереди на границе NVME_DMA_ALIGN (страница CC.MPS). */
typedef struct {
    NvmeSqEntry Asq[NVME_ADMIN_Q_DEPTH];
    UINT8 PadAsq[NVME_DMA_ALIGN - sizeof(NvmeSqEntry) * NVME_ADMIN_Q_DEPTH];
    NvmeCqEntry Acq[NVME_ADMIN_Q_DEPTH];
    UINT8 PadAcq[NVME_DMA_ALIGN - sizeof(NvmeCqEntry) * NVME_ADMIN_Q_DEPTH];
    NvmeSqEntry IoSq[NVME_IO_Q_DEPTH];
    UINT8 PadIoSq[NVME_DMA_ALIGN - sizeof(NvmeSqEntry) * NVME_IO_Q_DEPTH];
    NvmeCqEntry IoCq[NVME_IO_Q_DEPTH];
    UINT8 PadIoCq[NVME_DMA_ALIGN - sizeof(NvmeCqEntry) * NVME_IO_Q_DEPTH];
    UINT8 Identify[NVME_IDENTIFY_SIZE];
    UINT8 PadIdentify[NVME_DMA_ALIGN - NVME_IDENTIFY_SIZE];
    UINT8 IoBuf[NVME_CMD_SLOTS][PAGE_SIZE];
} NvmeDmaPool;

_Static_assert(sizeof(NvmeSqEntry) * NVME_ADMIN_Q_DEPTH < NVME_DMA_ALIGN, "nvme asq");
_Static_assert(sizeof(NvmeCqEntry) * NVME_ADMIN_Q_DEPTH < NVME_DMA_ALIGN, "nvme acq");
_Static_assert(sizeof(NvmeSqEntry) * NVME_IO_Q_DEPTH < NVME_DMA_ALIGN, "nvme iosq");
_Static_assert(sizeof(NvmeCqEntry) * NVME_IO_Q_DEPTH < NVME_DMA_ALIGN, "nvme iocq");
_Static_assert(NVME_IDENTIFY_SIZE < NVME_DMA_ALIGN, "nvme identify");

static NvmeDmaPool GNvmeDma __attribute__((aligned(NVME_DMA_ALIGN)));

typedef struct {
    NvmeRegs *Regs;
    UINT32 DbStride;
    UINT32 MpsBytes;
    UINT32 IoQdepth;
    UINT16 AdminSqTail;
    UINT16 AdminCqHead;
    UINT8 AdminCqPhase;
    UINT16 IoSqTail;
    UINT16 IoCqHead;
    UINT8 IoCqPhase;
    UINT16 NextCid;
    BOOL IoQueueReady;
    BOOL Enabled;
} NvmeController;

static PciDevice *GNvmePciDev = NULLPTR;
static NvmeController GNvmeCtrl;
static NvmeNamespace GNvmeNamespaces[NVME_MAX_NAMESPACES];
static UINT32 GNvmeNsCount = 0;
static UINT32 GNvmeGsi = 0;
static BOOL GNvmeIrqReady = FALSE;
static BOOL GNvmeMsi = FALSE;

static UINT32 NvmeRegRead32(NvmeRegs *Regs, UINT32 Off) {
    volatile UINT32 *P = (volatile UINT32 *)((UINT8 *)Regs + Off);
    return *P;
}

static void NvmeRegWrite32(NvmeRegs *Regs, UINT32 Off, UINT32 Val) {
    volatile UINT32 *P = (volatile UINT32 *)((UINT8 *)Regs + Off);
    *P = Val;
}

static UINT64 NvmeRegRead64(NvmeRegs *Regs, UINT32 Off) {
    volatile UINT64 *P = (volatile UINT64 *)((UINT8 *)Regs + Off);
    return *P;
}

static void NvmeRegWrite64(NvmeRegs *Regs, UINT32 Off, UINT64 Val) {
    volatile UINT64 *P = (volatile UINT64 *)((UINT8 *)Regs + Off);
    *P = Val;
}

static volatile UINT32 *NvmeSqDb(NvmeController *Ctrl, UINT16 Qid) {
    UINT8 *Base = (UINT8 *)Ctrl->Regs + NVME_REG_DB_BASE;
    return (volatile UINT32 *)(Base + (UINT32)(2 * Qid) * Ctrl->DbStride);
}

static volatile UINT32 *NvmeCqDb(NvmeController *Ctrl, UINT16 Qid) {
    UINT8 *Base = (UINT8 *)Ctrl->Regs + NVME_REG_DB_BASE;
    return (volatile UINT32 *)(Base + (UINT32)(2 * Qid + 1) * Ctrl->DbStride);
}

static BOOL NvmePollMs(UINT32 TimeoutMs, BOOL (*Ready)(NvmeController *), NvmeController *Ctrl) {
    UINT32 Steps = TimeoutMs * 10;
    for (UINT32 I = 0; I < Steps; I++) {
        if (Ready(Ctrl)) {
            return TRUE;
        }
        TimerUdelay(NVME_POLL_STEP_US);
    }
    return FALSE;
}

static BOOL NvmeCstsReady(NvmeController *Ctrl) {
    return (NvmeRegRead32(Ctrl->Regs, NVME_REG_CSTS) & NVME_CSTS_RDY) != 0;
}

static BOOL NvmeCstsNotReady(NvmeController *Ctrl) {
    return (NvmeRegRead32(Ctrl->Regs, NVME_REG_CSTS) & NVME_CSTS_RDY) == 0;
}

static UINT16 NvmeAllocCid(NvmeController *Ctrl) {
    UINT16 Cid = Ctrl->NextCid;
    Ctrl->NextCid++;
    if (Ctrl->NextCid == 0) {
        Ctrl->NextCid = 1;
    }
    return Cid;
}

static void NvmeAdminRingDb(NvmeController *Ctrl) {
    __sync_synchronize();
    *NvmeSqDb(Ctrl, 0) = Ctrl->AdminSqTail;
}

static void NvmeAdminCqRingDb(NvmeController *Ctrl) {
    *NvmeCqDb(Ctrl, 0) = Ctrl->AdminCqHead;
}

static void NvmeAdminAdvanceCq(NvmeController *Ctrl) {
    Ctrl->AdminCqHead++;
    if (Ctrl->AdminCqHead >= NVME_ADMIN_Q_DEPTH) {
        Ctrl->AdminCqHead = 0;
        Ctrl->AdminCqPhase ^= 1;
    }
    NvmeAdminCqRingDb(Ctrl);
}

static void NvmeLogAdminStatus(const CHAR *Op, UINT16 RawStatus) {
    UINT8 Sct = (UINT8)((RawStatus >> 9) & 0x7);
    UINT8 Sc = (UINT8)((RawStatus >> 1) & 0xFF);
    ConsolePrint("[NVMe] %s: SCT=%u SC=0x%02X\n", Op, Sct, Sc);
}

static INT NvmeAdminPollCq(NvmeController *Ctrl, UINT16 ExpectCid, UINT32 TimeoutMs,
                           UINT16 *OutRawStatus) {
    UINT32 Steps = TimeoutMs * 10;
    for (UINT32 I = 0; I < Steps; I++) {
        __sync_synchronize();
        NvmeCqEntry *Cqe = &GNvmeDma.Acq[Ctrl->AdminCqHead];
        UINT16 Phase = (UINT16)(Cqe->Status & NVME_CQ_PHASE);
        if (Phase != Ctrl->AdminCqPhase) {
            TimerUdelay(NVME_POLL_STEP_US);
            continue;
        }
        if (Cqe->CommandId != ExpectCid) {
            TimerUdelay(NVME_POLL_STEP_US);
            continue;
        }
        UINT16 Raw = Cqe->Status;
        UINT16 StatusField = (Raw >> 1) & 0x7FFF;
        if (OutRawStatus) {
            *OutRawStatus = Raw;
        }
        NvmeAdminAdvanceCq(Ctrl);
        if (StatusField != 0) {
            return DEVICE_ERROR;
        }
        return SUCCESS;
    }
    return TIMEOUT;
}

static INT NvmeAdminSubmit(NvmeController *Ctrl, NvmeSqEntry *Template, UINT16 *OutRawStatus) {
    NvmeSqEntry *Sq = &GNvmeDma.Asq[Ctrl->AdminSqTail];
    MemCpy(Sq, Template, sizeof(NvmeSqEntry));
    Ctrl->AdminSqTail++;
    if (Ctrl->AdminSqTail >= NVME_ADMIN_Q_DEPTH) {
        Ctrl->AdminSqTail = 0;
    }
    NvmeAdminRingDb(Ctrl);
    return NvmeAdminPollCq(Ctrl, Template->CommandId, NVME_CMD_TIMEOUT_MS, OutRawStatus);
}

static INT NvmeAdminIdentify(NvmeController *Ctrl, UINT8 Cns, UINT32 Nsid) {
    UINT16 Cid = NvmeAllocCid(Ctrl);
    NvmeSqEntry E;
    MemSet(&E, 0, sizeof(E));
    E.Opcode = NVME_ADMIN_OPC_IDENTIFY;
    E.CommandId = Cid;
    E.Nsid = Nsid;
    E.Prp1 = NvmeDmaPhys((NOPTR *)GNvmeDma.Identify);
    E.Cdw10 = Cns;
    return NvmeAdminSubmit(Ctrl, &E, NULLPTR);
}

static BOOL NvmeCqStatusInvalidQid(UINT16 Raw) {
    return ((Raw >> 9) & 0x7) == 0 && ((Raw >> 1) & 0xFF) == 0x0C;
}

static INT NvmeAdminDeleteIoSq(NvmeController *Ctrl, UINT16 Qid) {
    UINT16 Raw = 0;
    NvmeSqEntry E;
    MemSet(&E, 0, sizeof(E));
    E.Opcode = NVME_ADMIN_OPC_DELETE_SQ;
    E.CommandId = NvmeAllocCid(Ctrl);
    E.Cdw10 = (UINT32)Qid << 16;
    INT Ret = NvmeAdminSubmit(Ctrl, &E, &Raw);
    if (Ret == DEVICE_ERROR && NvmeCqStatusInvalidQid(Raw)) {
        return SUCCESS;
    }
    return Ret;
}

static INT NvmeAdminDeleteIoCq(NvmeController *Ctrl, UINT16 Qid) {
    UINT16 Raw = 0;
    NvmeSqEntry E;
    MemSet(&E, 0, sizeof(E));
    E.Opcode = NVME_ADMIN_OPC_DELETE_CQ;
    E.CommandId = NvmeAllocCid(Ctrl);
    E.Cdw10 = (UINT32)Qid << 16;
    INT Ret = NvmeAdminSubmit(Ctrl, &E, &Raw);
    if (Ret == DEVICE_ERROR && NvmeCqStatusInvalidQid(Raw)) {
        return SUCCESS;
    }
    return Ret;
}

static INT NvmeAdminCreateIoCq(NvmeController *Ctrl) {
    UINT16 Raw = 0;
    NvmeSqEntry E;
    MemSet(&E, 0, sizeof(E));
    E.Opcode = NVME_ADMIN_OPC_CREATE_CQ;
    E.CommandId = NvmeAllocCid(Ctrl);
    E.Prp1 = NvmeDmaPhys((NOPTR *)GNvmeDma.IoCq);
    /* CDW10: 15:0 QID, 31:16 QS (0-based) — как Linux/SPDK */
    E.Cdw10 = (UINT32)NVME_IO_QUEUE_ID | ((Ctrl->IoQdepth - 1) << 16);
    /* CDW11: IV=0, PC=1, IEN=0 — polling */
    E.Cdw11 = NVME_CQ_FLAG_PC;
    INT Ret = NvmeAdminSubmit(Ctrl, &E, &Raw);
    if (Ret != SUCCESS) {
        NvmeLogAdminStatus("Create IO CQ", Raw);
    }
    return Ret;
}

static INT NvmeAdminCreateIoSq(NvmeController *Ctrl) {
    UINT16 Raw = 0;
    NvmeSqEntry E;
    MemSet(&E, 0, sizeof(E));
    E.Opcode = NVME_ADMIN_OPC_CREATE_SQ;
    E.CommandId = NvmeAllocCid(Ctrl);
    E.Prp1 = NvmeDmaPhys((NOPTR *)GNvmeDma.IoSq);
    /* CDW10: 15:0 QID, 31:16 QS (0-based) */
    E.Cdw10 = (UINT32)NVME_IO_QUEUE_ID | ((Ctrl->IoQdepth - 1) << 16);
    /* CDW11: 31:16 CQID, low 16 sq_flags (PC + QPRIO) */
    E.Cdw11 = ((UINT32)NVME_IO_QUEUE_ID << 16)
        | NVME_SQ_FLAG_PC | NVME_SQ_PRIO_NORMAL;
    INT Ret = NvmeAdminSubmit(Ctrl, &E, &Raw);
    if (Ret != SUCCESS) {
        NvmeLogAdminStatus("Create IO SQ", Raw);
    }
    return Ret;
}

static void NvmeResetHostQueues(NvmeController *Ctrl) {
    Ctrl->AdminSqTail = 0;
    Ctrl->AdminCqHead = 0;
    Ctrl->AdminCqPhase = 1;
    Ctrl->IoSqTail = 0;
    Ctrl->IoCqHead = 0;
    Ctrl->IoCqPhase = 1;
    Ctrl->NextCid = 1;
    Ctrl->IoQueueReady = FALSE;
}

static INT NvmeDisable(NvmeController *Ctrl) {
    UINT32 Cc = NvmeRegRead32(Ctrl->Regs, NVME_REG_CC);
    if (!(Cc & NVME_CC_EN)) {
        NvmeResetHostQueues(Ctrl);
        return SUCCESS;
    }
    Cc &= ~NVME_CC_EN;
    NvmeRegWrite32(Ctrl->Regs, NVME_REG_CC, Cc);
    if (!NvmePollMs(NVME_RESET_MS, NvmeCstsNotReady, Ctrl)) {
        return TIMEOUT;
    }
    Ctrl->Enabled = FALSE;
    NvmeResetHostQueues(Ctrl);
    return SUCCESS;
}

static INT NvmeEnable(NvmeController *Ctrl) {
    UINT64 Cap = NvmeRegRead64(Ctrl->Regs, NVME_REG_CAP);
    UINT32 Mqes = (UINT32)(Cap & 0xFFFF) + 1;
    if (NVME_ADMIN_Q_DEPTH > Mqes) {
        return NOT_SUPPORTED;
    }

    INT Ret = NvmeDisable(Ctrl);
    if (Ret != SUCCESS && Ret != TIMEOUT) {
        return Ret;
    }

    MemSet(&GNvmeDma.Asq, 0, sizeof(GNvmeDma.Asq));
    MemSet(&GNvmeDma.Acq, 0, sizeof(GNvmeDma.Acq));

    UINT32 Aqa = (NVME_ADMIN_Q_DEPTH - 1)
        | (((NVME_ADMIN_Q_DEPTH - 1) & NVME_AQA_ACQS_MASK) << NVME_AQA_ASQS_SHIFT);
    NvmeRegWrite32(Ctrl->Regs, NVME_REG_AQA, Aqa);
    NvmeRegWrite64(Ctrl->Regs, NVME_REG_ASQ, NvmeDmaPhys((NOPTR *)GNvmeDma.Asq));
    NvmeRegWrite64(Ctrl->Regs, NVME_REG_ACQ, NvmeDmaPhys((NOPTR *)GNvmeDma.Acq));

    UINT32 MpsMin = (UINT32)((Cap >> 48) & 0x0F);
    UINT32 CcMps = MpsMin;
    if (MpsMin == 0) {
        CcMps = 0;
    }
    Ctrl->MpsBytes = PAGE_SIZE << CcMps;

    /* CC.IOSQES/IOCQES: размер SQ/CQ entry = 2^n, n=6→64B, n=4→16B */
    UINT32 Cc = NVME_CC_EN | NVME_CC_CSS_NVM | (CcMps << 7)
        | NVME_CC_AMS_RR | NVME_CC_SHN_NONE
        | NVME_CC_IOSQES_64 | NVME_CC_IOCQES_16;
    NvmeRegWrite32(Ctrl->Regs, NVME_REG_CC, Cc);

    if (!NvmePollMs(NVME_READY_MS, NvmeCstsReady, Ctrl)) {
        return TIMEOUT;
    }
    if (NvmeRegRead32(Ctrl->Regs, NVME_REG_CSTS) & NVME_CSTS_CFS) {
        return DEVICE_ERROR;
    }

    Ctrl->Enabled = TRUE;
    Ctrl->AdminCqPhase = 1;
    return SUCCESS;
}

static INT NvmeSetupIoQueue(NvmeController *Ctrl) {
    UINT64 Cap = NvmeRegRead64(Ctrl->Regs, NVME_REG_CAP);
    UINT32 Mqes = (UINT32)(Cap & 0xFFFF) + 1;
    UINT32 Qdepth = NVME_IO_Q_DEPTH;
    if (Qdepth > Mqes) {
        Qdepth = Mqes;
    }
    if (Qdepth < 2) {
        return NOT_SUPPORTED;
    }
    Ctrl->IoQdepth = Qdepth;

    MemSet(&GNvmeDma.IoSq, 0, sizeof(GNvmeDma.IoSq));
    MemSet(&GNvmeDma.IoCq, 0, sizeof(GNvmeDma.IoCq));
    Ctrl->IoSqTail = 0;
    Ctrl->IoCqHead = 0;
    Ctrl->IoCqPhase = 1;

    NvmeAdminDeleteIoSq(Ctrl, NVME_IO_QUEUE_ID);
    NvmeAdminDeleteIoCq(Ctrl, NVME_IO_QUEUE_ID);

    INT Ret = NvmeAdminCreateIoCq(Ctrl);
    if (Ret != SUCCESS) {
        return Ret;
    }
    Ret = NvmeAdminCreateIoSq(Ctrl);
    if (Ret != SUCCESS) {
        NvmeAdminDeleteIoCq(Ctrl, NVME_IO_QUEUE_ID);
        return Ret;
    }
    Ctrl->IoQueueReady = TRUE;
    return SUCCESS;
}

static UINT32 NvmeNsSectorSize(const NvmeIdentifyNs *Id) {
    UINT8 Flbas = Id->Flbas & 0x0F;
    if (Flbas >= 16) {
        return NVME_SECTOR_SIZE_DEFAULT;
    }
    const UINT8 *Lbaf = Id->NlbafDesc[Flbas];
    UINT8 Lbads = Lbaf[2];
    if (Lbads == 0) {
        return NVME_SECTOR_SIZE_DEFAULT;
    }
    return 1u << Lbads;
}

static INT NvmeProbeNamespace(NvmeController *Ctrl, UINT32 Nsid) {
    if (GNvmeNsCount >= NVME_MAX_NAMESPACES) {
        return NO_MEMORY;
    }
    INT Ret = NvmeAdminIdentify(Ctrl, NVME_IDENTIFY_CNS_NS, Nsid);
    if (Ret != SUCCESS) {
        return Ret;
    }

    NvmeIdentifyNs *Id = (NvmeIdentifyNs *)GNvmeDma.Identify;
    if (Id->Nsize == 0) {
        return NOT_FOUND;
    }

    NvmeNamespace *Ns = &GNvmeNamespaces[GNvmeNsCount];
    MemSet(Ns, 0, sizeof(NvmeNamespace));
    Ns->Nsid = Nsid;
    Ns->TotalSectors = Id->Nsize;
    Ns->SectorSize = NvmeNsSectorSize(Id);
    Ns->Status = NVME_NS_ACTIVE;
    SpinLockInit(&Ns->NsLock);
    for (INT I = 0; I < NVME_CMD_SLOTS; I++) {
        Ns->SlotWait[I].Active = FALSE;
        Ns->SlotWait[I].Done = FALSE;
        Ns->SlotWait[I].Result = 0;
        Ns->SlotWait[I].Task = NULLPTR;
    }
    GNvmeNsCount++;
    return SUCCESS;
}

static INT NvmeEnumerateNamespaces(NvmeController *Ctrl) {
    INT Ret = NvmeAdminIdentify(Ctrl, NVME_IDENTIFY_CNS_CTRL, 0);
    if (Ret != SUCCESS) {
        return Ret;
    }

    UINT32 MaxNs = *(UINT32 *)(GNvmeDma.Identify + NVME_IDENTIFY_CTRL_NN_OFF);
    if (MaxNs == 0) {
        MaxNs = 1;
    }
    if (MaxNs > NVME_NS_PROBE_MAX) {
        MaxNs = NVME_NS_PROBE_MAX;
    }

    UINT32 FailStreak = 0;
    for (UINT32 Nsid = 1; Nsid <= MaxNs; Nsid++) {
        if (NvmeProbeNamespace(Ctrl, Nsid) == SUCCESS) {
            FailStreak = 0;
        } else {
            FailStreak++;
            if (FailStreak >= NVME_NS_PROBE_FAIL_STOP) {
                break;
            }
        }
    }
    if (GNvmeNsCount == 0) {
        return NOT_FOUND;
    }
    return SUCCESS;
}

static void NvmeIoRingSq(NvmeController *Ctrl) {
    __sync_synchronize();
    *NvmeSqDb(Ctrl, NVME_IO_QUEUE_ID) = Ctrl->IoSqTail;
}

static void NvmeIoRingCq(NvmeController *Ctrl) {
    *NvmeCqDb(Ctrl, NVME_IO_QUEUE_ID) = Ctrl->IoCqHead;
}

static INT NvmeFindIoSlot(NvmeNamespace *Ns) {
    for (INT I = 0; I < NVME_CMD_SLOTS; I++) {
        if (!Ns->SlotWait[I].Active) {
            return I;
        }
    }
    return -1;
}

static void NvmeCompleteIoSlot(NvmeNamespace *Ns, INT Slot, INT Result) {
    NvmeCmdWait *Wait = &Ns->SlotWait[Slot];
    if (!Wait->Active) {
        return;
    }
    Wait->Active = FALSE;
    Wait->Done = TRUE;
    Wait->Result = Result;
    if (Wait->Task) {
        SchedulerWakeup(Wait->Task);
    }
}

static void NvmeProcessIoCq(NvmeController *Ctrl) {
    for (;;) {
        NvmeCqEntry *Cqe = &GNvmeDma.IoCq[Ctrl->IoCqHead];
        UINT16 Phase = (UINT16)(Cqe->Status & NVME_CQ_PHASE);
        if (Phase != Ctrl->IoCqPhase) {
            break;
        }

        UINT16 Cid = Cqe->CommandId;
        UINT16 StatusField = (Cqe->Status >> 1) & 0x7FFF;
        INT Result = (StatusField == 0) ? SUCCESS : DEVICE_ERROR;

        if (Cid >= 1 && Cid <= NVME_CMD_SLOTS) {
            for (UINT32 I = 0; I < GNvmeNsCount; I++) {
                NvmeNamespace *Ns = &GNvmeNamespaces[I];
                NvmeCmdWait *Wait = &Ns->SlotWait[Cid - 1];
                if (Wait->Active) {
                    NvmeCompleteIoSlot(Ns, (INT)(Cid - 1), Result);
                    break;
                }
            }
        }

        Ctrl->IoCqHead++;
        if (Ctrl->IoCqHead >= NVME_IO_Q_DEPTH) {
            Ctrl->IoCqHead = 0;
            Ctrl->IoCqPhase ^= 1;
        }
        NvmeIoRingCq(Ctrl);
    }
}

static INT NvmeIoPollSlot(NvmeController *Ctrl, NvmeNamespace *Ns, INT Slot, UINT32 TimeoutMs) {
    NvmeCmdWait *Wait = &Ns->SlotWait[Slot];
    UINT64 StartTicks;
    UINT64 TimeoutTicks;
    UINT32 Step;
    
    Wait->Active = TRUE;
    Wait->Done = FALSE;
    Wait->Result = TIMEOUT;
    Wait->Task = SchedulerGetCurrent();
    
    StartTicks = TimerTicks();
    TimeoutTicks = (UINT64)TimeoutMs * TimerFreq() / 1000;
    if (TimeoutTicks == 0) TimeoutTicks = 1;
    
    // Пробуем polling первые 100 мс (быстрее чем IRQ)
    for (Step = 0; Step < 100; Step++) {
        NvmeProcessIoCq(Ctrl);
        if (Wait->Done) {
            goto done;
        }
        TimerUdelay(1000);  // 1ms
        if ((TimerTicks() - StartTicks) >= TimeoutTicks) {
            goto timeout;
        }
    }
    
    // Если есть IRQ и мы в контексте задачи — засыпаем
    if (GNvmeIrqReady && SchedulerGetCurrent()) {
        // Простой spinlock как флаг ожидания
        SpinLock WaitLock;
        SpinLockInit(&WaitLock);
        
        while (!Wait->Done) {
            if ((TimerTicks() - StartTicks) >= TimeoutTicks) {
                goto timeout;
            }
            // Засыпаем с таймаутом 10 мс
            // SchedulerSleep должен разбудить по IRQ
            SchedulerSleep(&WaitLock);
            // После пробуждения проверяем Completion Queue
            NvmeProcessIoCq(Ctrl);
        }
    } else {
        // Чистый polling
        while (!Wait->Done) {
            NvmeProcessIoCq(Ctrl);
            if ((TimerTicks() - StartTicks) >= TimeoutTicks) {
                goto timeout;
            }
            TimerUdelay(NVME_POLL_STEP_US);
        }
    }
    
done:
    Wait->Active = FALSE;
    Wait->Task = NULLPTR;
    return Wait->Result;
    
timeout:
    Wait->Active = FALSE;
    Wait->Task = NULLPTR;
    
    // Попытка сбросить очередь
    NvmeRegWrite32(Ctrl->Regs, NVME_REG_CC, 0);
    TimerMdelay(10);
    
    return TIMEOUT;
}

static INT NvmeIoSubmit(NvmeController *Ctrl, NvmeNamespace *Ns, INT Slot,
                        UINT8 Opcode, UINT64 Lba, UINT32 SectorCount,
                        UINTPTR PhysBuf) {
    if (!Ctrl->IoQueueReady) {
        return DEVICE_ERROR;
    }

    UINT16 Cid = (UINT16)(Slot + 1);
    NvmeSqEntry E;
    MemSet(&E, 0, sizeof(E));
    E.Opcode = Opcode;
    E.CommandId = Cid;
    E.Nsid = Ns->Nsid;
    
    
    E.Prp1 = NvmeDmaPhys((NOPTR *)PhysBuf);
    E.Cdw10 = (UINT32)(Lba & 0xFFFFFFFF);
    E.Cdw11 = (UINT32)(Lba >> 32);
    E.Cdw12 = SectorCount - 1;

    NvmeSqEntry *Sq = &GNvmeDma.IoSq[Ctrl->IoSqTail];
    MemCpy(Sq, &E, sizeof(E));
    Ctrl->IoSqTail++;
    if (Ctrl->IoSqTail >= NVME_IO_Q_DEPTH) {
        Ctrl->IoSqTail = 0;
    }
    NvmeIoRingSq(Ctrl);
    return NvmeIoPollSlot(Ctrl, Ns, Slot, NVME_CMD_TIMEOUT_MS);
}

NOPTR NvmeIrqHandler(NOPTR) {
    NvmeProcessIoCq(&GNvmeCtrl);
    ApicEoi();
    if (!GNvmeMsi && GNvmeGsi) {
        IoapicEoi(GNvmeGsi);
    }
}

static INT NvmeSetupIrq(NOPTR) {
    IdtSetGate(NVME_IRQ, NvmeIrqHandler, KERNEL_CODE_SEL, IDT_GATE_INT, 0);

    if (PciEnableMsi(GNvmePciDev, NVME_IRQ, ApicGetId()) == SUCCESS) {
        GNvmeMsi = TRUE;
        GNvmeGsi = 0;
        return SUCCESS;
    }

    UINT8 Irq = GNvmePciDev->InterruptLine;
    if (Irq == 0xFF) {
        return NOT_SUPPORTED;
    }

    UINT32 Flags;
    if (IoapicGetOverride(Irq, &GNvmeGsi, &Flags) != 0) {
        GNvmeGsi = Irq;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }

    if (IoapicRedirectIrq(GNvmeGsi, NVME_IRQ, ApicGetId(), Flags) != 0) {
        return IO_ERROR;
    }
    IoapicUnmaskIrq(GNvmeGsi);
    GNvmeMsi = FALSE;
    return SUCCESS;
}

static INT NvmeNsTransfer(NvmeNamespace *Ns, UINT64 Lba, UINT32 Count,
                          NOPTR *Buffer, INT Write) {
    if (!Ns || Ns->Status != NVME_NS_ACTIVE) {
        RETURN(INCORRECT_VALUE);
    }
    if (Count == 0) {
        RETURN(INCORRECT_VALUE);
    }

    SpinLockAcquire(&Ns->NsLock);

    UINT8 Opcode = Write ? NVME_NVM_OPC_WRITE : NVME_NVM_OPC_READ;
    UINT8 *Dst = (UINT8 *)Buffer;
    UINT64 Remaining = (Count + Ns->SectorSize - 1) / Ns->SectorSize;
    UINT64 CurLba = Lba;

    while (Remaining > 0) {
        UINT32 Chunk = Remaining > NVME_SECTORS_PER_IO
            ? NVME_SECTORS_PER_IO : (UINT32)Remaining;
        INT Slot = NvmeFindIoSlot(Ns);
        if (Slot < 0) {
            SpinLockRelease(&Ns->NsLock);
            RETURN(BUSY);
        }

        UINT32 Bytes = Chunk * Ns->SectorSize;
        UINT8 *SlotBuf = GNvmeDma.IoBuf[Slot];
        if (Write) {
            MemCpy(SlotBuf, Dst, Bytes);
        }

        INT Ret = NvmeIoSubmit(&GNvmeCtrl, Ns, Slot, Opcode, CurLba, Chunk,
                               (UINTPTR)SlotBuf);
        if (Ret != SUCCESS) {
            SpinLockRelease(&Ns->NsLock);
            RETURN(Ret);
        }

        if (!Write) {
            MemCpy(Dst, SlotBuf, Bytes);
        }
        Dst += Bytes;
        CurLba += Chunk;
        Remaining -= Chunk;
    }

    SpinLockRelease(&Ns->NsLock);
    RETURN(SUCCESS);
}

INT NvmeNsRead(NvmeNamespace *Ns, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    return NvmeNsTransfer(Ns, Lba, Count, Buffer, 0);
}

INT NvmeNsWrite(NvmeNamespace *Ns, UINT64 Lba, UINT32 Count, const NOPTR *Buffer) {
    return NvmeNsTransfer(Ns, Lba, Count, (NOPTR *)Buffer, 1);
}

NvmeNamespace *NvmeGetNamespace(UINT32 Nsid) {
    for (UINT32 I = 0; I < GNvmeNsCount; I++) {
        if (GNvmeNamespaces[I].Nsid == Nsid && GNvmeNamespaces[I].Status == NVME_NS_ACTIVE) {
            return &GNvmeNamespaces[I];
        }
    }
    return NULLPTR;
}

UINT32 NvmeGetNamespaceCount(NOPTR) {
    return GNvmeNsCount;
}

static INT NvmeControllerReset(NvmeController *Ctrl) {
    UINT64 Cap = NvmeRegRead64(Ctrl->Regs, NVME_REG_CAP);
    if ((Cap & (1ULL << 4)) == 0) {
        return SUCCESS;
    }
    NvmeRegWrite32(Ctrl->Regs, NVME_REG_NSSR, 0x4E564D65u);
    if (!NvmePollMs(NVME_RESET_MS, NvmeCstsNotReady, Ctrl)) {
        return TIMEOUT;
    }
    return SUCCESS;
}

INT NvmeInit(NOPTR) {
    MemSet(&GNvmeCtrl, 0, sizeof(GNvmeCtrl));
    MemSet(GNvmeNamespaces, 0, sizeof(GNvmeNamespaces));
    GNvmeNsCount = 0;
    GNvmeIrqReady = FALSE;
    GNvmeCtrl.NextCid = 1;
    GNvmeCtrl.AdminCqPhase = 1;
    GNvmeCtrl.IoCqPhase = 1;
    GNvmePciDev = PciFindClass(0x01, 0x08);
    if (!GNvmePciDev || GNvmePciDev->ProgIf != 0x02) {
        RETURN(NOT_FOUND);
    }
    PciEnable(GNvmePciDev);
    PciEnableBusmaster(GNvmePciDev);

    UINT64 Bar0 = GNvmePciDev->Bars[0];
    if (Bar0 == 0) {
        RETURN(NO_OBJECT);
    }
    GNvmeCtrl.Regs = (NvmeRegs *)(UINTPTR)(Bar0 & ~0xFULL);
    UINT64 Cap = NvmeRegRead64(GNvmeCtrl.Regs, NVME_REG_CAP);
    UINT32 Dstrd = (UINT32)((Cap >> 32) & 0x0F);
    GNvmeCtrl.DbStride = 4u << Dstrd;
    INT Ret = NvmeControllerReset(&GNvmeCtrl);
    if (Ret != SUCCESS && Ret != TIMEOUT) {
        RETURN(Ret);
    }
    Ret = NvmeEnable(&GNvmeCtrl);
    if (Ret != SUCCESS) {
        RETURN(Ret);
    }
    Ret = NvmeSetupIoQueue(&GNvmeCtrl);
    if (Ret != SUCCESS) {
        NvmeDisable(&GNvmeCtrl);
        RETURN(Ret);
    }
    Ret = NvmeEnumerateNamespaces(&GNvmeCtrl);
    if (Ret != SUCCESS) {
        NvmeDisable(&GNvmeCtrl);
        RETURN(Ret);
    }
    if (NvmeSetupIrq() == SUCCESS) {
        GNvmeIrqReady = TRUE;
    }

    RETURN(SUCCESS);
}
