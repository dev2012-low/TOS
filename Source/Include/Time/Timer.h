#pragma once

#include <Kernel/Types.h>

/*
 * ============================================================================= Time structures ============================================================================
 */

typedef struct {
    UINT32 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
} DateTime;

typedef UINT64 UnixTime;

/*
 * ============================================================================= BCD / Unix time conversion ============================================================================
 */

UINT8 BcdToBin(UINT8 Bcd);
UINT8 BinToBcd(UINT8 Bin);
UnixTime TimeToUnix(DateTime *T);
NOPTR UnixToTime(UnixTime Ut, DateTime *T);

/*
 * ============================================================================= RTC ============================================================================
 */

INT RtcReadTime(DateTime *T);

/*
 * ============================================================================= APIC Timer ============================================================================
 */

NOPTR TimerInit(UINT32 Freq);
UINT64 TimerTicks(NOPTR);
UINT32 TimerFreq(NOPTR);
UINT32 TimerApicMs(NOPTR);
UINT32 TimerTicksPerMs(NOPTR);
UINT64 TimerTscFreq(NOPTR);

static inline UINT64 TimerMsToTicks(UINT32 Ms) {
    UINT32 Tpm = TimerTicksPerMs();
    if (Tpm == 0) {
        Tpm = 1;
    }
    return (UINT64)Ms * Tpm;
}

/*
 * ============================================================================= Delay functions (TSC-based) ============================================================================
 */

NOPTR TimerUdelay(UINT32 Us);
NOPTR TimerMdelay(UINT32 Ms);
NOPTR TimerSdelay(UINT32 S);
NOPTR TimerSleep(UINT32 Ms);

typedef enum {
    TIMER_SOURCE_HPET,
    TIMER_SOURCE_APIC,
    TIMER_SOURCE_PIT,
} TimerSource;
