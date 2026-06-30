#include <Serial.h>
#include <Asm/Io.h>
#include <Asm/Cpu.h>
#include <Kernel/KDriver.h>
#include <Kernel/Return.h>
#include <Time/Timer.h>
#include <Lib/String.h>

#define COM1_BASE           0x3F8
#define COM_LCR             5
#define COM_LSR             5
#define COM_LSR_THRE        0x20
#define COM_LSR_DR          0x01
#define COM_IER             1
#define COM_FCR             2
#define COM_MCR             4
#define COM_DLL             0
#define COM_DLM             1
#define COM_LCR_DLAB        0x80
#define COM_LCR_8N1         0x03

static struct {
    BOOL Ready;
    UINT16 Base;
    KDriver *Driver;
} GSerial = {0};

static inline UINT8 SerialIn(UINT16 Reg) {
    return Inb(GSerial.Base + Reg);
}

static inline NOPTR SerialOut(UINT16 Reg, UINT8 Val) {
    Outb(GSerial.Base + Reg, Val);
}

static BOOL SerialWaitTx(NOPTR) {
    for (INT I = 0; I < 100000; I++) {
        if (SerialIn(COM_LSR) & COM_LSR_THRE) {
            return TRUE;
        }
        CpuPause();
    }
    return FALSE;
}

INT SerialInit(NOPTR) {
    if (GSerial.Ready) {
        RETURN(SUCCESS);
    }

    GSerial.Base = COM1_BASE;

    SerialOut(COM_IER, 0x00);
    SerialOut(COM_LCR, COM_LCR_DLAB | COM_LCR_8N1);
    SerialOut(COM_DLL, 0x01);
    SerialOut(COM_DLM, 0x00);
    SerialOut(COM_LCR, COM_LCR_8N1);
    SerialOut(COM_FCR, 0xC7);
    SerialOut(COM_MCR, 0x0B);
    SerialOut(COM_IER, 0x01);

    GSerial.Ready = TRUE;
    GSerial.Driver = KDriverGenerateStruct("Serial16550", DCL2, TRUE, NULLPTR, NULLPTR);
    if (GSerial.Driver) {
        KDriverRegister(GSerial.Driver);
    }

    RETURN(SUCCESS);
}

BOOL SerialIsReady(NOPTR) {
    return GSerial.Ready;
}

INT SerialWriteByte(UINT8 Byte) {
    if (!GSerial.Ready) {
        RETURN(NOT_SUPPORTED);
    }
    if (!SerialWaitTx()) {
        RETURN(TIMEOUT);
    }
    SerialOut(COM_DLL, Byte);
    RETURN(SUCCESS);
}

INT SerialReadByte(UINT8 *Out) {
    if (!GSerial.Ready || !Out) {
        RETURN(INCORRECT_VALUE);
    }
    if (!(SerialIn(COM_LSR) & COM_LSR_DR)) {
        RETURN(NOT_FOUND);
    }
    *Out = SerialIn(COM_DLL);
    RETURN(SUCCESS);
}

INT SerialWrite(const CHAR *Str, USIZE Len) {
    USIZE I;

    if (!Str || Len == 0) {
        RETURN(SUCCESS);
    }
    for (I = 0; I < Len; I++) {
        if (IsError(SerialWriteByte((UINT8)Str[I])).IsError) {
            RETURN(DEVICE_ERROR);
        }
    }
    RETURN(SUCCESS);
}
