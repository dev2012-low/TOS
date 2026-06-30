#pragma once

#include <Kernel/Types.h>
#include <Kernel/SpinLock.h>
#include <Kernel/Task.h>

#define NVME_CMD_SLOTS          8
#define NVME_ADMIN_Q_DEPTH      32
#define NVME_IO_Q_DEPTH         32
#define NVME_IO_QUEUE_ID          1
#define NVME_MAX_NAMESPACES     32
#define NVME_SECTOR_SIZE_DEFAULT  512
#define NVME_IDENTIFY_SIZE        4096
#define NVME_IDENTIFY_CTRL_NN_OFF 0x200

/* Выравнивание PRP: 8 KiB покрывает CC.MPS 0 (4K) и 1 (8K). Не ставить 64K на
 * отдельные символы — линкер раздувает PT_LOAD и уносит Multiboot2 за 32K. */
#define NVME_DMA_ALIGN          8192u

#define NVME_REG_CAP            0x00
#define NVME_REG_VS             0x08
#define NVME_REG_INTMS          0x0C
#define NVME_REG_INTMC          0x10
#define NVME_REG_CC             0x14
#define NVME_REG_CSTS           0x1C
#define NVME_REG_NSSR           0x20
#define NVME_REG_AQA            0x24
#define NVME_REG_ASQ            0x28
#define NVME_REG_ACQ            0x30
#define NVME_REG_DB_BASE        0x1000

#define NVME_CC_EN              (1u << 0)
#define NVME_CC_CSS_NVM         (0u << 4)
#define NVME_CC_MPS_4K          (0u << 7)
#define NVME_CC_AMS_RR          (0u << 11)
#define NVME_CC_SHN_NONE        (0u << 14)
/* CC.IOSQES/IOCQES: размер записи = 2^n байт (n = значение поля). 6→64B, 4→16B. */
#define NVME_CC_IOSQES_64       (6u << 16)
#define NVME_CC_IOCQES_16       (4u << 20)
#define NVME_CAP_CQR            (1u << 16)
#define NVME_IDENTIFY_CTRL_SQES_OFF 512
#define NVME_IDENTIFY_CTRL_CQES_OFF 513
/* Create I/O CQ — CDW11 low 16: cq_flags (Linux/QEMU: PC=0, IEN=1); IV in 31:16 */
#define NVME_CQ_FLAG_PC         (1u << 0)
#define NVME_CQ_FLAG_IEN        (1u << 1)
/* Create I/O SQ — CDW11: low 16 sq_flags (PC=0), high 16 CQID; QPRIO in sq_flags 2:1 */
#define NVME_SQ_FLAG_PC         (1u << 0)
#define NVME_SQ_PRIO_URGENT     (0u << 1)
#define NVME_SQ_PRIO_HIGH       (1u << 1)
#define NVME_SQ_PRIO_NORMAL     (2u << 1)
#define NVME_SQ_PRIO_LOW        (3u << 1)

#define NVME_CSTS_RDY           (1u << 0)
#define NVME_CSTS_CFS           (1u << 1)

#define NVME_AQA_ACQS_MASK      0xFFF
#define NVME_AQA_ASQS_SHIFT     16

#define NVME_ADMIN_OPC_DELETE_SQ    0x00
#define NVME_ADMIN_OPC_CREATE_SQ    0x01
#define NVME_ADMIN_OPC_DELETE_CQ    0x04
#define NVME_ADMIN_OPC_CREATE_CQ    0x05
#define NVME_ADMIN_OPC_IDENTIFY     0x06
#define NVME_ADMIN_OPC_SET_FEATURES 0x09
#define NVME_ADMIN_OPC_GET_FEATURES 0x0A

#define NVME_NVM_OPC_FLUSH        0x00
#define NVME_NVM_OPC_WRITE        0x01
#define NVME_NVM_OPC_READ         0x02

#define NVME_IDENTIFY_CNS_NS      0x00
#define NVME_IDENTIFY_CNS_CTRL    0x01

#define NVME_CQ_PHASE             (1u << 0)

typedef volatile struct {
    UINT64 Cap;
    UINT32 Vs;
    UINT32 Intms;
    UINT32 Intmc;
    UINT32 Cc;
    UINT32 Rsvd;
    UINT32 Csts;
    UINT32 Nssr;
    UINT32 Aqa;
    UINT64 Asq;
    UINT64 Acq;
} ATTRIBUTE(packed) NvmeRegs;

typedef struct ATTRIBUTE(packed) NvmeSqEntry {
    UINT8 Opcode;
    UINT8 Flags;
    UINT16 CommandId;
    UINT32 Nsid;
    UINT64 Rsv2;
    UINT64 Mptr;
    UINT64 Prp1;
    UINT64 Prp2;
    UINT32 Cdw10;
    UINT32 Cdw11;
    UINT32 Cdw12;
    UINT32 Cdw13;
    UINT32 Cdw14;
    UINT32 Cdw15;
} NvmeSqEntry;

typedef struct ATTRIBUTE(packed) NvmeCqEntry {
    UINT32 Dword0;
    UINT32 Rsv;
    UINT16 SqHead;
    UINT16 SqId;
    UINT16 CommandId;
    UINT16 Status;
} NvmeCqEntry;

typedef struct ATTRIBUTE(packed) NvmeIdentifyCtrl {
    UINT16 Vid;
    UINT16 Ssvid;
    CHAR   Sn[20];
    CHAR   Mn[40];
    CHAR   Fr[8];
    UINT8  Rab;
    UINT8  Ieee[3];
    UINT8  Cmic;
    UINT8  Mdts;
    UINT16 Cntlid;
    UINT32 Ver;
    UINT32 Rtd3r;
    UINT32 Rtd3e;
    UINT32 Oaes;
    UINT32 Cmctr;
    UINT16 Ooncs;
    UINT16 Fuses;
    UINT8  Frmw;
    UINT8  Mec;
    UINT16 CntlidDup;
    UINT8  Oacs;
    UINT8  Acl;
    UINT8  Aerl;
    UINT8  Frmw2;
    UINT8  Lpa;
    UINT8  Elpe;
    UINT8  Npss;
    UINT8  Avscc;
    UINT8  Apsta;
    UINT16 Wctemp;
    UINT16 Cctemp;
    UINT16 Mtfa;
    UINT32 Hmpre;
    UINT32 Hmmin;
    UINT8  Tnvmcap[16];
    UINT8  Unvmcap[16];
    UINT32 Rpmbs;
    UINT16 Edstt;
    UINT8  Desc;
    UINT8  Rsv1[178];
    UINT16 Oacs2;
    UINT8  Rsv2[9];
    UINT8  Sqes;
    UINT8  Cqes;
    UINT16 Maxcmd;
    UINT32 Nn;
} NvmeIdentifyCtrl;

typedef struct ATTRIBUTE(packed) NvmeIdentifyNs {
    UINT64 Nsize;
    UINT64 Ncap;
    UINT64 Nuse;
    UINT8  Nsfeat;
    UINT8  Nlbaf;
    UINT8  Flbas;
    UINT8  Mc;
    UINT8  Dpc;
    UINT8  Dps;
    UINT8  Nmic;
    UINT8  Rescap;
    UINT8  Fpi;
    UINT8  Rsv1[2];
    UINT8  Anagrpid[4];
    UINT8  Rsv2[112];
    UINT8  NlbafDesc[16][4];
} NvmeIdentifyNs;

typedef enum {
    NVME_NS_UNINITIALIZED = 0,
    NVME_NS_ERROR = 1,
    NVME_NS_ACTIVE = 2,
} NvmeNsStatus;

typedef struct {
    volatile BOOL Active;
    volatile BOOL Done;
    volatile INT Result;
    KTask *Task;
} NvmeCmdWait;

typedef struct NvmeNamespace {
    UINT32 Nsid;
    NvmeNsStatus Status;
    UINT32 SectorSize;
    UINT64 TotalSectors;
    SpinLock NsLock;
    NvmeCmdWait SlotWait[NVME_CMD_SLOTS];
} NvmeNamespace;

INT NvmeInit(NOPTR);
INT NvmeNsRead(NvmeNamespace *Ns, UINT64 Lba, UINT32 Count, NOPTR *Buffer);
INT NvmeNsWrite(NvmeNamespace *Ns, UINT64 Lba, UINT32 Count, const NOPTR *Buffer);
NvmeNamespace *NvmeGetNamespace(UINT32 Nsid);
UINT32 NvmeGetNamespaceCount(NOPTR);
NOPTR NvmeIrqHandler(NOPTR);
