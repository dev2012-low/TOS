#pragma once

#include <Kernel/Types.h>

// Инициализация Decon
NOPTR DeconInit(NOPTR);

// Запись в буфер (аналог ConsolePrint, но для дебага)
NOPTR DeconPrint(const CHAR *Fmt, ...);

// Очистка буфера
NOPTR DeconClear(NOPTR);

// Показать содержимое буфера
NOPTR DeconShow(NOPTR);

// Получить размер буфера
UINT32 DeconGetSize(NOPTR);

// Получить указатель на буфер (для сохранения)
const CHAR* DeconGetBuffer(NOPTR);