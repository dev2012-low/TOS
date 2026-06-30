#pragma once

#include <Kernel/Types.h>

#define DRIVE_NAME_MAX      32
#define DRIVE_LABEL_LEN     10
#define MAX_DRIVES          32

/* Типы дисков */
typedef enum {
    DRIVE_TYPE_UNKNOWN = 0,
    DRIVE_TYPE_PATA,
    DRIVE_TYPE_SATA,
    DRIVE_TYPE_NVME,
    DRIVE_TYPE_SCSI,
    DRIVE_TYPE_USB,
    DRIVE_TYPE_VIRTIO,
    DRIVE_TYPE_RAMDISK
} DriveType;

/* Структура диска */
typedef struct Drive {
    CHAR        Name[DRIVE_NAME_MAX];       /* Человеческое имя */
    CHAR        Label[DRIVE_LABEL_LEN + 1]; /* Строковая метка */
    UINT32      LabelNum;                   /* Числовая метка (указатель) */
    DriveType   Type;
    UINT32      SectorSize;
    UINT64      TotalSectors;
    NOPTR       *Priv;                      /* Приватные данные драйвера */
    
    /* Унифицированные функции */
    INT         (*Read)(struct Drive *Drive, UINT64 Lba, UINT32 Count, NOPTR *Buffer);
    INT         (*Write)(struct Drive *Drive, UINT64 Lba, UINT32 Count, const NOPTR *Buffer);
    INT         (*Sync)(struct Drive *Drive);
} Drive;

/* Инициализация менеджера дисков */
INT DriveManagerInit(NOPTR);

/* Инициализация драйверов (автоматически регистрируют диски) */
NOPTR DriveInitializePata(NOPTR);
NOPTR DriveInitializeSata(NOPTR);
NOPTR DriveInitializeNvme(NOPTR);
NOPTR DriveInitializeVirtio(NOPTR);

/* Регистрация диска */
INT DriveRegister(Drive *Drive);

/* Поиск дисков */
Drive *DriveGetByName(const CHAR *Name);
Drive *DriveGetByLabel(const CHAR *Label);
Drive *DriveGetByLabelNum(UINT32 LabelNum);
Drive *DriveGetByIndex(UINT32 Index);
UINT32 DriveGetCount(NOPTR);

/* Унифицированный интерфейс (по имени) */
INT DriveRead(const CHAR *Name, UINT64 Lba, UINT32 Count, NOPTR *Buffer);
INT DriveWrite(const CHAR *Name, UINT64 Lba, UINT32 Count, const NOPTR *Buffer);
INT DriveSync(const CHAR *Name);

/* Управление дисками */
INT DriveSetName(Drive *Drive, const CHAR *Name);
INT DriveSetNameByLabel(const CHAR *Label, const CHAR *Name);
UINT32 DriveGenerateLabel(NOPTR);

/* Вывод информации */
NOPTR DrivePrintInfo(NOPTR);
