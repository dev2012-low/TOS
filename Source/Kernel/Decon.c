#include <Decon.h>
#include <Lib/StdIo.h>
#include <Lib/String.h>
#include <Memory/Allocator.h>
#include <Kernel/SpinLock.h>
#include <Console.h>

// ============================================================================
// Конфигурация Decon
// ============================================================================

#define DECON_BUFFER_SIZE      (256 * 1024)  // 256 KB
#define DECON_MAX_LINES        4096

// ============================================================================
// Структура буфера
// ============================================================================

typedef struct {
    CHAR   *Buffer;
    UINT32  Size;
    UINT32  Used;
    UINT32  Lines;
    UINT32  LineStarts[DECON_MAX_LINES];
    SpinLock Lock;
    BOOL    Initialized;
} DeconState;

static DeconState GDecon;

// ============================================================================
// Внутренние функции
// ============================================================================

static NOPTR DeconAddLineStart(UINT32 Pos) {
    if (GDecon.Lines < DECON_MAX_LINES) {
        GDecon.LineStarts[GDecon.Lines++] = Pos;
    } else {
        // Сдвигаем старые записи (кольцевой буфер индексов)
        MemMove(GDecon.LineStarts, GDecon.LineStarts + 1, 
                (DECON_MAX_LINES - 1) * sizeof(UINT32));
        GDecon.LineStarts[DECON_MAX_LINES - 1] = Pos;
    }
}

static NOPTR DeconWriteChar(CHAR C) {
    if (GDecon.Used >= GDecon.Size - 1) {
        // Буфер переполнен — сдвигаем на четверть
        UINT32 NewSize = GDecon.Size * 3 / 4;
        UINT32 MoveBytes = GDecon.Used - NewSize;
        
        MemMove(GDecon.Buffer, GDecon.Buffer + MoveBytes, NewSize);
        GDecon.Used = NewSize;
        
        // Обновляем индексы строк
        for (UINT32 I = 0; I < GDecon.Lines; I++) {
            if (GDecon.LineStarts[I] >= MoveBytes) {
                GDecon.LineStarts[I] -= MoveBytes;
            } else {
                GDecon.LineStarts[I] = 0;
            }
        }
    }
    
    GDecon.Buffer[GDecon.Used++] = C;
    
    if (C == '\n') {
        DeconAddLineStart(GDecon.Used);
    }
}

static NOPTR DeconWriteString(const CHAR *Str, UINT32 Len) {
    for (UINT32 I = 0; I < Len; I++) {
        DeconWriteChar(Str[I]);
    }
}

// ============================================================================
// Публичные API
// ============================================================================

NOPTR DeconInit(NOPTR) {
    if (GDecon.Initialized) {
        return;
    }
    
    MemSet(&GDecon, 0, sizeof(GDecon));
    SpinLockInit(&GDecon.Lock);
    
    GDecon.Size = DECON_BUFFER_SIZE;
    GDecon.Buffer = (CHAR*)MemoryAllocate(GDecon.Size);
    
    if (!GDecon.Buffer) {
        // Если не хватило памяти — используем статический буфер (меньше)
        GDecon.Size = 64 * 1024;
        GDecon.Buffer = (CHAR*)MemoryAllocate(GDecon.Size);
        if (!GDecon.Buffer) {
            return;
        }
    }
    
    MemSet(GDecon.Buffer, 0, GDecon.Size);
    GDecon.Used = 0;
    GDecon.Lines = 0;
    GDecon.Initialized = TRUE;
}

NOPTR DeconPrint(const CHAR *Fmt, ...) {
    if (!GDecon.Initialized) {
        return;
    }
    
    VA_LIST Args;
    CHAR TempBuf[512];
    INT Len;
    
    VaStart(Args, Fmt);
    Len = VsnPrintf(TempBuf, sizeof(TempBuf), Fmt, Args);
    VaEnd(Args);
    
    if (Len <= 0) {
        return;
    }
    
    SpinLockAcquire(&GDecon.Lock);
    DeconWriteString(TempBuf, (UINT32)Len);
    SpinLockRelease(&GDecon.Lock);
}

NOPTR DeconClear(NOPTR) {
    if (!GDecon.Initialized) {
        return;
    }
    
    SpinLockAcquire(&GDecon.Lock);
    MemSet(GDecon.Buffer, 0, GDecon.Size);
    GDecon.Used = 0;
    GDecon.Lines = 0;
    SpinLockRelease(&GDecon.Lock);
    
    ConsolePrint("Buffer cleared\n");
}

NOPTR DeconShow(NOPTR) {
    if (!GDecon.Initialized) {
        ConsolePrint("Not initialized!\n");
        return;
    }
    
    SpinLockAcquire(&GDecon.Lock);
    
    if (GDecon.Used == 0) {
        ConsolePrint("Buffer is empty\n");
        SpinLockRelease(&GDecon.Lock);
        return;
    }
    
    // Печатаем построчно с номерами
    UINT32 LineNum = 0;
    UINT32 Start = 0;
    
    for (UINT32 I = 0; I < GDecon.Used; I++) {
        if (GDecon.Buffer[I] == '\n' || I == GDecon.Used - 1) {
            UINT32 End = (GDecon.Buffer[I] == '\n') ? I : I + 1;
            UINT32 Len = End - Start;
            
            if (Len > 0) {
                ConsolePrint("%4u | ", LineNum++);
                
                // Выводим строку (без \n)
                for (UINT32 J = Start; J < End; J++) {
                    if (GDecon.Buffer[J] == '\r' || GDecon.Buffer[J] == '\n') {
                        // Пропускаем управляющие символы при выводе
                    } else {
                        ConsolePrint("%c", GDecon.Buffer[J]);
                    }
                }
                ConsolePrint("\n");
            }
            
            Start = I + 1;
        }
    }
    
    SpinLockRelease(&GDecon.Lock);
}

UINT32 DeconGetSize(NOPTR) {
    if (!GDecon.Initialized) {
        return 0;
    }
    return GDecon.Used;
}

const CHAR* DeconGetBuffer(NOPTR) {
    if (!GDecon.Initialized) {
        return NULLPTR;
    }
    return GDecon.Buffer;
}