#include <Watchdog.h>
#include <Acpi.h>
#include <AcpiTables.h>
#include <Asm/Mmio.h>
#include <Asm/Io.h>
#include <Console.h>
#include <Lib/String.h>
#include <Kernel/Return.h>

/* ACPI WDRT (Watchdog Resource Table) сигнатуры */
#define ACPI_WDRT_SIGNATURE "WDRT"
#define ACPI_WDRT_REVISION  1

/* Структура ACPI Watchdog Table */
typedef struct {
    SDTHeader Header;
    UINT32   TimerMaxCount;
    UINT32   TimerMinCount;
    UINT32   TimerPeriod;
    UINT16   Flags;
    UINT16   Reserved;
    UINT32   Count;
    UINT32   Reserved2;
    UINT32   Control;
    UINT32   Reserved3;
    UINT32   TimerValue;
    UINT32   Reserved4;
} ATTRIBUTE(packed) AcpiWdrt;

/* Флаги WDRT */
#define WDRT_FLAG_ENABLED       (1 << 0)
#define WDRT_FLAG_AUTO_DISABLE  (1 << 1)

/* Регистры ACPI Watchdog (MMIO) */
#define WDT_REG_CONTROL         0x00
#define WDT_REG_COUNT           0x04
#define WDT_REG_STATUS          0x08

/* Команды управления */
#define WDT_CTRL_STOP           0x00
#define WDT_CTRL_START          0x01
#define WDT_CTRL_KICK           0x02

static struct {
    BOOL Available;
    AcpiWdrt *Table;
    volatile UINT8 *MmioBase;
    UINT32 Gsi;
} GAcpiWdt;

/* ============================================================================
 * Детекция ACPI Watchdog
 * ============================================================================ */
static BOOL AcpiWdtDetect(NOPTR) {
    Acpi *AcpiTable = AcpiGetTable();
    SDTHeader *Header;
    
    if (!AcpiTable) return FALSE;
    
    /* Ищем таблицу WDRT */
    Header = (SDTHeader*)AcpiFindTable("WDRT");
    if (!Header) return FALSE;
    
    /* Проверяем контрольную сумму */
    if (AcpiChecksum(Header, Header->Length) != 0) {
        ConsolePrint("[ACPI-WDT] WDRT checksum failed\n");
        return FALSE;
    }
    
    GAcpiWdt.Table = (AcpiWdrt*)Header;
    GAcpiWdt.MmioBase = (volatile UINT8*)(UINTPTR)GAcpiWdt.Table->Control;
    
    ConsolePrint("[ACPI-WDT] Found WDRT table (MMIO=0x%p, max=%u, min=%u)\n",
                 GAcpiWdt.MmioBase,
                 GAcpiWdt.Table->TimerMaxCount,
                 GAcpiWdt.Table->TimerMinCount);
    
    return TRUE;
}

/* ============================================================================
 * Инициализация ACPI Watchdog
 * ============================================================================ */
static INT AcpiWdtInit(NOPTR) {
    if (!GAcpiWdt.Table) return NOT_FOUND;
    
    /* Проверяем, не запущен ли уже watchdog */
    if (GAcpiWdt.Table->Flags & WDRT_FLAG_ENABLED) {
        ConsolePrint("[ACPI-WDT] Watchdog already enabled by BIOS\n");
    }
    
    /* Останавливаем на всякий случай */
    MmioWrite32((volatile NOPTR*)(GAcpiWdt.MmioBase + WDT_REG_CONTROL), WDT_CTRL_STOP);
    
    /* Устанавливаем счётчик (по умолчанию 5 секунд) */
    UINT32 Ticks = 5 * GAcpiWdt.Table->TimerPeriod / 1000;  /* период в микросекундах? */
    if (Ticks < GAcpiWdt.Table->TimerMinCount) Ticks = GAcpiWdt.Table->TimerMinCount;
    if (Ticks > GAcpiWdt.Table->TimerMaxCount) Ticks = GAcpiWdt.Table->TimerMaxCount;
    
    MmioWrite32((volatile NOPTR*)(GAcpiWdt.MmioBase + WDT_REG_COUNT), Ticks);
    
    ConsolePrint("[ACPI-WDT] Initialized (period=%uus, ticks=%u)\n",
                 GAcpiWdt.Table->TimerPeriod, Ticks);
    
    return SUCCESS;
}

/* ============================================================================
 * Управление ACPI Watchdog
 * ============================================================================ */
static NOPTR AcpiWdtStart(UINT32 TimeoutMs) {
    if (!GAcpiWdt.MmioBase) return;
    
    /* Вычисляем счётчик */
    UINT32 PeriodUs = GAcpiWdt.Table->TimerPeriod;
    UINT32 Ticks = (TimeoutMs * 1000) / PeriodUs;
    
    if (Ticks < GAcpiWdt.Table->TimerMinCount) Ticks = GAcpiWdt.Table->TimerMinCount;
    if (Ticks > GAcpiWdt.Table->TimerMaxCount) Ticks = GAcpiWdt.Table->TimerMaxCount;
    
    MmioWrite32((volatile NOPTR*)(GAcpiWdt.MmioBase + WDT_REG_COUNT), Ticks);
    MmioWrite32((volatile NOPTR*)(GAcpiWdt.MmioBase + WDT_REG_CONTROL), WDT_CTRL_START);
}

static NOPTR AcpiWdtStop(NOPTR) {
    if (!GAcpiWdt.MmioBase) return;
    MmioWrite32((volatile NOPTR*)(GAcpiWdt.MmioBase + WDT_REG_CONTROL), WDT_CTRL_STOP);
}

static NOPTR AcpiWdtKick(NOPTR) {
    if (!GAcpiWdt.MmioBase) return;
    MmioWrite32((volatile NOPTR*)(GAcpiWdt.MmioBase + WDT_REG_CONTROL), WDT_CTRL_KICK);
}

/* ============================================================================
 * Драйвер ACPI Watchdog
 * ============================================================================ */
static WatchdogDriver GAcpiWatchdogDriver = {
    .Detect = AcpiWdtDetect,
    .Init = AcpiWdtInit,
    .Start = AcpiWdtStart,
    .Stop = AcpiWdtStop,
    .Kick = AcpiWdtKick,
    .Type = WATCHDOG_TYPE_ACPI
};

/* ============================================================================
 * Регистрация драйвера
 * ============================================================================ */
INT AcpiWatchdogInit(NOPTR) {
    if (!GAcpiWatchdogDriver.Detect()) {
        return NOT_FOUND;
    }
    
    if (WatchdogRegisterDriver(&GAcpiWatchdogDriver) != SUCCESS) {
        return DEVICE_ERROR;
    }
    
    return SUCCESS;
}