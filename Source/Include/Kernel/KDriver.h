#pragma once

#include <Kernel/Types.h>
#include <Kernel/List.h>

// Driver critical levels
#define DCL0 0 // Driver Critical Level 0 - Critical
#define DCL1 1 // Driver Critical Level 1 - Ordinary
#define DCL2 2 // Driver Critical Level 2 - Additional

struct KDriver;

typedef NOPTR (*KDriverCallback)(struct KDriver *Self);

typedef struct KDriver {
    ListHead Node;
    CHAR Name[32];
    UINT8 DCL;
    BOOL Initialized;
    NOPTR *Priv;
    KDriverCallback Shutdown;
} KDriver;

NOPTR KDriverInit(NOPTR);
INT KDriverRegister(KDriver *Driver);
INT KDriverUnregister(KDriver *Driver);
KDriver* KDriverGenerateStruct(const CHAR *Name, UINT8 DCL, BOOL Initialized, NOPTR *Priv, KDriverCallback ShutdownCallback);
KDriver* KDriverFindByName(const CHAR *Name);
UINT32 KDriverGetCount(NOPTR);
NOPTR* KDriverGetPrivate(KDriver *Driver, USIZE Size);

/* If you're want to register driver by one line, use:
 *   KDriverRegister(KDriverGenerateStruct([YourName], [DCL], [Initialized], [Priv]));
 * Where replace [YourName] to your driver name, [DCL] to your DCL (UINT8) (0-2), [Initialized] to TRUE/FALSE, [Priv] to private data or NULLPTR
 */