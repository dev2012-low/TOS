#include <AcpiEvents.h>
#include <Acpi.h>
#include <Kernel/Idt.h>
#include <Ioapic.h>
#include <Apic.h>
#include <Asm/Io.h>
#include <Asm/Mmio.h>
#include <Kernel/Return.h>
#include <Lib/String.h>
#include <Time/Timer.h>

EXTERN(Acpi, GAcpi);

#define PM1_PWRBTN_STS  (1 << 8)
#define PM1_SLPBTN_STS  (1 << 9)
#define PM1_RTC_STS     (1 << 10)
#define PM1_GBL_STS     (1 << 5)

static struct {
    UINT16 Pm1aEvt;
    UINT16 Pm1bEvt;
    UINT16 Pm1EvtLen;
    UINT32 Gpe0Blk;
    UINT32 Gpe1Blk;
    UINT8 Gpe0Len;
    UINT8 Gpe1Len;
    UINT8 Gpe1Base;
    UINT32 SciGsi;
    BOOL Initialized;
    BOOL SciEnabled;
} GAcpiEvents;

/* ============================================================================
 * БЕЗОПАСНЫЕ функции доступа к портам
 * ============================================================================ */

static UINT16 AcpiReadPm1(UINT16 Port) {
    if (!Port || Port == 0xFFFF) {
        return 0;
    }
    /* Проверяем, что порт в допустимом диапазоне (0x0000-0xFFFF) */
    if (Port < 0x100) {
        return Inw(Port);
    }
    return 0;
}

static NOPTR AcpiWritePm1(UINT16 Port, UINT16 Value) {
    if (!Port || Port == 0xFFFF) {
        return;
    }
    if (Port >= 0x100) {
        return;
    }
    Outw(Port, Value);
}

static UINT32 AcpiReadGpe32(UINT32 Base, UINT32 Offset) {
    if (!Base || Base == 0xFFFFFFFF) {
        return 0;
    }
    /* GPE блоки обычно в диапазоне 0x0000-0xFFFF (I/O space) */
    if (Base < 0x10000) {
        return Inl(Base + Offset);
    }
    return 0;
}

static NOPTR AcpiWriteGpe32(UINT32 Base, UINT32 Offset, UINT32 Value) {
    if (!Base || Base == 0xFFFFFFFF) {
        return;
    }
    if (Base >= 0x10000) {
        return;
    }
    Outl(Base + Offset, Value);
}

/* ============================================================================
 * БЕЗОПАСНОЕ включение SCI
 * ============================================================================ */

static NOPTR AcpiEnableSci(NOPTR) {
    UINT16 SmiCmd;
    UINT8 AcpiEnable;
    
    if (!GAcpi.Fadt) {
        return;
    }
    
    SmiCmd = (UINT16)GAcpi.Fadt->SmiCmd;
    AcpiEnable = GAcpi.Fadt->AcpiEnable;
    
    /* Проверяем, что SMI команда валидна */
    if (SmiCmd == 0 || SmiCmd == 0xFFFF) {
        return;
    }
    
    if (AcpiEnable == 0) {
        return;
    }
    
    /* Проверяем, что порт в допустимом диапазоне */
    if (SmiCmd >= 0x100) {
        return;
    }
    
    Outb(SmiCmd, AcpiEnable);
    TimerMdelay(10);
}

/* ============================================================================
 * Обработчик GPE (обновлен с защитой)
 * ============================================================================ */

NOPTR AcpiHandleGpe(NOPTR) {
    UINT32 Gpe0Sts = 0;
    UINT32 Gpe0En = 0;
    UINT32 Gpe1Sts = 0;
    UINT32 Gpe1En = 0;
    UINT32 Offset;

    /* Проверяем, что есть GPE блоки */
    if (!GAcpiEvents.Gpe0Blk && !GAcpiEvents.Gpe1Blk) {
        return;
    }

    /* Обрабатываем GPE0 */
    if (GAcpiEvents.Gpe0Blk && GAcpiEvents.Gpe0Len > 0) {
        Offset = GAcpiEvents.Gpe0Len / 2;
        if (Offset > 0 && Offset < 32) {
            Gpe0Sts = AcpiReadGpe32(GAcpiEvents.Gpe0Blk, 0);
            Gpe0En = AcpiReadGpe32(GAcpiEvents.Gpe0Blk, Offset);
            Gpe0Sts &= Gpe0En;
            
            if (Gpe0Sts) {
                AcpiWriteGpe32(GAcpiEvents.Gpe0Blk, 0, Gpe0Sts);
            }
        }
    }

    /* Обрабатываем GPE1 */
    if (GAcpiEvents.Gpe1Blk && GAcpiEvents.Gpe1Len > 0) {
        Offset = GAcpiEvents.Gpe1Len / 2;
        if (Offset > 0 && Offset < 32) {
            Gpe1Sts = AcpiReadGpe32(GAcpiEvents.Gpe1Blk, 0);
            Gpe1En = AcpiReadGpe32(GAcpiEvents.Gpe1Blk, Offset);
            Gpe1Sts &= Gpe1En;
            
            if (Gpe1Sts) {
                AcpiWriteGpe32(GAcpiEvents.Gpe1Blk, 0, Gpe1Sts);
            }
        }
    }
}

/* ============================================================================
 * ОБНОВЛЕННЫЙ обработчик SCI
 * ============================================================================ */

NOPTR AcpiSciHandler(NOPTR) {
    UINT16 Pm1aSts = 0;
    UINT16 Pm1bSts = 0;

    /* Проверяем, что инициализация прошла успешно */
    if (!GAcpiEvents.Initialized || !GAcpiEvents.SciEnabled) {
        goto eoi;
    }

    /* Читаем статусы ТОЛЬКО если порты валидны */
    if (GAcpiEvents.Pm1aEvt && GAcpiEvents.Pm1aEvt != 0xFFFF) {
        Pm1aSts = AcpiReadPm1(GAcpiEvents.Pm1aEvt);
    }
    if (GAcpiEvents.Pm1bEvt && GAcpiEvents.Pm1bEvt != 0xFFFF) {
        Pm1bSts = AcpiReadPm1(GAcpiEvents.Pm1bEvt);
    }

    /* Если статусов нет - пропускаем */
    if (Pm1aSts == 0 && Pm1bSts == 0) {
        goto eoi;
    }

    /* Очищаем статусы */
    if (Pm1aSts) {
        AcpiWritePm1(GAcpiEvents.Pm1aEvt, Pm1aSts);
    }
    if (Pm1bSts) {
        AcpiWritePm1(GAcpiEvents.Pm1bEvt, Pm1bSts);
    }

    /* Обрабатываем GPE */
    AcpiHandleGpe();

eoi:
    /* Отправляем EOI */
    ApicEoi();
    if (GAcpiEvents.SciGsi && GAcpiEvents.SciEnabled) {
        IoapicEoi(GAcpiEvents.SciGsi);
    }
}

EXTERN(NOPTR, AcpiSciIrq());

/* ============================================================================
 * НАСТРОЙКА SCI вектора (улучшенная безопасность)
 * ============================================================================ */

static INT AcpiSetupSciVector(NOPTR) {
    UINT16 SciInt;
    UINT32 Gsi;
    UINT32 Flags;
    INT Result;

    if (!GAcpi.Fadt) {
        RETURN(NO_OBJECT);
    }

    SciInt = GAcpi.Fadt->SciInt;
    
    /* Проверяем, что SCI IRQ валидный */
    if (SciInt == 0 || SciInt > 255) {
        RETURN(INCORRECT_VALUE);
    }

    /* Получаем GSI с учетом override */
    Result = IoapicGetOverride(SciInt, &Gsi, &Flags);
    if (IsError(Result).IsError) {
        Gsi = SciInt;
        Flags = IOAPIC_FLAG_LEVEL_TRIGGERED | IOAPIC_FLAG_ACTIVE_LOW;
    }

    GAcpiEvents.SciGsi = Gsi;

    /* Устанавливаем IDT вентиль */
    IdtSetGate(ACPI_SCI, AcpiSciIrq, KERNEL_CODE_SEL, IDT_GATE_INT, 0);

    /* Перенаправляем прерывание */
    Result = IoapicRedirectIrq(Gsi, ACPI_SCI, ApicGetId(), Flags);
    if (IsError(Result).IsError) {
        RETURN(Result);
    }

    /* Размаскируем прерывание */
    IoapicUnmaskIrq(Gsi);
    
    GAcpiEvents.SciEnabled = TRUE;
    RETURN(SUCCESS);
}

/* ============================================================================
 * ОСНОВНАЯ ФУНКЦИЯ ИНИЦИАЛИЗАЦИИ (полностью переработана)
 * ============================================================================ */

INT AcpiEventsInit(NOPTR) {
    FADT *Fadt;
    UINT16 Pm1aEvt, Pm1bEvt;
    UINT32 Gpe0Blk, Gpe1Blk;
    UINT8 Gpe0Len, Gpe1Len;
    
    if (GAcpiEvents.Initialized) {
        RETURN(SUCCESS);
    }

    Fadt = GAcpi.Fadt;
    if (!Fadt) {
        RETURN(NO_OBJECT);
    }
    
    /* Очищаем структуру */
    MemSet(&GAcpiEvents, 0, sizeof(GAcpiEvents));

    /* ============ PM1 EVENT БЛОКИ ============ */
    Pm1aEvt = (UINT16)Fadt->Pm1aEvtBlk;
    Pm1bEvt = (UINT16)Fadt->Pm1bEvtBlk;
    
    /* Для ACPI 3.0+ используем Extended версии */
    if (Fadt->Header.Revision >= 3) {
        UINT64 XPm1aEvt = *(UINT64*)Fadt->XPm1aEvtBlk;
        UINT64 XPm1bEvt = *(UINT64*)Fadt->XPm1bEvtBlk;
        if (XPm1aEvt) Pm1aEvt = (UINT16)XPm1aEvt;
        if (XPm1bEvt) Pm1bEvt = (UINT16)XPm1bEvt;
    }
    
    /* Проверяем и сохраняем PM1a */
    if (Pm1aEvt && Pm1aEvt != 0xFFFF && Pm1aEvt < 0x100) {
        GAcpiEvents.Pm1aEvt = Pm1aEvt;
    }
    
    /* Проверяем и сохраняем PM1b */
    if (Pm1bEvt && Pm1bEvt != 0xFFFF && Pm1bEvt < 0x100) {
        GAcpiEvents.Pm1bEvt = Pm1bEvt;
    }
    
    GAcpiEvents.Pm1EvtLen = Fadt->Pm1EvtLen;
    
    /* Если нет PM1 портов - нет смысла продолжать */
    if (!GAcpiEvents.Pm1aEvt && !GAcpiEvents.Pm1bEvt) {
        RETURN(NO_OBJECT);
    }

    /* ============ GPE БЛОКИ ============ */
    Gpe0Blk = Fadt->Gpe0Blk;
    Gpe1Blk = Fadt->Gpe1Blk;
    Gpe0Len = Fadt->Gpe0BlkLen;
    Gpe1Len = Fadt->Gpe1BlkLen;
    
    /* Для ACPI 3.0+ используем Extended версии */
    if (Fadt->Header.Revision >= 3) {
        UINT64 XGpe0 = *(UINT64*)Fadt->XGpe0Blk;
        UINT64 XGpe1 = *(UINT64*)Fadt->XGpe1Blk;
        if (XGpe0 && XGpe0 < 0x10000) Gpe0Blk = (UINT32)XGpe0;
        if (XGpe1 && XGpe1 < 0x10000) Gpe1Blk = (UINT32)XGpe1;
        /* GPE длины берутся из полей XGpe0BlkLen/XGpe1BlkLen в ACPI 3.0+ */
        if (Fadt->XGpe0BlkLen) Gpe0Len = Fadt->XGpe0BlkLen;
        if (Fadt->XGpe1BlkLen) Gpe1Len = Fadt->XGpe1BlkLen;
    }
    
    /* Проверяем GPE0 */
    if (Gpe0Blk && Gpe0Blk != 0xFFFFFFFF && Gpe0Blk < 0x10000 && Gpe0Len > 0) {
        GAcpiEvents.Gpe0Blk = Gpe0Blk;
        GAcpiEvents.Gpe0Len = Gpe0Len;
    }
    
    /* Проверяем GPE1 */
    if (Gpe1Blk && Gpe1Blk != 0xFFFFFFFF && Gpe1Blk < 0x10000 && Gpe1Len > 0) {
        GAcpiEvents.Gpe1Blk = Gpe1Blk;
        GAcpiEvents.Gpe1Len = Gpe1Len;
        GAcpiEvents.Gpe1Base = Fadt->Gpe1Base;
    }

    /* ============ ВКЛЮЧЕНИЕ SCI ============ */
    AcpiEnableSci();

    /* ============ НАСТРОЙКА SCI ВЕКТОРА ============ */
    /* Настраиваем SCI ТОЛЬКО если есть PM1 порты или GPE */
    if (GAcpiEvents.Pm1aEvt || GAcpiEvents.Pm1bEvt || GAcpiEvents.Gpe0Blk) {
        INT SciResult = AcpiSetupSciVector();
        if (IsError(SciResult).IsError) {
            /* Не возвращаем ошибку, пусть система работает без ACPI событий */
        }
    }

    GAcpiEvents.Initialized = TRUE;
    
    RETURN(SUCCESS);
}