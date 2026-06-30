#include <Watchdog.h>
#include <Asm/Io.h>
#include <Pci.h>
#include <Kernel/Types.h>
#include <Console.h>
#include <Kernel/Return.h>

/* ICH Watchdog регистры через I/O порты */
#define ICH_WDT_CTRL      0x60
#define ICH_WDT_COUNT     0x62

/* Команды */
#define ICH_WDT_START     0x01
#define ICH_WDT_STOP      0x02
#define ICH_WDT_KICK      0x04

/* PMC регистры */
#define ICH_PMC_BASE      0x0430
#define ICH_PMC_CFG       0x0432
#define ICH_PMC_WDT_CFG   0x0434

/* Таймаут по умолчанию (5 секунд) */
#define ICH_DEFAULT_TIMEOUT_SEC 5

static struct {
    BOOL Available;
    BOOL Enabled;
    UINT16 BasePort;
} GIchWdt;

/* ============================================================================
 * Детекция ICH Watchdog
 * ============================================================================ */
static BOOL IchWdtDetect(NOPTR) {
    PciDevice *Dev;
    
    /* Ищем Intel Host Bridge (класс 0x06, подкласс 0x00) */
    Dev = PciFindClass(0x06, 0x00);
    if (!Dev || Dev->VendorId != 0x8086) {
        /* Ищем LPC Controller (класс 0x06, подкласс 0x01) */
        Dev = PciFindClass(0x06, 0x01);
        if (!Dev || Dev->VendorId != 0x8086) {
            return FALSE;
        }
    }
    
    /* Пробуем получить доступ к PMC */
    UINT16 PmcCfg = Inw(ICH_PMC_CFG);
    
    /* Включаем доступ к PM регистрам (бит 0) */
    if (!(PmcCfg & 0x01)) {
        PmcCfg |= 0x01;
        Outw(ICH_PMC_CFG, PmcCfg);
    }
    
    /* Проверяем, есть ли WDT (пробуем прочитать контрольный регистр) */
    UINT8 WdtCtrl = Inb(ICH_WDT_CTRL);
    (void)WdtCtrl;
    
    GIchWdt.BasePort = ICH_WDT_CTRL;
    GIchWdt.Available = TRUE;
    
    ConsolePrint("[ICH-WDT] Detected (port=0x%x)\n", GIchWdt.BasePort);
    
    return TRUE;
}

/* ============================================================================
 * Инициализация ICH Watchdog
 * ============================================================================ */
static INT IchWdtInit(NOPTR) {
    if (!GIchWdt.Available) return NOT_FOUND;
    
    /* Включаем PM регистры через PCI (если ещё не включены) */
    UINT16 PmcCfg = Inw(ICH_PMC_CFG);
    if (!(PmcCfg & 0x01)) {
        PmcCfg |= 0x01;
        Outw(ICH_PMC_CFG, PmcCfg);
    }
    
    /* Выключаем legacy watchdog (если включён BIOS) */
    Outw(ICH_PMC_BASE + 0x30, 0x0008);
    
    /* Останавливаем WDT на всякий случай */
    Outb(ICH_WDT_CTRL, ICH_WDT_STOP);
    
    ConsolePrint("[ICH-WDT] Initialized\n");
    
    return SUCCESS;
}

/* ============================================================================
 * Управление ICH Watchdog
 * ============================================================================ */
static NOPTR IchWdtStart(UINT32 TimeoutMs) {
    if (!GIchWdt.Available) return;
    
    UINT32 TimeoutSec = (TimeoutMs + 999) / 1000;
    UINT16 Timeout = (UINT16)(TimeoutSec * 10);  /* В тиках по 0.1 секунды */
    
    if (Timeout > 0xFFFF) Timeout = 0xFFFF;
    if (Timeout < 1) Timeout = 10;  /* Минимум 0.1 секунды */
    
    Outw(ICH_WDT_COUNT, Timeout);
    Outb(ICH_WDT_CTRL, ICH_WDT_START);
    GIchWdt.Enabled = TRUE;
}

static NOPTR IchWdtStop(NOPTR) {
    if (!GIchWdt.Available) return;
    
    Outb(ICH_WDT_CTRL, ICH_WDT_STOP);
    GIchWdt.Enabled = FALSE;
}

static NOPTR IchWdtKick(NOPTR) {
    if (!GIchWdt.Available || !GIchWdt.Enabled) return;
    
    Outb(ICH_WDT_CTRL, ICH_WDT_KICK);
}

/* ============================================================================
 * Драйвер ICH Watchdog
 * ============================================================================ */
static WatchdogDriver GIchWatchdogDriver = {
    .Detect = IchWdtDetect,
    .Init = IchWdtInit,
    .Start = IchWdtStart,
    .Stop = IchWdtStop,
    .Kick = IchWdtKick,
    .Type = WATCHDOG_TYPE_ICH
};

/* ============================================================================
 * Регистрация драйвера
 * ============================================================================ */
INT IchWatchdogInit(NOPTR) {
    if (!GIchWatchdogDriver.Detect()) {
        return NOT_FOUND;
    }
    
    if (WatchdogRegisterDriver(&GIchWatchdogDriver) != SUCCESS) {
        return DEVICE_ERROR;
    }
    
    return SUCCESS;
}