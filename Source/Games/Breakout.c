#include <Games/Breakout.h>
#include <FBDevice.h>
#include <Rgb.h>
#include <Console.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Lib/Math.h>
#include <Kernel/Return.h>
#include <Kernel/Scheduler.h>
#include <Time/Timer.h>
#include <Ps2Keyboard.h>
#include <Memory/Allocator.h>
#include <Crypto/Rng.h>

// ============================================================================
// КОНСТАНТЫ
// ============================================================================

#define BALL_SIZE           6
#define PADDLE_WIDTH        60
#define PADDLE_HEIGHT       8
#define BLOCK_ROWS          6
#define BLOCK_COLS          10
#define BLOCK_WIDTH         50
#define BLOCK_HEIGHT        20
#define BLOCK_PADDING       3

#define MAX_BALLS           10
#define MAX_BONUSES         10

#define PLAYER_LIVES        3

// ЦВЕТА
#define COLOR_BG_TOP        RGB_DARK_BLUE
#define COLOR_BG_BOTTOM     RGB_BLACK
#define COLOR_PADDLE        RGB_LIME_GREEN
#define COLOR_PADDLE_GLOW   RGB_GREEN_YELLOW
#define COLOR_BALL          RGB_WHITE
#define COLOR_BALL_GLOW     RGB_CYAN
#define COLOR_BLOCK_1       RGB_RED
#define COLOR_BLOCK_2       RGB_ORANGE
#define COLOR_BLOCK_3       RGB_YELLOW
#define COLOR_BLOCK_4       RGB_GREEN
#define COLOR_BLOCK_5       RGB_BLUE
#define COLOR_BLOCK_6       RGB_PURPLE
#define COLOR_BLOCK_HARD    RGB_GRAY
#define COLOR_BONUS         RGB_GOLD
#define COLOR_SHADOW        RGB_BLACK

// ============================================================================
// СТРУКТУРЫ
// ============================================================================

typedef struct {
    FLOAT X, Y;
    FLOAT Vx, Vy;
    BOOL Active;
} Ball;

typedef struct {
    INT32 X, Y;
    INT32 Width, Height;
    INT32 HP;
    BOOL Active;
    UINT32 ColorIndex;
} Block;

typedef struct {
    INT32 X, Y;
    INT32 Type;  // 0 = широкий, 1 = бонусный, 2 = ловушка
    BOOL Active;
    FLOAT Vy;
} Bonus;

typedef struct {
    INT32 X;
    INT32 Speed;
    INT32 Width;
    INT32 Lives;
    INT32 Score;
    INT32 Level;
    BOOL GameOver;
    BOOL Paused;
    BOOL LeftPressed;
    BOOL RightPressed;
    UINT32 FrameCounter;
    Ball Balls[MAX_BALLS];
    Block Blocks[BLOCK_ROWS * BLOCK_COLS];
    Bonus Bonuses[MAX_BONUSES];
    INT32 ActiveBalls;
    INT32 ActiveBlocks;
    FLOAT ShakeX, ShakeY;
    FLOAT BallSpeedMultiplier;
    UINT32 SpeedIncreaseCounter;
} BreakoutState;

static BreakoutState GBreakout;
static BOOL BreakoutRunning = FALSE;

// ============================================================================
// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ============================================================================

static INT32 RandomRange(INT32 Min, INT32 Max) {
    UINT32 Rnd;
    RngGetRandomBytes((UINT8*)&Rnd, 4);
    return Min + (Rnd % (Max - Min + 1));
}

static FLOAT FRandomRange(FLOAT Min, FLOAT Max) {
    UINT32 Rnd;
    RngGetRandomBytes((UINT8*)&Rnd, 4);
    return Min + ((FLOAT)Rnd / 0xFFFFFFFF) * (Max - Min);
}

static FBDeviceColor GetBlockColor(INT32 HP) {
    switch (HP % 6) {
        case 0: return COLOR_BLOCK_1;
        case 1: return COLOR_BLOCK_2;
        case 2: return COLOR_BLOCK_3;
        case 3: return COLOR_BLOCK_4;
        case 4: return COLOR_BLOCK_5;
        case 5: return COLOR_BLOCK_6;
        default: return COLOR_BLOCK_1;
    }
}

// ============================================================================
// ГЕНЕРАЦИЯ УРОВНЯ (РАНДОМНАЯ)
// ============================================================================

static NOPTR GenerateLevel(NOPTR) {
    INT32 TotalBlocks = 0;
    INT32 ScreenWidth = FBDeviceGetWidth();
    INT32 ScreenHeight = FBDeviceGetHeight();
    
    INT32 OffsetX = (ScreenWidth - BLOCK_COLS * (BLOCK_WIDTH + BLOCK_PADDING)) / 2;
    INT32 OffsetY = 60;
    
    for (INT I = 0; I < BLOCK_ROWS * BLOCK_COLS; I++) {
        GBreakout.Blocks[I].Active = FALSE;
    }
    
    INT32 MaxHP = 1 + GBreakout.Level / 3;
    if (MaxHP > 5) MaxHP = 5;
    
    FLOAT FillFactor = 0.4f + GBreakout.Level * 0.02f;
    if (FillFactor > 0.85f) FillFactor = 0.85f;
    
    for (INT Row = 0; Row < BLOCK_ROWS; Row++) {
        for (INT Col = 0; Col < BLOCK_COLS; Col++) {
            INT32 Index = Row * BLOCK_COLS + Col;
            
            if (FRandomRange(0, 1) > FillFactor) continue;
            
            if (Row > 2 && Row < 5 && Col > 3 && Col < 7) {
                if (FRandomRange(0, 1) > 0.3f) continue;
            }
            
            GBreakout.Blocks[Index].Active = TRUE;
            GBreakout.Blocks[Index].X = OffsetX + Col * (BLOCK_WIDTH + BLOCK_PADDING);
            GBreakout.Blocks[Index].Y = OffsetY + Row * (BLOCK_HEIGHT + BLOCK_PADDING);
            GBreakout.Blocks[Index].Width = BLOCK_WIDTH;
            GBreakout.Blocks[Index].Height = BLOCK_HEIGHT;
            GBreakout.Blocks[Index].HP = RandomRange(1, MaxHP);
            GBreakout.Blocks[Index].ColorIndex = RandomRange(0, 5);
            
            TotalBlocks++;
        }
    }
    
    if (TotalBlocks < 5) {
        for (INT Row = 0; Row < 3; Row++) {
            for (INT Col = 0; Col < 5; Col++) {
                INT32 Index = Row * BLOCK_COLS + Col + 2;
                GBreakout.Blocks[Index].Active = TRUE;
                GBreakout.Blocks[Index].X = OffsetX + Col * (BLOCK_WIDTH + BLOCK_PADDING) + 50;
                GBreakout.Blocks[Index].Y = OffsetY + Row * (BLOCK_HEIGHT + BLOCK_PADDING) + 20;
                GBreakout.Blocks[Index].Width = BLOCK_WIDTH;
                GBreakout.Blocks[Index].Height = BLOCK_HEIGHT;
                GBreakout.Blocks[Index].HP = 1;
                GBreakout.Blocks[Index].ColorIndex = Row;
                TotalBlocks++;
            }
        }
    }
    
    GBreakout.ActiveBlocks = TotalBlocks;
}

// ============================================================================
// ИНИЦИАЛИЗАЦИЯ
// ============================================================================

static NOPTR BreakoutInit(NOPTR) {
    UINT32 ScreenWidth = FBDeviceGetWidth();
    UINT32 ScreenHeight = FBDeviceGetHeight();
    
    MemSet(&GBreakout, 0, sizeof(BreakoutState));
    GBreakout.X = ScreenWidth / 2 - PADDLE_WIDTH / 2;
    GBreakout.Width = PADDLE_WIDTH;
    GBreakout.Lives = PLAYER_LIVES;
    GBreakout.Score = 0;
    GBreakout.Level = 1;
    GBreakout.GameOver = FALSE;
    GBreakout.Paused = FALSE;
    GBreakout.Speed = 10;
    GBreakout.BallSpeedMultiplier = 1.0f;
    GBreakout.SpeedIncreaseCounter = 0;
    
    for (INT I = 0; I < MAX_BALLS; I++) {
        GBreakout.Balls[I].Active = FALSE;
    }
    
    for (INT I = 0; I < MAX_BONUSES; I++) {
        GBreakout.Bonuses[I].Active = FALSE;
    }
    
    GBreakout.Balls[0].Active = TRUE;
    GBreakout.Balls[0].X = GBreakout.X + PADDLE_WIDTH / 2;
    GBreakout.Balls[0].Y = ScreenHeight - 100;
    GBreakout.Balls[0].Vx = FRandomRange(-3, 3);
    GBreakout.Balls[0].Vy = -4 - GBreakout.Level * 0.2f;
    if (GBreakout.Balls[0].Vx == 0) GBreakout.Balls[0].Vx = 1;
    GBreakout.ActiveBalls = 1;
    
    GenerateLevel();
}

// ============================================================================
// ОТРИСОВКА
// ============================================================================

static NOPTR BreakoutDrawBackground(NOPTR) {
    UINT32 Width = FBDeviceGetWidth();
    UINT32 Height = FBDeviceGetHeight();
    
    for (UINT32 Y = 0; Y < Height; Y++) {
        FLOAT T = (FLOAT)Y / Height;
        UINT8 R = (UINT8)(COLOR_BG_TOP.R * (1 - T) + COLOR_BG_BOTTOM.R * T);
        UINT8 G = (UINT8)(COLOR_BG_TOP.G * (1 - T) + COLOR_BG_BOTTOM.G * T);
        UINT8 B = (UINT8)(COLOR_BG_TOP.B * (1 - T) + COLOR_BG_BOTTOM.B * T);
        FBDeviceDrawLine(0, Y, Width, Y, FBDEVICE_RGB(R, G, B));
    }
    
    static UINT32 StarSeed = 0x1234;
    for (INT I = 0; I < 30; I++) {
        INT32 X = (StarSeed * (I * 0x9E3779B9 + 0x12345678)) % Width;
        INT32 Y = (StarSeed * (I * 0x9E3779B9 + 0x87654321)) % (INT32)(Height * 0.5);
        FBDevicePutPixel(X, Y, RGB_GRAY30);
    }
    StarSeed += 0x9E3779B9;
}

static NOPTR BreakoutDrawBlocks(NOPTR) {
    INT32 TotalBlocks = BLOCK_ROWS * BLOCK_COLS;
    
    for (INT I = 0; I < TotalBlocks; I++) {
        Block *B = &GBreakout.Blocks[I];
        if (!B->Active) continue;
        
        FBDeviceColor Color = GetBlockColor(B->HP);
        
        FBDeviceFillRect(B->X + 2, B->Y + 2, B->Width, B->Height, COLOR_SHADOW);
        FBDeviceFillRect(B->X, B->Y, B->Width, B->Height, Color);
        FBDeviceFillRect(B->X + 2, B->Y + 1, B->Width - 4, 3, RGB_WHITE);
        FBDeviceDrawRect(B->X, B->Y, B->Width, B->Height, RGB_BLACK);
        
        if (B->HP > 1) {
            CHAR HPStr[4];
            SnPrintf(HPStr, sizeof(HPStr), "%d", B->HP);
            UINT32 TX = B->X + B->Width / 2 - 4;
            UINT32 TY = B->Y + B->Height / 2 - 6;
            FBDeviceDrawString(HPStr, TX, TY, RGB_WHITE, RGB_BLACK);
        }
    }
}

static NOPTR BreakoutDrawBalls(NOPTR) {
    for (INT I = 0; I < MAX_BALLS; I++) {
        Ball *B = &GBreakout.Balls[I];
        if (!B->Active) continue;
        
        INT32 X = (INT32)B->X;
        INT32 Y = (INT32)B->Y;
        
        FBDeviceFillCircle(X, Y, BALL_SIZE + 4, COLOR_BALL_GLOW);
        FBDeviceFillCircle(X, Y, BALL_SIZE, COLOR_BALL);
        FBDeviceFillCircle(X - 2, Y - 2, 2, RGB_WHITE);
    }
}

static NOPTR BreakoutDrawPaddle(NOPTR) {
    INT32 X = GBreakout.X;
    INT32 Y = FBDeviceGetHeight() - 50;
    INT32 W = GBreakout.Width;
    INT32 H = PADDLE_HEIGHT;
    
    FBDeviceFillRect(X + 3, Y + 3, W, H, COLOR_SHADOW);
    
    for (INT I = 0; I < H; I++) {
        FLOAT T = (FLOAT)I / H;
        UINT8 R = (UINT8)(COLOR_PADDLE.R * (1 - T) + COLOR_PADDLE_GLOW.R * T);
        UINT8 G = (UINT8)(COLOR_PADDLE.G * (1 - T) + COLOR_PADDLE_GLOW.G * T);
        UINT8 B = (UINT8)(COLOR_PADDLE.B * (1 - T) + COLOR_PADDLE_GLOW.B * T);
        FBDeviceDrawLine(X, Y + I, X + W - 1, Y + I, FBDEVICE_RGB(R, G, B));
    }
    
    FBDeviceDrawRect(X, Y, W, H, RGB_WHITE);
    FBDeviceFillRect(X + 2, Y + 2, 4, H - 4, RGB_CYAN);
    FBDeviceFillRect(X + W - 6, Y + 2, 4, H - 4, RGB_CYAN);
}

static NOPTR BreakoutDrawBonuses(NOPTR) {
    for (INT I = 0; I < MAX_BONUSES; I++) {
        Bonus *B = &GBreakout.Bonuses[I];
        if (!B->Active) continue;
        
        FBDeviceFillRect(B->X - 2, B->Y - 2, 14, 14, COLOR_BONUS);
        FBDeviceFillRect(B->X, B->Y, 10, 10, RGB_YELLOW);
        
        CHAR Icon = '?';
        switch (B->Type) {
            case 0: Icon = 'W'; break;
            case 1: Icon = '+'; break;
            case 2: Icon = 'S'; break;
        }
        FBDeviceDrawString(&Icon, B->X + 2, B->Y + 1, RGB_BLACK, RGB_YELLOW);
    }
}

static NOPTR BreakoutDrawUI(NOPTR) {
    UINT32 Width = FBDeviceGetWidth();
    UINT32 Height = FBDeviceGetHeight();
    CHAR Info[128];
    
    SnPrintf(Info, sizeof(Info), "SCORE: %u", GBreakout.Score);
    FBDeviceDrawString(Info, 10, 10, RGB_WHITE, RGB_BLACK);
    
    SnPrintf(Info, sizeof(Info), "LEVEL: %u", GBreakout.Level);
    FBDeviceDrawString(Info, 10, 30, RGB_GRAY70, RGB_BLACK);
    
    SnPrintf(Info, sizeof(Info), "LIVES: %u", GBreakout.Lives);
    FBDeviceDrawString(Info, 10, 50, RGB_RED, RGB_BLACK);
    
    SnPrintf(Info, sizeof(Info), "BLOCKS: %d", GBreakout.ActiveBlocks);
    UINT32 InfoX = Width - FBDeviceTextWidth(Info) - 10;
    FBDeviceDrawString(Info, InfoX, 10, RGB_GRAY70, RGB_BLACK);
    
    CHAR Controls[] = "<- -> - Move | P - Pause | Q - Quit";
    UINT32 CtrlX = (Width - FBDeviceTextWidth(Controls)) / 2;
    FBDeviceDrawString(Controls, CtrlX, Height - 20, RGB_GRAY70, RGB_BLACK);
    
    if (GBreakout.Paused) {
        CHAR PauseText[] = "PAUSED";
        UINT32 PX = (Width - FBDeviceTextWidth(PauseText)) / 2;
        FBDeviceDrawString(PauseText, PX, Height/2 - 20, RGB_YELLOW, RGB_BLACK);
    }
    
    if (GBreakout.GameOver) {
        CHAR GoText[] = "GAME OVER";
        UINT32 GX = (Width - FBDeviceTextWidth(GoText)) / 2;
        FBDeviceDrawString(GoText, GX, Height/2 - 40, RGB_RED, RGB_BLACK);
        
        CHAR ScoreText[64];
        SnPrintf(ScoreText, sizeof(ScoreText), "Final Score: %u", GBreakout.Score);
        UINT32 SX = (Width - FBDeviceTextWidth(ScoreText)) / 2;
        FBDeviceDrawString(ScoreText, SX, Height/2, RGB_WHITE, RGB_BLACK);
        
        CHAR RestartText[] = "Press SPACE to restart or Q to quit";
        UINT32 RX = (Width - FBDeviceTextWidth(RestartText)) / 2;
        FBDeviceDrawString(RestartText, RX, Height/2 + 40, RGB_GRAY70, RGB_BLACK);
    }
}

static NOPTR BreakoutDraw(NOPTR) {
    BreakoutDrawBackground();
    BreakoutDrawBlocks();
    BreakoutDrawBonuses();
    BreakoutDrawBalls();
    BreakoutDrawPaddle();
    BreakoutDrawUI();
    FBDeviceSwapBuffers();
}

// ============================================================================
// ФИЗИКА И ЛОГИКА
// ============================================================================

static NOPTR SpawnBonus(INT32 X, INT32 Y) {
    for (INT I = 0; I < MAX_BONUSES; I++) {
        if (!GBreakout.Bonuses[I].Active) {
            GBreakout.Bonuses[I].Active = TRUE;
            GBreakout.Bonuses[I].X = X;
            GBreakout.Bonuses[I].Y = Y;
            GBreakout.Bonuses[I].Vy = 1.5f;
            GBreakout.Bonuses[I].Type = RandomRange(0, 2);
            return;
        }
    }
}

static NOPTR BreakoutUpdate(NOPTR) {
    if (GBreakout.Paused || GBreakout.GameOver) return;

    GBreakout.SpeedIncreaseCounter++;
    
    // Каждые 5 секунд (примерно 300 кадров при 60 FPS) увеличиваем скорость на 3%
    if (GBreakout.SpeedIncreaseCounter >= 300) {
        GBreakout.SpeedIncreaseCounter = 0;
        GBreakout.BallSpeedMultiplier += 0.03f;  // +3% к скорости
        if (GBreakout.BallSpeedMultiplier > 3.0f) {
            GBreakout.BallSpeedMultiplier = 3.0f;  // Максимум x3
        }
    }
    
    UINT32 Width = FBDeviceGetWidth();
    UINT32 Height = FBDeviceGetHeight();
    INT32 PaddleY = Height - 50;
    INT32 PaddleX = GBreakout.X;
    INT32 PaddleW = GBreakout.Width;
    
    if (GBreakout.LeftPressed) {
        GBreakout.X -= GBreakout.Speed;
    }
    if (GBreakout.RightPressed) {
        GBreakout.X += GBreakout.Speed;
    }
    if (GBreakout.X < 0) GBreakout.X = 0;
    if (GBreakout.X > Width - PaddleW) GBreakout.X = Width - PaddleW;
    
    for (INT I = 0; I < MAX_BALLS; I++) {
        Ball *B = &GBreakout.Balls[I];
        if (!B->Active) continue;

        FLOAT CurrentSpeed = SqrtF(B->Vx * B->Vx + B->Vy * B->Vy);
        if (CurrentSpeed > 0.1f) {
            FLOAT TargetSpeed = 4.0f * GBreakout.BallSpeedMultiplier;
            FLOAT SpeedFactor = TargetSpeed / CurrentSpeed;
            B->Vx *= SpeedFactor;
            B->Vy *= SpeedFactor;
        }
        
        B->X += B->Vx;
        B->Y += B->Vy;
        
        if (B->X - BALL_SIZE < 0) {
            B->X = BALL_SIZE;
            B->Vx = -B->Vx;
        }
        if (B->X + BALL_SIZE > Width) {
            B->X = Width - BALL_SIZE;
            B->Vx = -B->Vx;
        }
        if (B->Y - BALL_SIZE < 0) {
            B->Y = BALL_SIZE;
            B->Vy = -B->Vy;
        }
        
        if (B->Y + BALL_SIZE > Height) {
            B->Active = FALSE;
            GBreakout.ActiveBalls--;
            continue;
        }
        
        if (B->Y + BALL_SIZE >= PaddleY &&
            B->Y - BALL_SIZE <= PaddleY + PADDLE_HEIGHT &&
            B->X >= PaddleX - BALL_SIZE &&
            B->X <= PaddleX + PaddleW + BALL_SIZE) {
    
            FLOAT HitPos = (B->X - PaddleX) / PaddleW - 0.5f;
            B->Vx = HitPos * 5;
    
            // ПРИМЕНЯЕМ УСКОРЕНИЕ
            FLOAT BaseSpeed = 4.0f + GBreakout.Level * 0.15f;
            B->Vy = -BaseSpeed * GBreakout.BallSpeedMultiplier;
    
            // Минимальная скорость по X с учётом ускорения
            FLOAT MinVx = 1.5f * GBreakout.BallSpeedMultiplier;
            if (B->Vx > -MinVx && B->Vx < MinVx) {
                B->Vx = (B->Vx >= 0) ? MinVx : -MinVx;
            }
    
            B->Y = PaddleY - BALL_SIZE;
        }
        
        INT32 TotalBlocks = BLOCK_ROWS * BLOCK_COLS;
        for (INT J = 0; J < TotalBlocks; J++) {
            Block *Block = &GBreakout.Blocks[J];
            if (!Block->Active) continue;
            
            INT32 BX = Block->X;
            INT32 BY = Block->Y;
            INT32 BW = Block->Width;
            INT32 BH = Block->Height;
            
            if (B->X + BALL_SIZE >= BX && B->X - BALL_SIZE <= BX + BW &&
                B->Y + BALL_SIZE >= BY && B->Y - BALL_SIZE <= BY + BH) {
                
                FLOAT OverlapX = (B->X < BX + BW/2) ? 
                    (B->X + BALL_SIZE - BX) : (BX + BW - (B->X - BALL_SIZE));
                FLOAT OverlapY = (B->Y < BY + BH/2) ?
                    (B->Y + BALL_SIZE - BY) : (BY + BH - (B->Y - BALL_SIZE));
                
                if (OverlapX < OverlapY) {
                    B->Vx = -B->Vx;
                } else {
                    B->Vy = -B->Vy;
                }
                
                Block->HP--;
                if (Block->HP <= 0) {
                    Block->Active = FALSE;
                    GBreakout.ActiveBlocks--;
                    GBreakout.Score += 10;
                    
                    if (RandomRange(0, 100) < 15) {
                        SpawnBonus(Block->X + Block->Width/2, Block->Y + Block->Height/2);
                    }
                }
                
                break;
            }
        }
    }
    
    for (INT I = 0; I < MAX_BONUSES; I++) {
        Bonus *B = &GBreakout.Bonuses[I];
        if (!B->Active) continue;
        
        B->Y += B->Vy;
        
        if (B->Y + 10 >= PaddleY && B->Y <= PaddleY + PADDLE_HEIGHT &&
            B->X + 10 >= PaddleX && B->X <= PaddleX + PaddleW) {
            
            B->Active = FALSE;
            
            switch (B->Type) {
                case 0:
                    GBreakout.Width = PADDLE_WIDTH * 1.5;
                    break;
                case 1:
                    for (INT J = 0; J < MAX_BALLS; J++) {
                        if (!GBreakout.Balls[J].Active) {
                            GBreakout.Balls[J].Active = TRUE;
                            GBreakout.Balls[J].X = GBreakout.X + PaddleW/2;
                            GBreakout.Balls[J].Y = PaddleY - BALL_SIZE;
                            GBreakout.Balls[J].Vx = FRandomRange(-3, 3);
                            GBreakout.Balls[J].Vy = -4 - GBreakout.Level * 0.15f;
                            if (GBreakout.Balls[J].Vx == 0) GBreakout.Balls[J].Vx = 1;
                            GBreakout.ActiveBalls++;
                            break;
                        }
                    }
                    break;
                case 2:
                    for (INT J = 0; J < MAX_BALLS; J++) {
                        if (GBreakout.Balls[J].Active) {
                            GBreakout.Balls[J].Vx *= 0.7f;
                            GBreakout.Balls[J].Vy *= 0.7f;
                        }
                    }
                    break;
            }
        }
        
        if (B->Y > Height) {
            B->Active = FALSE;
        }
    }
    
    if (GBreakout.ActiveBlocks == 0) {
        GBreakout.Level++;
        GenerateLevel();
        
        for (INT I = 0; I < MAX_BALLS; I++) {
            GBreakout.Balls[I].Active = FALSE;
        }
        GBreakout.Balls[0].Active = TRUE;
        GBreakout.Balls[0].X = GBreakout.X + PaddleW/2;
        GBreakout.Balls[0].Y = PaddleY - BALL_SIZE;
        GBreakout.Balls[0].Vx = FRandomRange(-3, 3);
        GBreakout.Balls[0].Vy = -4 - GBreakout.Level * 0.15f;
        if (GBreakout.Balls[0].Vx == 0) GBreakout.Balls[0].Vx = 1;
        GBreakout.ActiveBalls = 1;
    }
    
    if (GBreakout.ActiveBalls <= 0) {
        GBreakout.Lives--;
        if (GBreakout.Lives <= 0) {
            GBreakout.GameOver = TRUE;
        } else {
            GBreakout.Balls[0].Active = TRUE;
            GBreakout.Balls[0].X = GBreakout.X + PaddleW/2;
            GBreakout.Balls[0].Y = PaddleY - BALL_SIZE;
            GBreakout.Balls[0].Vx = FRandomRange(-3, 3);
            GBreakout.Balls[0].Vy = -4 - GBreakout.Level * 0.15f;
            if (GBreakout.Balls[0].Vx == 0) GBreakout.Balls[0].Vx = 1;
            GBreakout.ActiveBalls = 1;
        }
    }
}

// ============================================================================
// ВВОД
// ============================================================================

static NOPTR BreakoutHandleInput(KeyEvent *Event, NOPTR *Ud) {
    (NOPTR)Ud;
    if (!Event) return;
    
    if (Event->State == KEY_STATE_PRESSED) {
        switch (Event->Ascii) {
            case 'q': case 'Q':
                GBreakout.GameOver = TRUE;
                BreakoutRunning = FALSE;
                return;
            case 'p': case 'P':
                GBreakout.Paused = !GBreakout.Paused;
                return;
            case ' ':
                if (GBreakout.GameOver) {
                    BreakoutInit();
                }
                return;
        }
        
        if (GBreakout.GameOver) return;
        
        switch (Event->ScanCode) {
            case PS2_KEY_LEFT:  GBreakout.LeftPressed = TRUE; break;
            case PS2_KEY_RIGHT: GBreakout.RightPressed = TRUE; break;
        }
    }
    
    if (Event->State == KEY_STATE_RELEASED) {
        switch (Event->ScanCode) {
            case PS2_KEY_LEFT:  GBreakout.LeftPressed = FALSE; break;
            case PS2_KEY_RIGHT: GBreakout.RightPressed = FALSE; break;
        }
    }
}

// ============================================================================
// ПОТОК ИГРЫ
// ============================================================================

static NOPTR BreakoutThread(NOPTR *Arg) {
    (NOPTR)Arg;
    
    ConsoleSwitch(FALSE);
    Ps2KeyboardSubscribe(BreakoutHandleInput, NULLPTR);
    
    BreakoutInit();
    BreakoutRunning = TRUE;
    
    UINT32 WaitMs = 16;
    
    while (BreakoutRunning && !GBreakout.GameOver) {
        BreakoutUpdate();
        BreakoutDraw();
        TimerSleep(WaitMs);
    }
    
    while (GBreakout.GameOver && BreakoutRunning) {
        BreakoutDraw();
        TimerSleep(WaitMs);
    }
    
    Ps2KeyboardUnsubscribe(BreakoutHandleInput);
    ConsoleSwitch(TRUE);
    ConsoleClear();
    return;
}

// ============================================================================
// ПУБЛИЧНЫЕ ФУНКЦИИ
// ============================================================================

INT BreakoutStart(NOPTR) {
    KTask *Task = TaskCreate("Breakout", BreakoutThread, NULLPTR,
                              SCHED_PRIORITY_NORMAL, TASK_DEFAULT_QUANTUM);
    if (!Task) {
        ConsolePrint("Failed to create Breakout thread!\n");
        return NO_MEMORY;
    }
    
    SchedulerRegisterTask(Task);
    SchedulerEnqueueReady(Task);
    return SUCCESS;
}
