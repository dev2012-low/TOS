#pragma once

#include <Kernel/Types.h>

INT SerialInit(NOPTR);
BOOL SerialIsReady(NOPTR);
INT SerialWriteByte(UINT8 Byte);
INT SerialReadByte(UINT8 *Out);
INT SerialWrite(const CHAR *Str, USIZE Len);
