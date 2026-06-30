#include <Console.h>
#include <FBDevice.h>
#include <RgbColor.h>
#include <Lib/StdIo.h>
#include <Lib/String.h>
#include <Memory/Allocator.h>
#include <Time/Timer.h>
#include <Ps2Keyboard.h>
#include <Kernel/SpinLock.h>
#include <Kernel/SpinLockIrq.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Task.h>
#include <Kernel/Return.h>
#include <Kernel/UserAccount.h>
#include <Fs/Vfs.h>

// ============================================================================
// Конфигурация
// ============================================================================
#define SCROLLBACK_LINES     2048
#define TAB_WIDTH            8
#define CURSOR_BLINK_MS      500
#define CURSOR_CHAR          '_'
#define CONSOLE_INPUT_RING   2048
#define CONSOLE_HISTORY_SIZE 32
#define CONSOLE_HISTORY_LEN  512
#define CONSOLE_KEY_UP       0x10
#define CONSOLE_KEY_DOWN     0x11
#define CONSOLE_KEY_LEFT     0x12
#define CONSOLE_KEY_RIGHT    0x13
#define OUTPUT_BUFFER_SIZE   4096   

EXTERN(BOOL, TimerInitialized);

struct {
    CHAR Ring[CONSOLE_INPUT_RING];
    volatile UINT32 Head;
    volatile UINT32 Tail;
    BOOL Echo;
    BOOL Attached;
    CHAR History[CONSOLE_HISTORY_SIZE][CONSOLE_HISTORY_LEN];
    UINT32 HistoryCount;
    UINT32 HistoryBrowse;
    volatile KTask *WaitingTask;
    volatile BOOL HasData;
} GInput;

static struct {
    CHAR Buffer[OUTPUT_BUFFER_SIZE];
    UINT32 Length;
    BOOL Pending;
    SpinLockIrq Lock;
} GOutput;

// ANSI цвета (0-15)
static const FBDeviceColor AnsiColors[16] = {
    RGB_BLACK,          // 0
    RGB_RED,            // 1
    RGB_GREEN,          // 2
    RGB_YELLOW,         // 3
    RGB_BLUE,           // 4
    RGB_MAGENTA,        // 5
    RGB_CYAN,           // 6
    RGB_WHITE_SMOKE,    // 7
    RGB_GRAY,           // 8 (bright black)
    RGB_LIGHT_RED,      // 9 (bright red)
    RGB_LIGHT_GREEN,    // 10
    RGB_LIGHT_YELLOW,   // 11
    RGB_LIGHT_BLUE,     // 12
    RGB_LIGHT_MAGENTA,  // 13
    RGB_LIGHT_CYAN,     // 14
    RGB_WHITE           // 15
};

typedef enum {
    ANSI_NORMAL,
    ANSI_ESC,
    ANSI_CSI,
    ANSI_CSI_PARAM
} AnsiState;

typedef struct {
    CHAR* Text;
    UINT8* Colors;
    USIZE Length;
    USIZE Capacity;
} ConsoleLine;

typedef struct {
    ConsoleLine* Lines;
    UINT32 ScrollbackSize;
    UINT32 CurrentLine;   /* слот в кольцевом буфере */
    UINT32 LineSerial;    /* всего строк с начала (монотонно) */
    UINT32 VisibleStart;  /* логический номер верхней видимой строки */
    UINT32 VisibleLines;
    
    INT32 CursorX;
    INT32 CursorY;
    
    FBDeviceColor CurrentFg;
    FBDeviceColor CurrentBg;
    UINT8 CurrentFgAnsi;
    UINT8 CurrentBgAnsi;
    
    // Стили текста
    BOOL Bold;
    BOOL Italic;
    BOOL Underline;
    BOOL Inverse;
    
    // ANSI парсер
    AnsiState AnsiState;
    CHAR AnsiBuffer[32];
    UINT32 AnsiIndex;
    INT32 AnsiParams[16];
    UINT32 AnsiParamCount;
    
    // Курсор
    BOOL CursorVisible;
    UINT64 LastBlinkTick;
    BOOL CursorState;
    INT32 SavedCursorX;
    INT32 SavedCursorY;
    
    BOOL NeedsFullRedraw;
    UINT32 MaxCols;
    UINT32 MaxRows;
    
    BOOL ScrollbackActive;
    UINT32 SavedVisibleStart;
    
    INT32 PromptStartX;     // X позиция начала ввода (после "TOS> ")
    INT32 PromptStartY;     // Y позиция начала ввода
    BOOL PromptActive;      // TRUE когда мы внутри ввода команды

    BOOL SelActive;
    BOOL SelDragging;
    BOOL SelNeedsRedraw;
    UINT32 SelStartLine;
    INT32 SelStartCol;
    UINT32 SelEndLine;
    INT32 SelEndCol;
} ConsoleState;

static ConsoleState* C = NULLPTR;
static BOOL ConsoleInitialized = FALSE;
static CHAR GConsolePrintBuf[4096];

static volatile BOOL ConsoleOn = TRUE;

// ============================================================================
// Внутренние функции
// ============================================================================

NOPTR ConsoleSwitch(BOOL On) {
    if (ConsoleOn == On) return;
    ConsoleOn = On;
}

static ConsoleLine* GetLine(UINT32 Index) {
    if (Index >= C->ScrollbackSize) return NULLPTR;
    return &C->Lines[Index];
}

static UINT32 OldestLogicalLine(NOPTR) {
    if (C->LineSerial <= C->ScrollbackSize) {
        return 0;
    }
    return C->LineSerial - C->ScrollbackSize;
}

static UINT32 LogicalLineToBuffer(UINT32 LogicalLine) {
    if (C->LineSerial <= C->ScrollbackSize) {
        return LogicalLine;
    }
    UINT32 Oldest = OldestLogicalLine();
    UINT32 OldestBuf = (C->CurrentLine + 1) % C->ScrollbackSize;
    return (OldestBuf + (LogicalLine - Oldest)) % C->ScrollbackSize;
}

static NOPTR FullRedraw(NOPTR);

static NOPTR UpdateFollowModeVisibleStart(NOPTR) {
    UINT32 Cur = C->LineSerial - 1;
    UINT32 Oldest = OldestLogicalLine();

    if (C->LineSerial > C->VisibleLines) {
        C->VisibleStart = Cur - C->VisibleLines + 1;
        if (C->VisibleStart < Oldest) {
            C->VisibleStart = Oldest;
        }
    } else {
        C->VisibleStart = 0;
    }
    C->CursorY = (INT32)(Cur - C->VisibleStart);
}

static NOPTR FreeLine(ConsoleLine* Line) {
    if (Line->Text) {
        MemoryFree(Line->Text);
        Line->Text = NULLPTR;
    }
    if (Line->Colors) {
        MemoryFree(Line->Colors);
        Line->Colors = NULLPTR;
    }
    Line->Length = 0;
    Line->Capacity = 0;
}

static NOPTR EnsureLineCapacity(ConsoleLine* Line, USIZE MinCapacity) {
    if (MinCapacity <= Line->Capacity) return;
    
    USIZE NewCapacity = Line->Capacity * 2;
    if (NewCapacity < MinCapacity) NewCapacity = MinCapacity;
    if (NewCapacity < 64) NewCapacity = 64;
    
    CHAR* NewText = (CHAR*)MemoryAllocate(NewCapacity);
    UINT8* NewColors = (UINT8*)MemoryAllocate(NewCapacity);
    
    if (Line->Text) {
        MemCpy(NewText, Line->Text, Line->Length);
        MemCpy(NewColors, Line->Colors, Line->Length);
        MemoryFree(Line->Text);
        MemoryFree(Line->Colors);
    }
    
    Line->Text = NewText;
    Line->Colors = NewColors;
    Line->Capacity = NewCapacity;
}

static UINT8 PackColor(FBDeviceColor Fg, FBDeviceColor Bg) {
    UINT8 FgIdx = 7, BgIdx = 0;
    INT I;
    for (I = 0; I < 16; I++) {
        if (MemCmp(&AnsiColors[I], &Fg, sizeof(FBDeviceColor)) == 0) FgIdx = (UINT8)I;
        if (MemCmp(&AnsiColors[I], &Bg, sizeof(FBDeviceColor)) == 0) BgIdx = (UINT8)I;
    }
    return (BgIdx << 4) | (FgIdx & 0x0F);
}

static FBDeviceColor ApplyCurrentFg(NOPTR) {
    FBDeviceColor Fg = C->CurrentFg;
    
    if (C->Inverse) {
        return C->CurrentBg;
    }
    
    if (C->Bold) {
        Fg.R = (Fg.R * 12 / 10 > 255) ? 255 : (UINT8)(Fg.R * 12 / 10);
        Fg.G = (Fg.G * 12 / 10 > 255) ? 255 : (UINT8)(Fg.G * 12 / 10);
        Fg.B = (Fg.B * 12 / 10 > 255) ? 255 : (UINT8)(Fg.B * 12 / 10);
    }
    
    return Fg;
}

static FBDeviceColor ApplyCurrentBg(NOPTR) {
    if (C->Inverse) {
        return C->CurrentFg;
    }
    return C->CurrentBg;
}

static NOPTR NewLine(NOPTR) {
    UINT32 NextLine = (C->CurrentLine + 1) % C->ScrollbackSize;
    UINT32 PrevVisibleStart = C->VisibleStart;
    BOOL Scrolled = FALSE;

    if (C->LineSerial >= C->ScrollbackSize && C->Lines[NextLine].Text) {
        FreeLine(&C->Lines[NextLine]);
    }

    ConsoleLine* New = &C->Lines[NextLine];
    New->Length = 0;
    New->Text = NULLPTR;
    New->Colors = NULLPTR;
    New->Capacity = 0;
    EnsureLineCapacity(New, C->MaxCols);

    C->CurrentLine = NextLine;
    C->LineSerial++;
    C->CursorX = 0;

    if (!C->ScrollbackActive) {
        UpdateFollowModeVisibleStart();
        Scrolled = (C->VisibleStart != PrevVisibleStart);
    } else {
        C->CursorY++;
        if (C->CursorY >= (INT32)C->VisibleLines) {
            C->CursorY = (INT32)C->VisibleLines - 1;
        }
    }

    if (Scrolled) {
        FullRedraw();
    }
}

static BOOL IsCellInSelection(UINT32 LogicalLine, UINT32 Col) {
    UINT32 L0;
    UINT32 L1;
    INT32 C0;
    INT32 C1;
    UINT32 TmpL;
    INT32 TmpC;

    if (!C->SelActive) {
        return FALSE;
    }

    L0 = C->SelStartLine;
    L1 = C->SelEndLine;
    C0 = C->SelStartCol;
    C1 = C->SelEndCol;

    if (L0 > L1 || (L0 == L1 && C0 > C1)) {
        TmpL = L0;
        L0 = L1;
        L1 = TmpL;
        TmpC = C0;
        C0 = C1;
        C1 = TmpC;
    }

    if (LogicalLine < L0 || LogicalLine > L1) {
        return FALSE;
    }
    if (LogicalLine == L0 && (INT32)Col < C0) {
        return FALSE;
    }
    if (LogicalLine == L1 && (INT32)Col > C1) {
        return FALSE;
    }
    return TRUE;
}

static NOPTR PixelToCell(INT32 Px, INT32 Py, INT32 *Col, UINT32 *LogicalLine) {
    UINT32 Row;

    *Col = Px / (INT32)FONT_WIDTH;
    if (*Col < 0) {
        *Col = 0;
    }
    if ((UINT32)*Col >= C->MaxCols) {
        *Col = (INT32)C->MaxCols - 1;
    }

    Row = (UINT32)Py / FONT_LINE_HEIGHT;
    if (Row >= C->VisibleLines) {
        Row = C->VisibleLines - 1;
    }
    *LogicalLine = C->VisibleStart + Row;
}

NOPTR ConsoleService(NOPTR) {
    if (!ConsoleInitialized) {
        return;
    }
    if (C->SelNeedsRedraw) {
        C->SelNeedsRedraw = FALSE;
        FullRedraw();
    }
}

static NOPTR FullRedraw(NOPTR) {
    if (!ConsoleOn) return;

    UINT32 LineIdx, X, Y;
    UINT32 StartLine, EndLine;
    ConsoleLine* Line;
    UINT8 FgIdx, BgIdx;
    FBDeviceColor Fg;
    FBDeviceColor Bg;

    FBDeviceClear(RGB_BLACK);
    
    StartLine = C->VisibleStart;
    EndLine = StartLine + C->VisibleLines;
    if (EndLine > C->LineSerial) {
        EndLine = C->LineSerial;
    }

    for (LineIdx = StartLine; LineIdx < EndLine; LineIdx++) {
        Line = GetLine(LogicalLineToBuffer(LineIdx));
        if (!Line || !Line->Text) continue;
        
        Y = LineIdx - StartLine;
        for (X = 0; X < Line->Length && X < C->MaxCols; X++) {
            FgIdx = Line->Colors[X] & 0x0F;
            BgIdx = (Line->Colors[X] >> 4) & 0x0F;
            Fg = AnsiColors[FgIdx];
            Bg = AnsiColors[BgIdx];

            if (IsCellInSelection(LineIdx, X)) {
                Bg = AnsiColors[12];
            }

            FBDeviceDrawChar(Line->Text[X], X * 8, Y * 16, Fg, Bg);
        }
    }
    
    C->NeedsFullRedraw = FALSE;
    FBDeviceSwapBuffers();
}

static NOPTR ProcessAnsi(NOPTR) {
    UINT32 I;
    
    if (C->AnsiParamCount == 0) return;
    
    INT32 Command = C->AnsiParams[C->AnsiParamCount - 1];
    
    switch (Command) {
        case 'A': // Cursor Up
            if (C->CursorY > 0) {
                C->CursorY--;
            } else {
                UINT32 Oldest = OldestLogicalLine();
                if (C->VisibleStart > Oldest) {
                    C->VisibleStart--;
                    C->NeedsFullRedraw = TRUE;
                }
            }
            break;
            
        case 'B': // Cursor Down
            if (C->CursorY < (INT32)(C->VisibleLines - 1)) {
                C->CursorY++;
            } else {
                UINT32 MaxStart = (C->LineSerial > C->VisibleLines)
                    ? C->LineSerial - C->VisibleLines : 0;
                if (C->VisibleStart < MaxStart) {
                    C->VisibleStart++;
                    C->NeedsFullRedraw = TRUE;
                }
            }
            break;
            
        case 'C': // Cursor Forward
            if (C->CursorX < (INT32)(C->MaxCols - 1)) C->CursorX++;
            break;
            
        case 'D': // Cursor Back
            if (C->CursorX > 0) C->CursorX--;
            break;
            
        case 'H': // Cursor Position
        case 'f':
            if (C->AnsiParamCount >= 3) {
                C->CursorY = C->AnsiParams[0] - 1;
                C->CursorX = C->AnsiParams[1] - 1;
                if (C->CursorY < 0) C->CursorY = 0;
                if (C->CursorX < 0) C->CursorX = 0;
                if (C->CursorY >= (INT32)C->VisibleLines) {
                    UINT32 Oldest = OldestLogicalLine();
                    C->VisibleStart = (UINT32)C->CursorY - C->VisibleLines + 1;
                    if (C->VisibleStart < Oldest) {
                        C->VisibleStart = Oldest;
                    }
                    C->CursorY = (INT32)((C->LineSerial - 1) - C->VisibleStart);
                    C->NeedsFullRedraw = TRUE;
                }
            }
            break;
            
        case 'J': // Erase Display
            if (C->AnsiParamCount >= 2 && C->AnsiParams[0] == 2) {
                ConsoleClear();
            }
            break;
            
        case 'm': // Set Graphics Mode
            for (I = 0; I < C->AnsiParamCount - 1; I++) {
                switch (C->AnsiParams[I]) {
                    case 0:
                        C->CurrentFg = AnsiColors[7];
                        C->CurrentBg = AnsiColors[0];
                        C->CurrentFgAnsi = 7;
                        C->CurrentBgAnsi = 0;
                        C->Bold = FALSE;
                        C->Italic = FALSE;
                        C->Underline = FALSE;
                        C->Inverse = FALSE;
                        break;
                    case 1: C->Bold = TRUE; break;
                    case 3: C->Italic = TRUE; break;
                    case 4: C->Underline = TRUE; break;
                    case 7: C->Inverse = TRUE; break;
                    case 30 ... 37:
                        C->CurrentFgAnsi = (UINT8)(C->AnsiParams[I] - 30);
                        C->CurrentFg = AnsiColors[C->CurrentFgAnsi];
                        break;
                    case 40 ... 47:
                        C->CurrentBgAnsi = (UINT8)(C->AnsiParams[I] - 40);
                        C->CurrentBg = AnsiColors[C->CurrentBgAnsi];
                        break;
                    case 90 ... 97:
                        C->CurrentFgAnsi = (UINT8)(C->AnsiParams[I] - 90 + 8);
                        C->CurrentFg = AnsiColors[C->CurrentFgAnsi];
                        break;
                    case 100 ... 107:
                        C->CurrentBgAnsi = (UINT8)(C->AnsiParams[I] - 100 + 8);
                        C->CurrentBg = AnsiColors[C->CurrentBgAnsi];
                        break;
                }
            }
            break;
            
        case 's': // Save cursor
            C->SavedCursorX = C->CursorX;
            C->SavedCursorY = C->CursorY;
            break;
            
        case 'u': // Restore cursor
            C->CursorX = C->SavedCursorX;
            C->CursorY = C->SavedCursorY;
            break;
    }
}

static NOPTR CarriageReturn(NOPTR) {
    C->CursorX = 0;
}

static BOOL ConsoleCanEraseBackspace(NOPTR) {
    if (!C->PromptActive) {
        return TRUE;
    }
    if (C->CursorY < C->PromptStartY) {
        return FALSE;
    }
    if (C->CursorY == C->PromptStartY && C->CursorX <= C->PromptStartX) {
        return FALSE;
    }
    return TRUE;
}

static NOPTR ConsoleEraseBackspace(NOPTR) {
    if (!ConsoleOn) return;

    UINT32 BufferIdx;
    ConsoleLine *Line;
    USIZE Pos;
    USIZE I;

    if (!ConsoleCanEraseBackspace()) {
        return;
    }

    if (C->CursorX > 0) {
        C->CursorX--;
        BufferIdx = LogicalLineToBuffer((UINT32)C->VisibleStart + (UINT32)C->CursorY);
        Line = GetLine(BufferIdx);
        if (!Line) {
            return;
        }
        Pos = (USIZE)C->CursorX;
        if (Pos < Line->Length) {
            for (I = Pos; I + 1 < Line->Length; I++) {
                Line->Text[I] = Line->Text[I + 1];
                Line->Colors[I] = Line->Colors[I + 1];
            }
            Line->Length--;
            FBDeviceDrawChar(' ', Pos * 8, C->CursorY * 16,
                             ApplyCurrentFg(), ApplyCurrentBg());
            for (I = Pos; I < Line->Length; I++) {
                FBDeviceDrawChar(Line->Text[I], (INT32)I * 8, C->CursorY * 16,
                                 ApplyCurrentFg(), ApplyCurrentBg());
            }
        }
        return;
    }
}

static NOPTR InputPush(CHAR Ch) {
    UINT32 Tail = GInput.Tail;
    UINT32 Next = (Tail + 1) % CONSOLE_INPUT_RING;
    
    // Если буфер полон — просто игнорируем символ
    if (Next == GInput.Head) {
        return;
    }
    
    GInput.Ring[Tail] = Ch;
    __sync_synchronize();  // Барьер памяти — гарантируем видимость для других ядер
    GInput.Tail = Next;
    
    GInput.HasData = TRUE;
}

static BOOL InputPop(CHAR *Out) {
    UINT32 Head = GInput.Head;
    
    if (Head == GInput.Tail) {
        return FALSE;
    }
    
    *Out = GInput.Ring[Head];
    __sync_synchronize();  // Барьер памяти
    GInput.Head = (Head + 1) % CONSOLE_INPUT_RING;
    return TRUE;
}

static NOPTR ConsoleEchoInputChar(CHAR Ch) {
    if (!ConsoleOn) return;
    if (!GInput.Echo) return;

    if (Ch >= 0x20 && Ch <= 0x7E) {
        ConsolePrint("%c", Ch);
        return;
    }

    switch (Ch) {
        case '\n':
        case '\r':
            ConsolePrint("\n");
            break;
        case '\b':
            ConsoleEraseBackspace();
            FBDeviceSwapBuffers();
            break;
        case '\t':
            ConsolePrint("\t");
            break;
        case 3:
            ConsolePrint("^C");
            break;
        default:
            break;
    }
}

static NOPTR ConsoleInputKeyHandler(KeyEvent *Event, NOPTR *UserData) {
    if (!ConsoleOn) return;

    (NOPTR)UserData;
    if (!ConsoleInitialized || Event->State != KEY_STATE_PRESSED) {
        return;
    }

    if (Event->Ascii >= 0x20 && Event->Ascii <= 0x7E) {
        InputPush(Event->Ascii);
        return;
    }

    BOOL CtrlPressed = (Event->Modifiers & MOD_CTRL) != 0;

    if (CtrlPressed) {
        switch (Event->ScanCode) {
            case PS2_KEY_UP:
                ConsoleScrollbackUp(1);  // На 1 строку вверх
                return;
            case PS2_KEY_DOWN:
                ConsoleScrollbackDown(1);  // На 1 строку вниз
                return;
            case PS2_KEY_HOME:
                ConsoleResetScrollback();  // Вернуться в конец
                return;
            default:
                break;
        }
    }

    switch (Event->Ascii) {
        case '\n':
        case '\r':
            InputPush('\n');
            break;
        case '\b':
            InputPush('\b');
            break;
        case '\t':
            InputPush('\t');
            break;
        case 3:
            InputPush(3);
            break;
        default:
            switch (Event->ScanCode) {
                case PS2_KEY_PGUP:
                    ConsoleScrollbackUp(3);
                    break;
                case PS2_KEY_PGDN:
                    ConsoleScrollbackDown(3);
                    break;
                case PS2_KEY_HOME:
                    ConsoleResetScrollback();
                    break;
                case PS2_KEY_UP:
                    if (C && C->PromptActive) {
                        InputPush(CONSOLE_KEY_UP);
                    }
                    break;
                case PS2_KEY_DOWN:
                    if (C && C->PromptActive) {
                        InputPush(CONSOLE_KEY_DOWN);
                    }
                    break;
                case PS2_KEY_LEFT:
                    if (C && C->PromptActive) {
                        InputPush(CONSOLE_KEY_LEFT);
                    }
                    break;
                case PS2_KEY_RIGHT:
                    if (C && C->PromptActive) {
                        InputPush(CONSOLE_KEY_RIGHT);
                    }
                    break;
                default:
                    break;
            }
            break;
    }
}

static NOPTR EraseCursor(NOPTR) {
    if (!ConsoleOn) return;

    if (!C->CursorVisible) return;
    
    // Рисуем пробел с цветом фона на месте курсора
    FBDeviceDrawChar(' ', 
                     C->CursorX * FONT_WIDTH, 
                     C->CursorY * FONT_LINE_HEIGHT,
                     ApplyCurrentBg(),  // фон
                     ApplyCurrentBg()); // и фон же (чтобы стереть)
}

static NOPTR ParseChar(CHAR Ch) {
    UINT32 BufferIdx;
    ConsoleLine* Line;
    UINT8 Color;
    
    EraseCursor();
    
    if (C->AnsiState != ANSI_NORMAL) {
        if (C->AnsiState == ANSI_ESC) {
            if (Ch == '[') {
                C->AnsiState = ANSI_CSI;
                C->AnsiIndex = 0;
                C->AnsiParamCount = 1;
                MemSet(C->AnsiParams, 0, sizeof(C->AnsiParams));
            } else {
                C->AnsiState = ANSI_NORMAL;
            }
            return;
        }
        
        if (C->AnsiState == ANSI_CSI || C->AnsiState == ANSI_CSI_PARAM) {
            if (Ch >= '0' && Ch <= '9') {
                if (C->AnsiIndex < sizeof(C->AnsiBuffer) - 1) {
                    C->AnsiBuffer[C->AnsiIndex++] = Ch;
                }
                if (C->AnsiParamCount <= 16) {
                    C->AnsiParams[C->AnsiParamCount - 1] = 
                        C->AnsiParams[C->AnsiParamCount - 1] * 10 + (Ch - '0');
                }
                C->AnsiState = ANSI_CSI_PARAM;
                return;
            }
            
            if (Ch == ';') {
                C->AnsiParamCount++;
                if (C->AnsiParamCount > 16) C->AnsiParamCount = 16;
                C->AnsiState = ANSI_CSI;
                return;
            }
            
            if (Ch >= '@' && Ch <= '~') {
                // Добавляем команду отдельным "параметром" в конец,
                // не затирая последний числовой параметр.
                if (C->AnsiParamCount < 16) {
                    C->AnsiParams[C->AnsiParamCount] = Ch;
                    C->AnsiParamCount++;
                } else {
                    C->AnsiParams[15] = Ch;
                }
                ProcessAnsi();
                C->AnsiState = ANSI_NORMAL;
                if (C->NeedsFullRedraw) {
                    FullRedraw();
                }
                return;
            }
            
            C->AnsiState = ANSI_NORMAL;
            return;
        }
        
        C->AnsiState = ANSI_NORMAL;
        return;
    }
    
    if (Ch == '\033') {
        C->AnsiState = ANSI_ESC;
        return;
    }
    
    Color = PackColor(ApplyCurrentFg(), ApplyCurrentBg());
    
    switch (Ch) {
        case '\n':
            C->PromptActive = FALSE;
            NewLine();
	    CarriageReturn();
            break;
            
        case '\r':
            C->CursorX = 0;
            break;
            
        case '\b':
            ConsoleEraseBackspace();
            break;
            
        case '\t':
            C->CursorX = ((C->CursorX / TAB_WIDTH) + 1) * TAB_WIDTH;
            if (C->CursorX >= (INT32)C->MaxCols) C->CursorX = (INT32)C->MaxCols - 1;
            break;
            
        default:
            if (Ch >= 0x20 && Ch <= 0x7E) {
                BufferIdx = LogicalLineToBuffer((UINT32)C->VisibleStart + (UINT32)C->CursorY);
                Line = GetLine(BufferIdx);
                if (!Line) {
                    break;
                }
                
                EnsureLineCapacity(Line, (USIZE)C->CursorX + 1);
                while (Line->Length <= (USIZE)C->CursorX) {
                    Line->Text[Line->Length] = ' ';
                    Line->Colors[Line->Length] = PackColor(AnsiColors[7], AnsiColors[0]);
                    Line->Length++;
                }
                
                Line->Text[C->CursorX] = Ch;
                Line->Colors[C->CursorX] = Color;
                
                FBDeviceDrawChar(Ch, C->CursorX * 8, C->CursorY * 16,
                               ApplyCurrentFg(), ApplyCurrentBg());
                
                C->CursorX++;
                if (C->CursorX >= (INT32)C->MaxCols) {
                    C->CursorX = 0;
                    NewLine();
                }
            }
            break;
    }

    if (C->NeedsFullRedraw) {
        FullRedraw();
    }
}

NOPTR RenderCursor(NOPTR) {
    if (!ConsoleOn) return;

    if (TimerInitialized == FALSE) return;
    if (!C->CursorVisible) return;
    
    UINT64 Now = TimerTicks();
    UINT64 Elapsed = (Now - C->LastBlinkTick) * 1000 / TimerFreq();
    
    if (Elapsed >= CURSOR_BLINK_MS) {
        // СТИРАЕМ старый курсор перед сменой состояния
        EraseCursor();
        
        C->CursorState = !C->CursorState;
        C->LastBlinkTick = Now;
    }
    
    if (!C->CursorState) return;
    
    // Рисуем новый курсор
    FBDeviceDrawChar(CURSOR_CHAR, 
                     C->CursorX * FONT_WIDTH, 
                     C->CursorY * FONT_LINE_HEIGHT,
                     ApplyCurrentFg(), 
                     ApplyCurrentBg());
    FBDeviceSwapBuffers();
}

// ============================================================================
// Публичные API
// ============================================================================

NOPTR ConsoleInit(NOPTR) {
    UINT32 Width, Height;
    
    if (ConsoleInitialized) return;
    
    Width = FBDeviceGetWidth();
    Height = FBDeviceGetHeight();
    
    C = (ConsoleState*)MemoryAllocate(sizeof(ConsoleState));
    MemSet(C, 0, sizeof(ConsoleState));
    
    C->MaxCols = Width / 8;
    C->MaxRows = Height / 16;
    C->VisibleLines = C->MaxRows;
    C->ScrollbackSize = SCROLLBACK_LINES;
    
    C->Lines = (ConsoleLine*)MemoryCallocate(C->ScrollbackSize, sizeof(ConsoleLine));
    
    C->CurrentFg = AnsiColors[7];
    C->CurrentBg = AnsiColors[0];
    C->CurrentFgAnsi = 7;
    C->CurrentBgAnsi = 0;
    
    C->CursorVisible = TRUE;
    C->CursorState = TRUE;
    C->CursorX = 0;
    C->CursorY = 0;
    C->VisibleStart = 0;
    C->CurrentLine = 0;
    C->LineSerial = 1;
    C->NeedsFullRedraw = TRUE;
    C->LastBlinkTick = TimerTicks();
    
    EnsureLineCapacity(&C->Lines[0], C->MaxCols);

    MemSet(&GInput, 0, sizeof(GInput));
    GInput.Echo = TRUE;

    MemSet(&GOutput, 0, sizeof(GOutput));
    SpinLockIrqInit(&GOutput.Lock);

    ConsoleClear();
    ConsoleInitialized = TRUE;
}

NOPTR ConsoleClear(NOPTR) {
    if (!ConsoleOn) return;

    UINT32 I;
    
    FBDeviceClear(RGB_BLACK);
    FBDeviceSwapBuffers();
    
    for (I = 0; I < C->ScrollbackSize; I++) {
        if (C->Lines[I].Text) {
            MemSet(C->Lines[I].Text, 0, C->Lines[I].Capacity);
            MemSet(C->Lines[I].Colors, 0, C->Lines[I].Capacity);
        }
        C->Lines[I].Length = 0;
    }
    
    C->CurrentLine = 0;
    C->VisibleStart = 0;
    C->CursorX = 0;
    C->CursorY = 0;
    C->LineSerial = 1;
    C->NeedsFullRedraw = TRUE;

    FullRedraw();
}

// ============================================================================
// Быстрый вывод строки целиком
// ============================================================================
static NOPTR FlushStringFast(const CHAR *Str, UINT32 Len) {
    if (!ConsoleOn || !Str || Len == 0) return;
    
    BOOL WasVisible = C->CursorVisible;
    C->CursorVisible = FALSE;
    
    for (UINT32 I = 0; I < Len; I++) {
        ParseChar(Str[I]);
    }
    
    FBDeviceSwapBuffers();
    C->CursorVisible = WasVisible;
    if (WasVisible) RenderCursor();
}

NOPTR FlushOutputBuffer(NOPTR) {
    if (!GOutput.Pending || GOutput.Length == 0) return;
    
    SpinLockIrqAcquire(&GOutput.Lock);
    if (GOutput.Length > 0) {
        FlushStringFast(GOutput.Buffer, GOutput.Length);
        GOutput.Length = 0;
        GOutput.Pending = FALSE;
    }
    SpinLockIrqRelease(&GOutput.Lock);
}

static NOPTR AppendToBuffer(const CHAR *Str, UINT32 Len) {
    if (!ConsoleOn || !Str || Len == 0) return;
    
    SpinLockIrqAcquire(&GOutput.Lock);
    
    if (GOutput.Length + Len <= OUTPUT_BUFFER_SIZE) {
        MemCpy(GOutput.Buffer + GOutput.Length, Str, Len);
        GOutput.Length += Len;
        GOutput.Pending = TRUE;
    } else {
        SpinLockIrqRelease(&GOutput.Lock);
        FlushOutputBuffer();
        SpinLockIrqAcquire(&GOutput.Lock);
        
        if (Len <= OUTPUT_BUFFER_SIZE) {
            MemCpy(GOutput.Buffer, Str, Len);
            GOutput.Length = Len;
            GOutput.Pending = TRUE;
        } else {
            SpinLockIrqRelease(&GOutput.Lock);
            FlushStringFast(Str, Len);
            return;
        }
    }
    
    SpinLockIrqRelease(&GOutput.Lock);
}

NOPTR ConsolePrint(const CHAR *Fmt, ...) {
    if (!ConsoleOn || !ConsoleInitialized) return;
    
    VA_LIST Args;
    CHAR TempBuf[1024];
    INT Len;
    
    VaStart(Args, Fmt);
    Len = VsnPrintf(TempBuf, sizeof(TempBuf), Fmt, Args);
    VaEnd(Args);
    
    if (Len <= 0) return;
    
    // Проверяем, есть ли \n
    BOOL HasNewLine = FALSE;
    for (INT I = 0; I < Len; I++) {
        if (TempBuf[I] == '\n') {
            HasNewLine = TRUE;
            break;
        }
    }
    
    if (HasNewLine) {
        FlushOutputBuffer();
        FlushStringFast(TempBuf, (UINT32)Len);
    } else {
        AppendToBuffer(TempBuf, (UINT32)Len);
    }
}

NOPTR ConsoleSetColor(UINT8 Fg, UINT8 Bg) {
    if (!ConsoleInitialized) return;
    if (Fg < 16) C->CurrentFg = AnsiColors[Fg];
    if (Bg < 16) C->CurrentBg = AnsiColors[Bg];
}

NOPTR ConsoleSetCursorPos(UINT32 X, UINT32 Y) {
    if (!ConsoleInitialized) return;
    if (X < C->MaxCols) C->CursorX = (INT32)X;
    if (Y < C->MaxRows) C->CursorY = (INT32)Y;
}

NOPTR ConsoleGetCursorPos(UINT32 *X, UINT32 *Y) {
    if (!ConsoleInitialized || !X || !Y) return;
    *X = (UINT32)C->CursorX;
    *Y = (UINT32)C->CursorY;
}

NOPTR ConsoleShowCursor(BOOL Show) {
    if (!ConsoleInitialized) return;
    C->CursorVisible = Show;
}

NOPTR ConsoleScrollbackUp(UINT32 Lines) {
    if (!ConsoleOn) return;

    UINT32 Oldest;

    if (!ConsoleInitialized) return;
    if (!C->ScrollbackActive) {
        C->ScrollbackActive = TRUE;
        C->SavedVisibleStart = C->VisibleStart;
    }
    Oldest = OldestLogicalLine();
    if (C->VisibleStart > Lines) {
        if (C->VisibleStart - Lines >= Oldest) {
            C->VisibleStart -= Lines;
        } else {
            C->VisibleStart = Oldest;
        }
    } else {
        C->VisibleStart = Oldest;
    }
    C->NeedsFullRedraw = TRUE;
    FullRedraw();
}

NOPTR ConsoleScrollbackDown(UINT32 Lines) {
    if (!ConsoleOn) return;

    if (!ConsoleInitialized) return;
    if (!C->ScrollbackActive) return;
    
    if (C->VisibleStart + Lines <= C->SavedVisibleStart) {
        C->VisibleStart += Lines;
    } else {
        C->VisibleStart = C->SavedVisibleStart;
        C->ScrollbackActive = FALSE;
    }
    C->NeedsFullRedraw = TRUE;
    FullRedraw();
}

NOPTR ConsoleResetScrollback(NOPTR) {
    if (!ConsoleOn) return;
    if (!ConsoleInitialized) return;
    C->ScrollbackActive = FALSE;
    UpdateFollowModeVisibleStart();
    C->NeedsFullRedraw = TRUE;
    FullRedraw();
}

NOPTR ConsoleShowPrompt(NOPTR) {
    if (!ConsoleOn) return;
    if (!ConsoleInitialized) return;
    
    if (C->CursorX > 0) ConsolePrint("\n");

    GInput.Echo = TRUE;
    
    if (UserManagerIsLoggedIn()) {
        const TosSession *Session = UserManagerGetSession();
        ConsolePrint("\033[34mTOS\033[0m [\033[32m%s\033[0m@\033[33m%s\033[0m]> ",
                     Session->Username, CurrentPath);
    } else {
        ConsolePrint("\033[34mTOS\033[0m [\033[31mlogin\033[0m]> ");
    }

    C->PromptStartX = C->CursorX;
    C->PromptStartY = C->CursorY;
    C->PromptActive = TRUE;
}

NOPTR ConsoleBeginCommandOutput(NOPTR) {
    if (!ConsoleInitialized) {
        return;
    }
    C->PromptActive = FALSE;
}

NOPTR ConsoleEndCommandOutput(NOPTR) {
    if (!ConsoleOn) return;
    if (!ConsoleInitialized) {
        return;
    }
    C->PromptActive = FALSE;
    if (C->CursorX > 0) {
        ConsolePrint("\n");
    }
}

static NOPTR ConsoleReplaceInputText(const CHAR *Input, USIZE *Pos) {
    USIZE OldPos = *Pos;
    USIZE NewPos;
    USIZE I;

    NewPos = Input ? StrLen(Input) : 0;
    if (NewPos >= CONSOLE_HISTORY_LEN) {
        NewPos = CONSOLE_HISTORY_LEN - 1;
    }

    while (OldPos > 0) {
        ConsoleEchoInputChar('\b');
        OldPos--;
    }

    for (I = 0; I < NewPos; I++) {
        ConsoleEchoInputChar(Input[I]);
    }

    *Pos = NewPos;
}

static NOPTR ConsoleInputWait(NOPTR) {
    if (!ConsoleOn) return;

    for (;;) {
        Ps2PortService();
        ConsoleService();

        // Если есть данные — выходим
        if (GInput.Head != GInput.Tail) {
            GInput.HasData = FALSE;
            return;
        }

        // Если флаг установлен — значит данные появились, но мы не успели прочитать
        if (GInput.HasData) {
            GInput.HasData = FALSE;
            continue;
        }

        // Ждём 1 мс
        LocalInterruptsEnable();
        TimerSleep(1);
    }
}

INT ConsoleInputAttach(NOPTR) {
    if (!ConsoleOn) return NOT_SUPPORTED;
    INT Ret;

    if (!ConsoleInitialized) {
        return DEVICE_ERROR;
    }
    Ret = Ps2KeyboardSubscribe(ConsoleInputKeyHandler, NULLPTR);
    if (Ret == SUCCESS) {
        GInput.Attached = TRUE;
    }
    return Ret;
}

NOPTR ConsoleInputDetach(NOPTR) {
    Ps2KeyboardUnsubscribe(ConsoleInputKeyHandler);
    GInput.Attached = FALSE;
}

BOOL ConsoleInputAvailable(NOPTR) {
    return (GInput.Head != GInput.Tail);
}

INT ConsoleTryReadChar(NOPTR) {
    CHAR Ch;

    if (!InputPop(&Ch)) {
        return -1;
    }
    return (INT)(UINT8)Ch;
}

INT ConsoleReadChar(NOPTR) {
    if (!ConsoleOn) return NOT_SUPPORTED;

    CHAR Ch;

    for (;;) {
        if (InputPop(&Ch)) {
            ConsoleEchoInputChar(Ch);
            return (INT)(UINT8)Ch;
        }
        ConsoleInputWait();
    }
}

INT ConsoleReadLine(CHAR *Buf, USIZE Size) {
    INT Ch;
    USIZE Pos = 0;
    INT HistoryIdx = -1;
    CHAR Saved[CONSOLE_HISTORY_LEN];

    if (!Buf || Size == 0) {
        return INCORRECT_VALUE;
    }

    Buf[0] = '\0';
    GInput.HistoryBrowse = 0;
    GInput.Echo = TRUE;

    for (;;) {
        ConsoleInputWait();

        Ch = ConsoleTryReadChar();
        if (Ch < 0) {
            continue;
        }

        // --- СТРЕЛКИ ВВЕРХ/ВНИЗ (ИСТОРИЯ) ---
        if (Ch == CONSOLE_KEY_UP) {
            if (GInput.HistoryCount == 0) continue;
            
            if (HistoryIdx < 0) {
                StrnCpy(Saved, Buf, sizeof(Saved) - 1);
                Saved[sizeof(Saved) - 1] = '\0';
                HistoryIdx = (INT)GInput.HistoryCount - 1;
            } else if (HistoryIdx > 0) {
                HistoryIdx--;
            } else {
                continue;
            }
            
            // Стираем текущую строку
            while (Pos > 0) {
                ConsoleEchoInputChar('\b');
                Pos--;
            }
            
            // Вставляем историю
            StrnCpy(Buf, GInput.History[HistoryIdx], Size - 1);
            Buf[Size - 1] = '\0';
            Pos = StrLen(Buf);
            ConsolePrint("%s", Buf);
            continue;
        }

        if (Ch == CONSOLE_KEY_DOWN) {
            if (HistoryIdx < 0) continue;
            
            if ((UINT32)HistoryIdx + 1 < GInput.HistoryCount) {
                HistoryIdx++;
                StrnCpy(Buf, GInput.History[HistoryIdx], Size - 1);
            } else {
                HistoryIdx = -1;
                StrnCpy(Buf, Saved, Size - 1);
            }
            
            // Стираем текущую строку
            while (Pos > 0) {
                ConsoleEchoInputChar('\b');
                Pos--;
            }
            
            Buf[Size - 1] = '\0';
            Pos = StrLen(Buf);
            ConsolePrint("%s", Buf);
            continue;
        }

        // --- СТРЕЛКИ ВЛЕВО/ВПРАВО (ПОКА ПРОСТО ИГНОРИРУЕМ) ---
        if (Ch == CONSOLE_KEY_LEFT || Ch == CONSOLE_KEY_RIGHT) {
            continue;
        }

        // --- ENTER ---
        if (Ch == '\n' || Ch == '\r') {
            Buf[Pos] = '\0';
            
            // Сохраняем в историю
            if (Pos > 0 && GInput.HistoryCount < CONSOLE_HISTORY_SIZE) {
                if (GInput.HistoryCount == 0 ||
                    StrCmp(GInput.History[GInput.HistoryCount - 1], Buf) != 0) {
                    StrnCpy(GInput.History[GInput.HistoryCount], Buf, CONSOLE_HISTORY_LEN - 1);
                    GInput.History[GInput.HistoryCount][CONSOLE_HISTORY_LEN - 1] = '\0';
                    GInput.HistoryCount++;
                }
            }
            
            ConsolePrint("\n");
            return (INT)Pos;
        }

        // --- CTRL+C ---
        if (Ch == 3) {
            Buf[0] = '\0';
            ConsolePrint("^C\n");
            return -2;
        }

        // --- BACKSPACE ---
        if (Ch == '\b') {
            if (Pos > 0) {
                Pos--;
                Buf[Pos] = '\0';
                ConsoleEchoInputChar('\b');
            }
            continue;
        }

        // --- ОБЫЧНЫЕ СИМВОЛЫ ---
        if (Ch == '\t' || (Ch >= 0x20 && Ch <= 0x7E)) {
            if (Pos + 1 < Size) {
                Buf[Pos++] = (CHAR)Ch;
                Buf[Pos] = '\0';
                ConsoleEchoInputChar((CHAR)Ch);
            }
        }
    }
}

NOPTR ConsoleSetInputEcho(BOOL Echo) {
    GInput.Echo = Echo;
}

BOOL ConsoleIsPromptActive(NOPTR) {
    if (!ConsoleInitialized) {
        return FALSE;
    }
    return C->PromptActive;
}

UINT32 ConsoleGetCurrentLineNum(NOPTR) {
    if (!ConsoleInitialized) {
        return 0;
    }
    return C->LineSerial;
}

NOPTR ConsoleSetEcho(BOOL State) {
    if (GInput.Echo == State) return;
    GInput.Echo = State;
}
