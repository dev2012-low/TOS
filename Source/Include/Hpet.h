#pragma once

#include <Kernel/Types.h>

// Инициализация
INT HpetInit(NOPTR);
BOOL HpetIsAvailable(NOPTR);

// Чтение счётчика
UINT64 HpetReadCounter(NOPTR);
UINT64 HpetGetFrequency(NOPTR);
UINT64 HpetGetPeriod(NOPTR);
UINT64 HpetGetNanoseconds(NOPTR);
UINT64 HpetGetMicroseconds(NOPTR);

// Задержки
NOPTR HpetDelayUs(UINT32 Us);
NOPTR HpetDelayMs(UINT32 Ms);

// Калибровка APIC и TSC
UINT32 HpetCalibrateApicTimer(UINT32 DesiredMs);
UINT64 HpetCalibrateTsc(NOPTR);