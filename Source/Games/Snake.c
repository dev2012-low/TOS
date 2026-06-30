#include <Games/Snake.h>
#include <FBDevice.h>
#include <Rgb.h>
#include <Console.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Kernel/Return.h>
#include <Kernel/Scheduler.h>
#include <Time/Timer.h>
#include <Ps2Keyboard.h>
#include <Memory/Allocator.h>

// Направления
#define DIR_UP    0
#define DIR_DOWN  1
#define DIR_LEFT  2
#define DIR_RIGHT 3

// Цвета
#define COLOR_BG       RGB_GRAY20
#define COLOR_GRID     RGB_GRAY30
#define COLOR_HEAD     RGB_LIME_GREEN
#define COLOR_BODY     RGB_GREEN
#define COLOR_BODY2    RGB_DARK_GREEN
#define COLOR_FOOD     RGB_RED
#define COLOR_FOOD_GLOW RGB_LIGHT_RED
#define COLOR_TEXT     RGB_WHITE
#define COLOR_TEXT_SHADOW RGB_BLACK

typedef struct {
    INT X, Y;
} Point;

typedef struct {
    Point *Body;
    INT Length;
    INT MaxLength;
    INT Direction;
    INT NextDirection;
    Point Food;
    BOOL GameOver;
    UINT32 Score;
    UINT32 CellSize;      // размер клетки в пикселях
    UINT32 Width;         // ширина поля в клетках
    UINT32 Height;        // высота поля в клетках
    UINT32 OffsetX;       // отступ слева
    UINT32 OffsetY;       // отступ сверху
} SnakeState;

static SnakeState GSnake;
static BOOL SnakePaused = FALSE;
static BOOL SnakeRunning = FALSE;

// ============================================================================
// Инициализация
// ============================================================================

static NOPTR SnakeSpawnFood(NOPTR);

static NOPTR SnakeInit(NOPTR) {
    UINT32 ScreenWidth = FBDeviceGetWidth();
    UINT32 ScreenHeight = FBDeviceGetHeight();
    
    // Размер клетки: от 16 до 32 пикселей в зависимости от экрана
    if (ScreenWidth >= 1920) {
        GSnake.CellSize = 32;
    } else if (ScreenWidth >= 1280) {
        GSnake.CellSize = 28;
    } else if (ScreenWidth >= 1024) {
        GSnake.CellSize = 24;
    } else if (ScreenWidth >= 800) {
        GSnake.CellSize = 20;
    } else {
        GSnake.CellSize = 16;
    }
    
    // Размер поля: подстраиваем под экран с отступами
    UINT32 Padding = GSnake.CellSize * 2;
    UINT32 MaxWidth = ScreenWidth - Padding * 2;
    UINT32 MaxHeight = ScreenHeight - Padding * 2 - GSnake.CellSize * 2; // -2 клетки для информации
    
    GSnake.Width = MaxWidth / GSnake.CellSize;
    GSnake.Height = MaxHeight / GSnake.CellSize;
    
    // Минимальный размер
    if (GSnake.Width < 10) GSnake.Width = 10;
    if (GSnake.Height < 8) GSnake.Height = 8;
    
    // Центрируем поле
    GSnake.OffsetX = (ScreenWidth - GSnake.Width * GSnake.CellSize) / 2;
    GSnake.OffsetY = (ScreenHeight - GSnake.Height * GSnake.CellSize) / 2 - GSnake.CellSize;
    
    // Максимальная длина змейки
    GSnake.MaxLength = GSnake.Width * GSnake.Height;
    GSnake.Body = (Point*)MemoryAllocate(GSnake.MaxLength * sizeof(Point));
    if (!GSnake.Body) {
        return;
    }
    
    // Начальная позиция (центр)
    INT StartX = GSnake.Width / 2;
    INT StartY = GSnake.Height / 2;
    
    GSnake.Length = 3;
    GSnake.Body[0].X = StartX;
    GSnake.Body[0].Y = StartY;
    GSnake.Body[1].X = StartX - 1;
    GSnake.Body[1].Y = StartY;
    GSnake.Body[2].X = StartX - 2;
    GSnake.Body[2].Y = StartY;
    
    GSnake.Direction = DIR_RIGHT;
    GSnake.NextDirection = DIR_RIGHT;
    GSnake.GameOver = FALSE;
    GSnake.Score = 0;
    
    SnakeSpawnFood();
}

static NOPTR SnakeSpawnFood(NOPTR) {
    UINT32 Attempts = 0;
    BOOL Placed = FALSE;
    
    while (!Placed && Attempts < 10000) {
        INT X = (INT)(TimerTicks() % (GSnake.Width));
        INT Y = (INT)((TimerTicks() / 100) % (GSnake.Height));
        
        BOOL Occupied = FALSE;
        for (INT I = 0; I < GSnake.Length; I++) {
            if (GSnake.Body[I].X == X && GSnake.Body[I].Y == Y) {
                Occupied = TRUE;
                break;
            }
        }
        
        if (!Occupied) {
            GSnake.Food.X = X;
            GSnake.Food.Y = Y;
            Placed = TRUE;
        }
        Attempts++;
    }
    
    if (!Placed) {
        GSnake.Food.X = GSnake.Width / 2;
        GSnake.Food.Y = GSnake.Height / 2;
    }
}

// ============================================================================
// Отрисовка
// ============================================================================

static NOPTR SnakeDrawCell(INT X, INT Y, FBDeviceColor Color) {
    UINT32 Px = GSnake.OffsetX + X * GSnake.CellSize;
    UINT32 Py = GSnake.OffsetY + Y * GSnake.CellSize;
    
    // Закруглённый прямоугольник (рисуем 4 маленьких квадрата по углам)
    UINT32 R = GSnake.CellSize / 5; // радиус скругления
    if (R < 2) R = 2;
    if (R > GSnake.CellSize / 2) R = GSnake.CellSize / 2;
    
    // Основной прямоугольник
    FBDeviceFillRect(Px, Py, GSnake.CellSize, GSnake.CellSize, Color);
    
    // Углы закрашиваем фоном (имитация скругления)
    if (R > 0) {
        FBDeviceColor Bg = (GSnake.CellSize > 20) ? RGB_GRAY15 : COLOR_BG;
        // Верхний-левый
        FBDeviceFillRect(Px, Py, R, R, Bg);
        // Верхний-правый
        FBDeviceFillRect(Px + GSnake.CellSize - R, Py, R, R, Bg);
        // Нижний-левый
        FBDeviceFillRect(Px, Py + GSnake.CellSize - R, R, R, Bg);
        // Нижний-правый
        FBDeviceFillRect(Px + GSnake.CellSize - R, Py + GSnake.CellSize - R, R, R, Bg);
    }
}

static NOPTR SnakeDrawField(NOPTR) {
    // Фон поля (сетка)
    for (UINT32 Y = 0; Y < GSnake.Height; Y++) {
        for (UINT32 X = 0; X < GSnake.Width; X++) {
            FBDeviceColor Color = ((X + Y) % 2 == 0) ? RGB_GRAY15 : RGB_GRAY20;
            UINT32 Px = GSnake.OffsetX + X * GSnake.CellSize;
            UINT32 Py = GSnake.OffsetY + Y * GSnake.CellSize;
            FBDeviceFillRect(Px, Py, GSnake.CellSize, GSnake.CellSize, Color);
        }
    }
}

static NOPTR SnakeDraw(NOPTR) {
    // Очищаем экран
    FBDeviceClear(RGB_BLACK);
    
    // Рисуем поле
    SnakeDrawField();
    
    // Рисуем еду (с glow-эффектом)
    if (GSnake.Food.X >= 0 && GSnake.Food.X < (INT)GSnake.Width &&
        GSnake.Food.Y >= 0 && GSnake.Food.Y < (INT)GSnake.Height) {
        
        // Glow (небольшое свечение вокруг еды)
        UINT32 Px = GSnake.OffsetX + GSnake.Food.X * GSnake.CellSize;
        UINT32 Py = GSnake.OffsetY + GSnake.Food.Y * GSnake.CellSize;
        INT GlowSize = GSnake.CellSize / 2;
        if (GlowSize > 5) {
            FBDeviceFillRect(Px - GlowSize/2, Py - GlowSize/2, 
                             GSnake.CellSize + GlowSize, GSnake.CellSize + GlowSize,
                             COLOR_FOOD_GLOW);
        }
        
        SnakeDrawCell(GSnake.Food.X, GSnake.Food.Y, COLOR_FOOD);
    }
    
    // Рисуем змейку (от хвоста к голове)
    for (INT I = GSnake.Length - 1; I >= 0; I--) {
        Point *P = &GSnake.Body[I];
        if (P->X >= 0 && P->X < (INT)GSnake.Width &&
            P->Y >= 0 && P->Y < (INT)GSnake.Height) {
            
            FBDeviceColor Color;
            if (I == 0) {
                // Голова — ярко-зелёная
                Color = COLOR_HEAD;
            } else {
                // Тело — градиент от зелёного к тёмно-зелёному
                FLOAT T = (FLOAT)I / GSnake.Length;
                UINT8 R = (UINT8)((1 - T) * 0 + T * 0);
                UINT8 G = (UINT8)((1 - T) * 200 + T * 50);
                UINT8 B = (UINT8)((1 - T) * 0 + T * 0);
                Color = FBDEVICE_RGB(R, G, B);
            }
            SnakeDrawCell(P->X, P->Y, Color);
        }
    }
    
    // Рамка поля
    UINT32 FrameX = GSnake.OffsetX - 2;
    UINT32 FrameY = GSnake.OffsetY - 2;
    UINT32 FrameW = GSnake.Width * GSnake.CellSize + 4;
    UINT32 FrameH = GSnake.Height * GSnake.CellSize + 4;
    FBDeviceDrawRect(FrameX, FrameY, FrameW, FrameH, RGB_GRAY40);
    FBDeviceDrawRect(FrameX + 1, FrameY + 1, FrameW - 2, FrameH - 2, RGB_GRAY30);
    
    // Информация внизу
    CHAR Text[128];
    SnPrintf(Text, sizeof(Text), "SCORE: %u  LENGTH: %d  SIZE: %dx%d", 
             GSnake.Score, GSnake.Length, GSnake.Width, GSnake.Height);
    
    UINT32 TextX = (FBDeviceGetWidth() - FBDeviceTextWidth(Text)) / 2;
    UINT32 TextY = GSnake.OffsetY + GSnake.Height * GSnake.CellSize + GSnake.CellSize;
    
    // Тень текста
    FBDeviceDrawString(Text, TextX + 1, TextY + 1, RGB_BLACK, RGB_BLACK);
    FBDeviceDrawString(Text, TextX, TextY, RGB_WHITE, RGB_BLACK);
    
    // Управление
    CHAR Controls[] = "WASD/ARROWS: MOVE   P: PAUSE   Q: QUIT";
    UINT32 CtrlX = (FBDeviceGetWidth() - FBDeviceTextWidth(Controls)) / 2;
    UINT32 CtrlY = TextY + FONT_LINE_HEIGHT;
    FBDeviceDrawString(Controls, CtrlX, CtrlY, RGB_GRAY70, RGB_BLACK);
    
    // Если игра на паузе
    if (SnakePaused) {
        CHAR PauseText[] = "*** PAUSED ***";
        UINT32 PX = (FBDeviceGetWidth() - FBDeviceTextWidth(PauseText)) / 2;
        UINT32 PY = GSnake.OffsetY + GSnake.Height * GSnake.CellSize / 2;
        FBDeviceDrawString(PauseText, PX, PY, RGB_YELLOW, RGB_BLACK);
    }
    
    FBDeviceSwapBuffers();
}

// ============================================================================
// Логика игры
// ============================================================================

static BOOL SnakeMove(NOPTR) {
    GSnake.Direction = GSnake.NextDirection;
    
    Point NewHead = GSnake.Body[0];
    switch (GSnake.Direction) {
        case DIR_UP:    NewHead.Y--; break;
        case DIR_DOWN:  NewHead.Y++; break;
        case DIR_LEFT:  NewHead.X--; break;
        case DIR_RIGHT: NewHead.X++; break;
    }
    
    // Столкновение со стеной
    if (NewHead.X < 0 || NewHead.X >= (INT)GSnake.Width ||
        NewHead.Y < 0 || NewHead.Y >= (INT)GSnake.Height) {
        return FALSE;
    }
    
    // Проверка на еду
    BOOL AteFood = (NewHead.X == GSnake.Food.X && NewHead.Y == GSnake.Food.Y);
    INT TailIndex = GSnake.Length - 1;
    
    // Столкновение с собой
    for (INT I = 0; I < GSnake.Length; I++) {
        if (!AteFood && I == TailIndex) continue;
        if (GSnake.Body[I].X == NewHead.X && GSnake.Body[I].Y == NewHead.Y) {
            return FALSE;
        }
    }
    
    // Движение
    for (INT I = GSnake.Length - 1; I > 0; I--) {
        GSnake.Body[I] = GSnake.Body[I - 1];
    }
    GSnake.Body[0] = NewHead;
    
    // Еда
    if (AteFood && GSnake.Length < GSnake.MaxLength) {
        GSnake.Length++;
        GSnake.Body[GSnake.Length - 1] = GSnake.Body[GSnake.Length - 2];
        GSnake.Score++;
        SnakeSpawnFood();
    }
    
    return TRUE;
}

// ============================================================================
// Ввод
// ============================================================================

static NOPTR SnakeHandleInput(KeyEvent *Event, NOPTR *Ud) {
    (NOPTR)Ud;  // контекст игры не нужен, если используешь глобальный GSnake

    // Проверка на валидность события
    if (!Event) {
        return;
    }

    // Нас интересуют только нажатия (для игрового управления)
    if (Event->State != KEY_STATE_PRESSED) {
        return;
    }

    // Выход
    if (Event->Ascii == 'q' || Event->Ascii == 'Q') {
        GSnake.GameOver = TRUE;
        return;
    }

    // Пауза
    if (Event->Ascii == 'p' || Event->Ascii == 'P') {
        SnakePaused = !SnakePaused;
        return;
    }

    if (SnakePaused) {
        return;
    }

    BOOL Handled = FALSE;

    // WASD
    switch (Event->Ascii) {
        case 'w':
        case 'W':
            if (GSnake.Direction != DIR_DOWN) {
                GSnake.NextDirection = DIR_UP;
                Handled = TRUE;
            }
            break;
        case 's':
        case 'S':
            if (GSnake.Direction != DIR_UP) {
                GSnake.NextDirection = DIR_DOWN;
                Handled = TRUE;
            }
            break;
        case 'a':
        case 'A':
            if (GSnake.Direction != DIR_RIGHT) {
                GSnake.NextDirection = DIR_LEFT;
                Handled = TRUE;
            }
            break;
        case 'd':
        case 'D':
            if (GSnake.Direction != DIR_LEFT) {
                GSnake.NextDirection = DIR_RIGHT;
                Handled = TRUE;
            }
            break;
        default:
            break;
    }

    // Если WASD обработан — не трогаем стрелки
    if (Handled) {
        return;
    }

    // Стрелки (по сканкоду)
    switch (Event->ScanCode) {
        case PS2_KEY_UP:
            if (GSnake.Direction != DIR_DOWN) GSnake.NextDirection = DIR_UP;
            break;
        case PS2_KEY_DOWN:
            if (GSnake.Direction != DIR_UP) GSnake.NextDirection = DIR_DOWN;
            break;
        case PS2_KEY_LEFT:
            if (GSnake.Direction != DIR_RIGHT) GSnake.NextDirection = DIR_LEFT;
            break;
        case PS2_KEY_RIGHT:
            if (GSnake.Direction != DIR_LEFT) GSnake.NextDirection = DIR_RIGHT;
            break;
        default:
            break;
    }
}


// ============================================================================
// Главная функция
// ============================================================================

static INT SnakeGame(NOPTR) {
    INT Result = SUCCESS;
    UINT32 WaitMs;
    
    // Отключаем консольный вывод на время игры
    ConsoleSwitch(FALSE);
    
    // Инициализация
    SnakeInit();
    if (!GSnake.Body) {
        ConsoleSwitch(TRUE);
        ConsolePrint("Not enough memory for Snake!\n");
        ConsoleReadChar();
        return NO_MEMORY;
    }
    
    // Скорость игры в зависимости от размера поля
    UINT32 TotalCells = GSnake.Width * GSnake.Height;
    if (TotalCells < 100) {
        WaitMs = 350;   // было 180
    } else if (TotalCells < 300) {
        WaitMs = 300;   // было 150
    } else {
        WaitMs = 260;   // было 120
    }
    
    SnakePaused = FALSE;
    SnakeRunning = TRUE;
    
    // Первый кадр
    SnakeDraw();
    TimerSleep(100);

    Ps2KeyboardSubscribe(SnakeHandleInput, NULLPTR);
    
    // Игровой цикл
    while (!GSnake.GameOver && SnakeRunning) {
        UINT64 StartTick = TimerTicks();
        
        if (!SnakePaused) {
            if (!SnakeMove()) {
                GSnake.GameOver = TRUE;
                break;
            }
        }
        
        SnakeDraw();
        
        // Задержка
        TimerSleep(WaitMs);
    }
    
    // Game Over экран
    FBDeviceClear(RGB_BLACK);
    
    // Заголовок
    CHAR Title[] = "=== GAME OVER ===";
    UINT32 TitleX = (FBDeviceGetWidth() - FBDeviceTextWidth(Title)) / 2;
    UINT32 TitleY = FBDeviceGetHeight() / 2 - FONT_LINE_HEIGHT * 2;
    FBDeviceDrawString(Title, TitleX, TitleY, RGB_RED, RGB_BLACK);
    
    // Счёт
    CHAR ScoreText[64];
    SnPrintf(ScoreText, sizeof(ScoreText), "Score: %u   Length: %d", GSnake.Score, GSnake.Length);
    UINT32 ScoreX = (FBDeviceGetWidth() - FBDeviceTextWidth(ScoreText)) / 2;
    UINT32 ScoreY = TitleY + FONT_LINE_HEIGHT * 2;
    FBDeviceDrawString(ScoreText, ScoreX, ScoreY, RGB_WHITE, RGB_BLACK);
    
    // Статистика
    CHAR StatsText[64];
    SnPrintf(StatsText, sizeof(StatsText), "Field: %dx%d   Cells: %d", 
             GSnake.Width, GSnake.Height, TotalCells);
    UINT32 StatsX = (FBDeviceGetWidth() - FBDeviceTextWidth(StatsText)) / 2;
    UINT32 StatsY = ScoreY + FONT_LINE_HEIGHT;
    FBDeviceDrawString(StatsText, StatsX, StatsY, RGB_GRAY70, RGB_BLACK);
    
    // Подсказка
    CHAR Prompt[] = "Press any key to return to shell...";
    UINT32 PromptX = (FBDeviceGetWidth() - FBDeviceTextWidth(Prompt)) / 2;
    UINT32 PromptY = StatsY + FONT_LINE_HEIGHT * 2;
    FBDeviceDrawString(Prompt, PromptX, PromptY, RGB_GRAY50, RGB_BLACK);
    
    FBDeviceSwapBuffers();
    
    // Ждём нажатия
    ConsoleSwitch(TRUE);
    ConsoleReadChar();
    
    // Освобождаем память
    if (GSnake.Body) {
        MemoryFree(GSnake.Body);
        GSnake.Body = NULLPTR;
    }
    Ps2KeyboardUnsubscribe(SnakeHandleInput);
    
    ConsoleClear();
    
    return SUCCESS;
}

static NOPTR SnakeThread(NOPTR *Arg) {
    (NOPTR)Arg;
    SnakeGame();
    TaskExit(0);
    return;
}

INT SnakeStart(NOPTR) {
    KTask *Task = TaskCreate("SnakeGame", SnakeThread, NULLPTR,
                              SCHED_PRIORITY_NORMAL, TASK_DEFAULT_QUANTUM);
    if (!Task) {
        ConsolePrint("Failed to create Snake thread!\n");
        return NO_MEMORY;
    }
    
    SchedulerRegisterTask(Task);
    SchedulerEnqueueReady(Task);
    return SUCCESS;
}
