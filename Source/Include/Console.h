#pragma once

#include <Kernel/Types.h>

void ConsoleInit(void);
void ConsolePrint(const CHAR *fmt, ...);
void ConsoleClear(void);

// Новые функции
void ConsoleSetColor(UINT8 fg, UINT8 bg);
void ConsoleSetCursorPos(UINT32 x, UINT32 y);
void ConsoleGetCursorPos(UINT32 *x, UINT32 *y);
void ConsoleShowCursor(BOOL show);
void ConsoleScrollbackUp(UINT32 lines);
void ConsoleScrollbackDown(UINT32 lines);
void ConsoleResetScrollback(void);
UINT32 ConsoleGetCurrentLineNum(NOPTR);

/* Ввод с клавиатуры (PS/2), буфер + блокирующее чтение */
INT ConsoleInputAttach(NOPTR);
NOPTR ConsoleInputDetach(NOPTR);
BOOL ConsoleInputAvailable(NOPTR);
INT ConsoleTryReadChar(NOPTR);
INT ConsoleReadChar(NOPTR);
INT ConsoleReadLine(CHAR *Buf, USIZE Size);
NOPTR ConsoleSetInputEcho(BOOL Echo);
NOPTR ConsoleShowPrompt(NOPTR);
NOPTR ConsoleBeginCommandOutput(NOPTR);
NOPTR ConsoleEndCommandOutput(NOPTR);
BOOL ConsoleIsPromptActive(NOPTR);
NOPTR ConsoleService(NOPTR);
NOPTR RenderCursor(NOPTR);
NOPTR ConsoleSetEcho(BOOL State);
NOPTR ConsoleSwitch(BOOL On);
