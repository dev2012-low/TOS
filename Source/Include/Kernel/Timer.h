#pragma once

#include <Kernel/Types.h>

// Типы таймеров
typedef enum {
    TIMER_TYPE_NONE = 0,
    TIMER_TYPE_PIT,      // 8253 Programmable Interval Timer
    TIMER_TYPE_HPET,     // High Precision Event Timer
    TIMER_TYPE_APIC      // Local APIC Timer
} TimerType;

// Частота системного таймера (1000 Гц = 1 мс)
#define TIMER_FREQUENCY 1000
#define TIMER_MS_TO_TICKS(ms) ((ms) * TIMER_FREQUENCY / 1000)

// Функции для работы с таймером
INT TimerInit(NOPTR);
INT TimerInitFallback(NOPTR);  // Принудительно использовать PIT
TimerType TimerGetCurrentType(NOPTR);
UINT64 TimerGetTicks(NOPTR);   // Количество тиков с момента запуска
NOPTR TimerSleepMs(UINT32 Ms);
NOPTR TimerSleepUs(UINT32 Us);
NOPTR TimerWaitTicks(UINT64 Ticks);

// Регистрация обработчика тиков (вызывается в прерывании)
typedef NOPTR (*TimerCallback)(NOPTR);
NOPTR TimerRegisterCallback(TimerCallback Callback);
NOPTR TimerUnregisterCallback(NOPTR);

// Получить частоту таймера в Гц
UINT64 TimerGetFrequency(NOPTR);

// Busy-loop задержки (без прерываний)
NOPTR TimerBusyWaitUs(UINT32 Us);
NOPTR TimerBusyWaitMs(UINT32 Ms);