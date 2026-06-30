#include <Drive/Drive.h>
#include <Kernel/Return.h>
#include <Kernel/SpinLock.h>
#include <Memory/Allocator.h>
#include <Drive/Pata.h>
#include <Drive/Sata.h>
#include <Drive/Nvme.h>
#include <Drive/Virtio.h>
#include <Console.h>
#include <Pci.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Kernel/UserAccount.h>
#include <Audit.h>

/* ============================================================================
 * Глобальные данные менеджера
 * ============================================================================ */

static Drive *GDrives[MAX_DRIVES];
static UINT32 GDriveCount = 0;
static SpinLock GDriveLock;
static UINT32 GNextLabel = 1000000000;

/* Старые массивы для обратной совместимости */
PataDrive PataDrives[4];

/* ============================================================================
 * Вспомогательные функции
 * ============================================================================ */

static void LabelNumToStr(UINT32 Num, CHAR *Buf) {
    for (INT I = DRIVE_LABEL_LEN - 1; I >= 0; I--) {
        Buf[I] = '0' + (Num % 10);
        Num /= 10;
    }
    Buf[DRIVE_LABEL_LEN] = '\0';
}

UINT32 DriveGenerateLabel(NOPTR) {
    UINT32 Label;
    
    Label = GNextLabel++;
    
    if (GNextLabel > 9999999999U) GNextLabel = 1000000000;
    
    return Label;
}

/* ============================================================================
 * Регистрация и поиск дисков
 * ============================================================================ */

INT DriveRegister(Drive *Drive) {
    if (!Drive) {
        return NO_OBJECT;
    }

    SpinLockAcquire(&GDriveLock);

    if (GDriveCount >= MAX_DRIVES) {
        SpinLockRelease(&GDriveLock);
        return NO_MEMORY;
    }
    
    /* Генерируем метку, если не задана */
    if (Drive->LabelNum == 0) {
        Drive->LabelNum = DriveGenerateLabel();
    }
    LabelNumToStr(Drive->LabelNum, Drive->Label);
    
    /* Имя по умолчанию, если не задано */
    if (Drive->Name[0] == '\0') {
        const CHAR *TypeStr = "Unknown";
        switch (Drive->Type) {
            case DRIVE_TYPE_PATA:  TypeStr = "PATA"; break;
            case DRIVE_TYPE_SATA:  TypeStr = "SATA"; break;
            case DRIVE_TYPE_NVME:  TypeStr = "NVMe"; break;
            case DRIVE_TYPE_SCSI:  TypeStr = "SCSI"; break;
            case DRIVE_TYPE_USB:   TypeStr = "USB"; break;
            case DRIVE_TYPE_VIRTIO: TypeStr = "VirtIO"; break;
            case DRIVE_TYPE_RAMDISK: TypeStr = "RAM"; break;
            default: break;
        }
        SnPrintf(Drive->Name, DRIVE_NAME_MAX, "%s_%s", TypeStr, Drive->Label);
    }
    
    GDrives[GDriveCount++] = Drive;
    SpinLockRelease(&GDriveLock);

    return SUCCESS;
}

Drive *DriveGetByName(const CHAR *Name) {
    if (!Name) return NULLPTR;
    
    SpinLockAcquire(&GDriveLock);
    for (UINT32 I = 0; I < GDriveCount; I++) {
        if (StrCmp(GDrives[I]->Name, Name) == 0) {
            SpinLockRelease(&GDriveLock);
            return GDrives[I];
        }
    }
    SpinLockRelease(&GDriveLock);
    return NULLPTR;
}

Drive *DriveGetByLabel(const CHAR *Label) {
    if (!Label) return NULLPTR;
    
    SpinLockAcquire(&GDriveLock);
    for (UINT32 I = 0; I < GDriveCount; I++) {
        if (StrCmp(GDrives[I]->Label, Label) == 0) {
            SpinLockRelease(&GDriveLock);
            return GDrives[I];
        }
    }
    SpinLockRelease(&GDriveLock);
    return NULLPTR;
}

Drive *DriveGetByLabelNum(UINT32 LabelNum) {
    SpinLockAcquire(&GDriveLock);
    for (UINT32 I = 0; I < GDriveCount; I++) {
        if (GDrives[I]->LabelNum == LabelNum) {
            SpinLockRelease(&GDriveLock);
            return GDrives[I];
        }
    }
    SpinLockRelease(&GDriveLock);
    return NULLPTR;
}

Drive *DriveGetByIndex(UINT32 Index) {
    if (Index >= GDriveCount) return NULLPTR;
    return GDrives[Index];
}

UINT32 DriveGetCount(NOPTR) {
    return GDriveCount;
}

INT DriveSetName(Drive *Drive, const CHAR *Name) {
    if (!Drive || !Name) return NO_OBJECT;
    if (StrLen(Name) >= DRIVE_NAME_MAX) return INCORRECT_VALUE;
    
    SpinLockAcquire(&GDriveLock);
    for (UINT32 I = 0; I < GDriveCount; I++) {
        if (GDrives[I] != Drive && StrCmp(GDrives[I]->Name, Name) == 0) {
            SpinLockRelease(&GDriveLock);
            ConsolePrint("[Drive] ERROR: Name '%s' already exists\n", Name);
            return ALREADY_EXISTS;
        }
    }
    StrCpy(Drive->Name, Name);
    SpinLockRelease(&GDriveLock);
    
    ConsolePrint("[Drive] Renamed to '%s' (label=%s)\n", Name, Drive->Label);
    AuditLog(AUDIT_LEVEL_DANGER, AUDIT_EVENT_DRIVE_RENAME, "Drive name was changed to '%s' by '%s'", Name, UserManagerGetSession()->Username);
    return SUCCESS;
}

INT DriveSetNameByLabel(const CHAR *Label, const CHAR *Name) {
    Drive *D = DriveGetByLabel(Label);
    if (!D) return NOT_FOUND;
    return DriveSetName(D, Name);
}

/* ============================================================================
 * Унифицированный интерфейс
 * ============================================================================ */

INT DriveRead(const CHAR *Name, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    Drive *D = DriveGetByName(Name);
    if (!D) {
        ConsolePrint("[Drive] Drive '%s' not found\n", Name);
        return NOT_FOUND;
    }
    if (!D->Read) {
        ConsolePrint("[Drive] Drive '%s' has no read function\n", Name);
        return NOT_IMPLEMENTED;
    }
    return D->Read(D, Lba, Count, Buffer);
}

INT DriveWrite(const CHAR *Name, UINT64 Lba, UINT32 Count, const NOPTR *Buffer) {
    Drive *D = DriveGetByName(Name);
    if (!D) {
        ConsolePrint("[Drive] Drive '%s' not found\n", Name);
        return NOT_FOUND;
    }
    if (!D->Write) {
        ConsolePrint("[Drive] Drive '%s' has no write function\n", Name);
        return NOT_IMPLEMENTED;
    }
    return D->Write(D, Lba, Count, Buffer);
}

INT DriveSync(const CHAR *Name) {
    Drive *D = DriveGetByName(Name);
    if (!D) return NOT_FOUND;
    if (!D->Sync) return NOT_IMPLEMENTED;
    return D->Sync(D);
}

/* ============================================================================
 * Вывод информации
 * ============================================================================ */

NOPTR DrivePrintInfo(NOPTR) {
    ConsolePrint("\n");
    ConsolePrint("Drives: %u\n", GDriveCount);
    ConsolePrint("------+------------------+------------+--------+------------\n");
    ConsolePrint("  #   | Name             | Label      | Type   | Size\n");
    ConsolePrint("------+------------------+------------+--------+------------\n");
    
    for (UINT32 I = 0; I < GDriveCount; I++) {
        Drive *D = GDrives[I];
        CHAR SizeStr[16];
        UINT64 Bytes = D->TotalSectors * D->SectorSize;
        
        if (Bytes >= 1024ULL * 1024 * 1024) {
            SnPrintf(SizeStr, sizeof(SizeStr), "%llu GB", Bytes / (1024ULL * 1024 * 1024));
        } else if (Bytes >= 1024 * 1024) {
            SnPrintf(SizeStr, sizeof(SizeStr), "%llu MB", Bytes / (1024ULL * 1024));
        } else if (Bytes >= 1024) {
            SnPrintf(SizeStr, sizeof(SizeStr), "%llu KB", Bytes / 1024);
        } else {
            SnPrintf(SizeStr, sizeof(SizeStr), "%llu B", Bytes);
        }
        
        const CHAR *TypeStr = "?";
        switch (D->Type) {
            case DRIVE_TYPE_PATA:  TypeStr = "PATA"; break;
            case DRIVE_TYPE_SATA:  TypeStr = "SATA"; break;
            case DRIVE_TYPE_NVME:  TypeStr = "NVMe"; break;
            case DRIVE_TYPE_SCSI:  TypeStr = "SCSI"; break;
            case DRIVE_TYPE_USB:   TypeStr = "USB"; break;
            case DRIVE_TYPE_VIRTIO: TypeStr = "VirtIO"; break;
            case DRIVE_TYPE_RAMDISK: TypeStr = "RAM"; break;
            default: break;
        }
        
        ConsolePrint("  %2u  | %-16s | %-10s | %-6s | %s\n",
                     I, D->Name, D->Label, TypeStr, SizeStr);
    }
    ConsolePrint("------+------------------+------------+--------+------------\n");
    ConsolePrint("\n");
}

/* ============================================================================
 * Инициализация менеджера
 * ============================================================================ */

INT DriveManagerInit(NOPTR) {
    GDriveCount = 0;
    MemSet(GDrives, 0, sizeof(GDrives));
    SpinLockInit(&GDriveLock);
    GNextLabel = 1000000000;
    return SUCCESS;
}

/* ============================================================================
 * PATA Driver
 * ============================================================================ */

static INT PataDriveRead(Drive *D, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    PataDrive *Pata = (PataDrive*)D->Priv;
    return PataReadSectors(Pata, Lba, Count, Buffer);
}

static INT PataDriveWrite(Drive *D, UINT64 Lba, UINT32 Count, const NOPTR *Buffer) {
    PataDrive *Pata = (PataDrive*)D->Priv;
    return PataWriteSectors(Pata, Lba, Count, Buffer);
}

static INT PataDriveSync(Drive *D) {
    (void)D;
    /* PATA flush command */
    return SUCCESS;
}

NOPTR DriveInitializePata(NOPTR) {
    ConsolePrint(" PATA drives found: ");
    INT FoundCount = 0;
    
    for (INT Ch = 0; Ch < 2; Ch++) {
        for (INT Dr = 0; Dr < 2; Dr++) {
            INT Idx = Ch * 2 + Dr;
            
            INT Result = PataInit(&PataDrives[Idx], (PataChannel)Ch, Dr);
            if (Result == SUCCESS && PataDrives[Idx].TotalSectors > 0) {
                Drive *D = (Drive*)MemoryAllocate(sizeof(Drive));
                if (D) {
                    MemSet(D, 0, sizeof(Drive));
                    D->Type = DRIVE_TYPE_PATA;
                    D->SectorSize = PataDrives[Idx].SectorSize;
                    D->TotalSectors = PataDrives[Idx].TotalSectors;
                    D->Priv = &PataDrives[Idx];
                    D->Read = PataDriveRead;
                    D->Write = PataDriveWrite;
                    D->Sync = PataDriveSync;
                    SnPrintf(D->Name, DRIVE_NAME_MAX, "PATA%d", Idx);
                    DriveRegister(D);
                }
                FoundCount++;
            }
        }
    }
    ConsolePrint("%d\n", FoundCount);
}

/* ============================================================================
 * SATA Driver
 * ============================================================================ */

static INT SataDriveRead(Drive *D, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    SataPort *Port = (SataPort*)D->Priv;
    return SataPortRead(Port, Lba, Count, Buffer);
}

static INT SataDriveWrite(Drive *D, UINT64 Lba, UINT32 Count, const NOPTR *Buffer) {
    SataPort *Port = (SataPort*)D->Priv;
    return SataPortWrite(Port, Lba, Count, (NOPTR*)Buffer);
}

static INT SataDriveSync(Drive *D) {
    (void)D;
    /* TODO: SATA flush command */
    return SUCCESS;
}

NOPTR DriveInitializeSata(NOPTR) {
    ConsolePrint(" SATA drives found: ");

    INT SataResult = SataInit();
    if (IsError(SataResult).IsError == TRUE) {
        ConsolePrint("0 (%s)\n", ReturnCode2String(SataResult));
        return;
    }

    INT FoundCount = 0;
    for (INT PortN = 0; PortN < 32; PortN++) {
        SataPort *Port = SataGetPort(PortN);
        if (Port && Port->Status == SATA_PORT_ACTIVE && Port->TotalSectors > 0) {
            Drive *D = (Drive*)MemoryAllocate(sizeof(Drive));
            if (D) {
                MemSet(D, 0, sizeof(Drive));
                D->Type = DRIVE_TYPE_SATA;
                D->SectorSize = Port->SectorSize;
                D->TotalSectors = Port->TotalSectors;
                D->Priv = Port;
                D->Read = SataDriveRead;
                D->Write = SataDriveWrite;
                D->Sync = SataDriveSync;
                SnPrintf(D->Name, DRIVE_NAME_MAX, "SATA%d", PortN);
                DriveRegister(D);
                FoundCount++;
            }
        }
    }
    ConsolePrint("%d\n", FoundCount);
}

/* ============================================================================
 * NVMe Driver
 * ============================================================================ */

static INT NvmeDriveRead(Drive *D, UINT64 Lba, UINT32 Count, NOPTR *Buffer) {
    NvmeNamespace *Ns = (NvmeNamespace*)D->Priv;
    return NvmeNsRead(Ns, Lba, Count, Buffer);
}

static INT NvmeDriveWrite(Drive *D, UINT64 Lba, UINT32 Count, const NOPTR *Buffer) {
    NvmeNamespace *Ns = (NvmeNamespace*)D->Priv;
    return NvmeNsWrite(Ns, Lba, Count, Buffer);
}

static INT NvmeDriveSync(Drive *D) {
    (void)D;
    /* NVMe flush command (можно добавить позже) */
    return SUCCESS;
}

NOPTR DriveInitializeNvme(NOPTR) {
    ConsolePrint(" NVMe namespaces found: ");

    INT NvmeResult = NvmeInit();
    if (IsError(NvmeResult).IsError == TRUE) {
        ConsolePrint("0 (%s)\n", ReturnCode2String(NvmeResult));
        return;
    }

    UINT32 FoundCount = NvmeGetNamespaceCount();
    for (UINT32 I = 0; I < FoundCount; I++) {
        NvmeNamespace *Ns = NvmeGetNamespace(I + 1);
        if (Ns && Ns->Status == NVME_NS_ACTIVE && Ns->TotalSectors > 0) {
            Drive *D = (Drive*)MemoryAllocate(sizeof(Drive));
            if (D) {
                MemSet(D, 0, sizeof(Drive));
                D->Type = DRIVE_TYPE_NVME;
                D->SectorSize = Ns->SectorSize;
                D->TotalSectors = Ns->TotalSectors;
                D->Priv = Ns;
                D->Read = NvmeDriveRead;
                D->Write = NvmeDriveWrite;
                D->Sync = NvmeDriveSync;
                SnPrintf(D->Name, DRIVE_NAME_MAX, "NVME%d", I);
                DriveRegister(D);
            }
        }
    }
    ConsolePrint("%u\n", FoundCount);
}

NOPTR DriveInitializeVirtio(NOPTR) {
    VirtioInit();
}
