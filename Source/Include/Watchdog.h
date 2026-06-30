#pragma once

#include <Kernel/Types.h>

/* Типы Watchdog */
typedef enum {
    WATCHDOG_TYPE_NONE = 0,
    WATCHDOG_TYPE_ICH,      /* Intel ICH */
    WATCHDOG_TYPE_HPET,     /* HPET (как таймер) */
    WATCHDOG_TYPE_ACPI      /* ACPI Watchdog (WDRT) */
} WatchdogType;

/* Статус Watchdog */
typedef struct {
    BOOL Available;
    BOOL Enabled;
    BOOL Running;
    WatchdogType Type;
    UINT32 TimeoutMs;
    UINT32 KickCount;
    UINT64 LastKickTick;
} WatchdogStatus;

/* Интерфейс драйвера Watchdog */
typedef struct {
    BOOL (*Detect)(NOPTR);
    INT (*Init)(NOPTR);
    NOPTR (*Start)(UINT32 TimeoutMs);
    NOPTR (*Stop)(NOPTR);
    NOPTR (*Kick)(NOPTR);
    WatchdogType Type;
} WatchdogDriver;

/* Публичные функции */
INT WatchdogInit(NOPTR);
INT WatchdogStart(UINT32 TimeoutMs);
NOPTR WatchdogStop(NOPTR);
NOPTR WatchdogKick(NOPTR);
WatchdogStatus* WatchdogGetStatus(NOPTR);
BOOL WatchdogIsAvailable(NOPTR);
INT WatchdogSetTimeout(UINT32 TimeoutMs);
UINT32 WatchdogGetTimeout(NOPTR);
NOPTR WatchdogTask(NOPTR *Arg);

/* Регистрация драйвера */
INT WatchdogRegisterDriver(WatchdogDriver *Driver);