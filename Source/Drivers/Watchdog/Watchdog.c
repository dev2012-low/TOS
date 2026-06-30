#include <Watchdog.h>
#include <Kernel/KDriver.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Task.h>
#include <Kernel/SpinLock.h>
#include <Kernel/Return.h>
#include <Time/Timer.h>
#include <Console.h>
#include <Lib/String.h>

/* Внешние драйверы */
extern INT IchWatchdogInit(NOPTR);
extern INT AcpiWatchdogInit(NOPTR);

/* Максимум драйверов */
#define MAX_WATCHDOG_DRIVERS 4

/* Глобальное состояние */
static struct {
    BOOL Available;
    BOOL Enabled;
    BOOL Running;
    WatchdogDriver *Driver;
    WatchdogType Type;
    UINT32 TimeoutMs;
    UINT32 KickCount;
    UINT64 LastKickTick;
    KTask *WatchdogTask;
    SpinLock Lock;
} GWdt;

/* Список драйверов */
static WatchdogDriver *GDrivers[MAX_WATCHDOG_DRIVERS];
static UINT32 GDriverCount = 0;

/* ============================================================================
 * Регистрация драйвера
 * ============================================================================ */
INT WatchdogRegisterDriver(WatchdogDriver *Driver) {
    if (!Driver || GDriverCount >= MAX_WATCHDOG_DRIVERS) {
        return NO_MEMORY;
    }
    
    GDrivers[GDriverCount++] = Driver;
    
    return SUCCESS;
}

/* ============================================================================
 * Задача Watchdog (периодически кормит собаку)
 * ============================================================================ */
NOPTR WatchdogTask(NOPTR *Arg) {
    (void)Arg;
    
    while (GWdt.Running) {
        /* Ждём половину таймаута */
        if (GWdt.TimeoutMs > 0) {
            TimerSleep(GWdt.TimeoutMs / 2);
        } else {
            TimerSleep(1000);
        }
        
        if (!GWdt.Enabled || !GWdt.Driver) continue;
        
        /* Проверяем, жив ли планировщик */
        if (SchedulerGetCurrent() == NULLPTR) {
            /* Планировщик мёртв — перезагрузка через watchdog */
            for (;;) Halt();  /* watchdog сам перезагрузит */
        }
        
        WatchdogKick();
    }
    
    return;
}

/* ============================================================================
 * Публичные API
 * ============================================================================ */
INT WatchdogInit(NOPTR) {
    INT i;
    
    MemSet(&GWdt, 0, sizeof(GWdt));
    SpinLockInit(&GWdt.Lock);
    
    /* Инициализируем драйверы */
    IchWatchdogInit();
    AcpiWatchdogInit();
    
    /* Выбираем первый доступный драйвер */
    for (i = 0; i < GDriverCount; i++) {
        if (GDrivers[i] && GDrivers[i]->Init() == SUCCESS) {
            GWdt.Driver = GDrivers[i];
            GWdt.Type = GWdt.Driver->Type;
            GWdt.Available = TRUE;
            break;
        }
    }
    
    if (!GWdt.Available) {
        return NOT_FOUND;
    }
    
    /* Устанавливаем таймаут по умолчанию (5 секунд) */
    GWdt.TimeoutMs = 5000;
    
    /* Запускаем задачу watchdog */
    GWdt.Running = TRUE;
    GWdt.WatchdogTask = TaskCreate("WDT", WatchdogTask, NULLPTR,
                                    SCHED_PRIORITY_HIGH, TASK_DEFAULT_QUANTUM);
    if (GWdt.WatchdogTask) {
        SchedulerEnqueueReady(GWdt.WatchdogTask);
    }
    
    KDriverRegister(KDriverGenerateStruct("Watchdog", DCL1, TRUE, NULLPTR, NULLPTR));
    
    return SUCCESS;
}

INT WatchdogStart(UINT32 TimeoutMs) {
    SpinLockAcquire(&GWdt.Lock);
    
    if (!GWdt.Available || !GWdt.Driver) {
        SpinLockRelease(&GWdt.Lock);
        return NOT_FOUND;
    }
    
    if (TimeoutMs > 0) {
        GWdt.TimeoutMs = TimeoutMs;
    }
    
    if (GWdt.Driver->Start) {
        GWdt.Driver->Start(GWdt.TimeoutMs);
    }
    
    GWdt.Enabled = TRUE;
    GWdt.KickCount = 0;
    GWdt.LastKickTick = TimerTicks();
    
    SpinLockRelease(&GWdt.Lock);
    
    return SUCCESS;
}

NOPTR WatchdogStop(NOPTR) {
    SpinLockAcquire(&GWdt.Lock);
    
    if (!GWdt.Available || !GWdt.Driver || !GWdt.Enabled) {
        SpinLockRelease(&GWdt.Lock);
        return;
    }
    
    if (GWdt.Driver->Stop) {
        GWdt.Driver->Stop();
    }
    
    GWdt.Enabled = FALSE;
    
    SpinLockRelease(&GWdt.Lock);
}

NOPTR WatchdogKick(NOPTR) {
    SpinLockAcquire(&GWdt.Lock);
    
    if (!GWdt.Available || !GWdt.Driver || !GWdt.Enabled) {
        SpinLockRelease(&GWdt.Lock);
        return;
    }
    
    if (GWdt.Driver->Kick) {
        GWdt.Driver->Kick();
    }
    
    GWdt.KickCount++;
    GWdt.LastKickTick = TimerTicks();
    
    SpinLockRelease(&GWdt.Lock);
}

WatchdogStatus* WatchdogGetStatus(NOPTR) {
    static WatchdogStatus Status;
    
    SpinLockAcquire(&GWdt.Lock);
    
    Status.Available = GWdt.Available;
    Status.Enabled = GWdt.Enabled;
    Status.Running = GWdt.Running;
    Status.Type = GWdt.Type;
    Status.TimeoutMs = GWdt.TimeoutMs;
    Status.KickCount = GWdt.KickCount;
    Status.LastKickTick = GWdt.LastKickTick;
    
    SpinLockRelease(&GWdt.Lock);
    
    return &Status;
}

BOOL WatchdogIsAvailable(NOPTR) {
    return GWdt.Available;
}

INT WatchdogSetTimeout(UINT32 TimeoutMs) {
    if (!GWdt.Available || !GWdt.Driver) return NOT_FOUND;
    if (TimeoutMs < 100) TimeoutMs = 100;
    
    SpinLockAcquire(&GWdt.Lock);
    GWdt.TimeoutMs = TimeoutMs;
    
    /* Перезапускаем watchdog с новым таймаутом */
    if (GWdt.Enabled && GWdt.Driver->Start) {
        if (GWdt.Driver->Stop) GWdt.Driver->Stop();
        GWdt.Driver->Start(TimeoutMs);
    }
    
    SpinLockRelease(&GWdt.Lock);
    
    return SUCCESS;
}

UINT32 WatchdogGetTimeout(NOPTR) {
    return GWdt.TimeoutMs;
}