#include <Audit.h>
#include <Lib/StdIo.h>
#include <Lib/String.h>
#include <Memory/Allocator.h>
#include <Time/Timer.h>
#include <Kernel/SpinLock.h>
#include <Kernel/Scheduler.h>
#include <Console.h>
#include <Fs/Vfs.h>
#include <Crypto/Sha256.h>

// ============================================================================
// Конфигурация аудита
// ============================================================================

#define AUDIT_MAX_ENTRIES      4096
#define AUDIT_MAGIC            0x41554454   // "AUDT"

// ============================================================================
// Структура состояния аудита
// ============================================================================

typedef struct {
    AuditEntry  Entries[AUDIT_MAX_ENTRIES];
    UINT32      Count;
    UINT32      Warnings;
    UINT32      Danger;
    UINT32      Critical;
    UINT32      Magic;
    UINT8       TotalHash[32];   // <-- SHA-256 всего буфера!
    SpinLock    Lock;
    BOOL        Initialized;
} AuditState;

static AuditState GAudit;

// ============================================================================
// Криптографические функции
// ============================================================================

// Вычисляем SHA-256 для одной записи (без поля Hash)
static NOPTR AuditHashEntry(const AuditEntry *Entry, UINT8 *OutHash) {
    UINT8 Buffer[512];
    USIZE Size = OffsetOf(AuditEntry, Hash);
    
    MemCpy(Buffer, Entry, Size);
    Sha256Hash(Buffer, Size, OutHash);
}

// Вычисляем SHA-256 для всего буфера
static NOPTR AuditHashAllEntries(UINT8 *OutHash) {
    Sha256Ctx Ctx;
    Sha256Init(&Ctx);
    
    for (UINT32 I = 0; I < GAudit.Count; I++) {
        // Хешируем каждую запись БЕЗ поля Hash
        UINT8 EntryHash[32];
        AuditHashEntry(&GAudit.Entries[I], EntryHash);
        Sha256Update(&Ctx, EntryHash, 32);
    }
    
    Sha256Final(&Ctx, OutHash);
}

// Проверяем целостность всего аудита
static UINT32 AuditVerifyIntegrityLocked(NOPTR) {
    UINT32 Corrupted = 0;
    
    if (!GAudit.Initialized) {
        return 0;
    }
    
    // Проверяем магическое число
    if (GAudit.Magic != AUDIT_MAGIC) {
        ConsolePrint("[Audit] WARNING: Audit buffer corrupted (bad magic)!\n");
        Corrupted++;
    }
    
    // Проверяем каждую запись
    for (UINT32 I = 0; I < GAudit.Count; I++) {
        AuditEntry *E = &GAudit.Entries[I];
        UINT8 Calculated[32];
        AuditHashEntry(E, Calculated);
        
        if (SecureMemCmp(Calculated, E->Hash, 32) != 0) {
            ConsolePrint("[Audit] WARNING: Entry %u is CORRUPTED (SHA-256 mismatch)!\n", I);
            Corrupted++;
        }
    }
    
    // Проверяем общий хеш (если есть записи)
    if (GAudit.Count > 0) {
        UINT8 Calculated[32];
        AuditHashAllEntries(Calculated);
        
        if (SecureMemCmp(Calculated, GAudit.TotalHash, 32) != 0) {
            ConsolePrint("[Audit] WARNING: Total hash mismatch!\n");
            Corrupted++;
        }
    }
    
    return Corrupted;
}

// Публичная функция
UINT32 AuditVerifyIntegrity(NOPTR) {
    UINT32 Corrupted = 0;
    
    SpinLockAcquire(&GAudit.Lock);
    Corrupted = AuditVerifyIntegrityLocked();
    SpinLockRelease(&GAudit.Lock);
    
    return Corrupted;
}

// ============================================================================
// Преобразование типов в строки
// ============================================================================

static const CHAR* AuditLevelString(AuditLevel Level) {
    switch (Level) {
        case AUDIT_LEVEL_INFO:     return "INFO";
        case AUDIT_LEVEL_WARNING:  return "WARN";
        case AUDIT_LEVEL_DANGER:   return "DANG";
        case AUDIT_LEVEL_CRITICAL: return "CRIT";
        default:                   return "UNKN";
    }
}

static const CHAR* AuditTypeString(AuditEventType Type) {
    switch (Type) {
        case AUDIT_EVENT_LOGIN:          return "LOGIN";
        case AUDIT_EVENT_LOGOUT:         return "LOGOUT";
        case AUDIT_EVENT_COMMAND:        return "COMMAND";
        case AUDIT_EVENT_FILE_ACCESS:    return "FILE_ACCESS";
        case AUDIT_EVENT_FILE_DELETE:    return "FILE_DELETE";
        case AUDIT_EVENT_FILE_MODIFY:    return "FILE_MODIFY";
        case AUDIT_EVENT_FORMAT:         return "FORMAT";
        case AUDIT_EVENT_MOUNT:          return "MOUNT";
        case AUDIT_EVENT_REBOOT:         return "REBOOT";
        case AUDIT_EVENT_SHUTDOWN:       return "SHUTDOWN";
        case AUDIT_EVENT_TASK_KILL:      return "TASK_KILL";
        case AUDIT_EVENT_USER_ADD:       return "USER_ADD";
        case AUDIT_EVENT_USER_DELETE:    return "USER_DELETE";
        case AUDIT_EVENT_USER_CHANGE_PASS: return "USER_CHANGE_PASS";
        case AUDIT_EVENT_DRIVE_RENAME:   return "DRIVE_RENAME";
        case AUDIT_EVENT_NETWORK_CONNECT: return "NET_CONNECT";
        case AUDIT_EVENT_EXECUTE:        return "EXECUTE";
        case AUDIT_EVENT_LOGIN_FAILED:   return "LOGIN_FAIL";
        default:                         return "UNKNOWN";
    }
}

// ============================================================================
// Внутренние функции
// ============================================================================

static NOPTR AuditAddEntry(AuditLevel Level, AuditEventType Type,
                            const CHAR *Username, UINT32 Uid, UINT32 Pid,
                            const CHAR *Message) {
    if (!GAudit.Initialized) {
        return;
    }
    
    SpinLockAcquire(&GAudit.Lock);
    
    if (GAudit.Count == 0 && GAudit.Magic != AUDIT_MAGIC) {
        GAudit.Magic = AUDIT_MAGIC;
    }
    
    if (GAudit.Count >= AUDIT_MAX_ENTRIES) {
        MemMove(&GAudit.Entries[0], &GAudit.Entries[1], 
                (AUDIT_MAX_ENTRIES - 1) * sizeof(AuditEntry));
        GAudit.Count--;
    }
    
    AuditEntry *Entry = &GAudit.Entries[GAudit.Count++];
    Entry->Timestamp = TimerTicks();
    Entry->Level = Level;
    Entry->Type = Type;
    Entry->Uid = Uid;
    Entry->Pid = Pid;
    
    if (Username) {
        StrnCpy(Entry->Username, Username, USER_NAME_MAX - 1);
    } else {
        StrCpy(Entry->Username, "unknown");
    }
    
    if (Message) {
        StrnCpy(Entry->Message, Message, sizeof(Entry->Message) - 1);
    } else {
        Entry->Message[0] = '\0';
    }
    
    // <-- ВЫЧИСЛЯЕМ SHA-256!
    AuditHashEntry(Entry, Entry->Hash);
    
    // Обновляем общий хеш
    AuditHashAllEntries(GAudit.TotalHash);
    
    // Считаем статистику
    switch (Level) {
        case AUDIT_LEVEL_WARNING:  GAudit.Warnings++; break;
        case AUDIT_LEVEL_DANGER:   GAudit.Danger++; break;
        case AUDIT_LEVEL_CRITICAL: GAudit.Critical++; break;
        default: break;
    }
    
    SpinLockRelease(&GAudit.Lock);
}

// ============================================================================
// Публичные API
// ============================================================================

NOPTR AuditInit(NOPTR) {
    if (GAudit.Initialized) {
        return;
    }
    
    MemSet(&GAudit, 0, sizeof(AuditState));
    SpinLockInit(&GAudit.Lock);
    GAudit.Magic = AUDIT_MAGIC;
    GAudit.Initialized = TRUE;
    
    AuditLog(AUDIT_LEVEL_INFO, AUDIT_EVENT_COMMAND, "System started");
}

NOPTR AuditLog(AuditLevel Level, AuditEventType Type, const CHAR *Format, ...) {
    if (!GAudit.Initialized) {
        return;
    }
    
    VA_LIST Args;
    CHAR Message[256];
    INT Len;
    
    VaStart(Args, Format);
    Len = VsnPrintf(Message, sizeof(Message), Format, Args);
    VaEnd(Args);
    
    if (Len <= 0) {
        return;
    }
    
    const TosSession *Session = UserManagerGetSession();
    UINT32 Uid = Session->Authenticated ? Session->Uid : UID_NOBODY;
    UINT32 Pid = SchedulerGetPid();
    const CHAR *Username = Session->Authenticated ? Session->Username : "guest";
    
    AuditAddEntry(Level, Type, Username, Uid, Pid, Message);
}

NOPTR AuditShow(NOPTR) {
    if (!GAudit.Initialized) {
        ConsolePrint("Not initialized!\n");
        return;
    }
    
    if (!UserManagerIsAdmin()) {
        ConsolePrint("[Audit] Permission denied. Admin required.\n");
        return;
    }
    
    SpinLockAcquire(&GAudit.Lock);
    
    UINT32 Corrupted = AuditVerifyIntegrityLocked();
    if (Corrupted > 0) {
        ConsolePrint("\n\033[31m*** WARNING: %u audit entries are CORRUPTED! ***\033[0m\n\n", Corrupted);
    }
    
    if (GAudit.Count == 0) {
        ConsolePrint("[Audit] No entries\n");
        SpinLockRelease(&GAudit.Lock);
        return;
    }
    
    ConsolePrint("\n");
    ConsolePrint("============================================================\n");
    ConsolePrint("                     AUDIT LOG (SHA-256)\n");
    ConsolePrint("============================================================\n");
    ConsolePrint("  # | Time | Level | Type        | User    | Message\n");
    ConsolePrint("----+------+-------+-------------+---------+----------------\n");
    
    for (UINT32 I = 0; I < GAudit.Count; I++) {
        AuditEntry *E = &GAudit.Entries[I];
        
        // Проверяем целостность
        UINT8 Calculated[32];
        AuditHashEntry(E, Calculated);
        BOOL IsCorrupted = (SecureMemCmp(Calculated, E->Hash, 32) != 0);
        
        UINT32 Sec = (UINT32)(E->Timestamp / TimerFreq());
        UINT32 Ms = (UINT32)((E->Timestamp % TimerFreq()) * 1000 / TimerFreq());
        
        if (IsCorrupted) {
            ConsolePrint(" %3u | %5u.%03u | \033[31mCORRUPT\033[0m | \033[31m%s\033[0m | \033[31m%s\033[0m | \033[31m%s\033[0m\n",
                         I, Sec, Ms,
                         AuditTypeString(E->Type),
                         E->Username[0] ? E->Username : "?",
                         E->Message);
        } else {
            ConsolePrint(" %3u | %5u.%03u | %s | %-11s | %-7s | %s\n",
                         I, Sec, Ms,
                         AuditLevelString(E->Level),
                         AuditTypeString(E->Type),
                         E->Username[0] ? E->Username : "?",
                         E->Message);
        }
    }
    
    ConsolePrint("----+------+-------+-------------+---------+----------------\n");
    ConsolePrint("Total: %u entries", GAudit.Count);
    if (GAudit.Warnings > 0) {
        ConsolePrint(" | Warnings: %u", GAudit.Warnings);
    }
    if (GAudit.Danger > 0) {
        ConsolePrint(" | Danger: %u", GAudit.Danger);
    }
    if (GAudit.Critical > 0) {
        ConsolePrint(" | CRITICAL: %u", GAudit.Critical);
    }
    if (Corrupted > 0) {
        ConsolePrint(" | \033[31mCORRUPTED: %u\033[0m", Corrupted);
    }
    ConsolePrint("\n");
    ConsolePrint("============================================================\n");
    
    SpinLockRelease(&GAudit.Lock);
}

NOPTR AuditClear(NOPTR) {
    if (!GAudit.Initialized) {
        return;
    }
    
    if (!UserManagerIsAdmin()) {
        ConsolePrint("Permission denied. Admin required.\n");
        return;
    }
    
    SpinLockAcquire(&GAudit.Lock);
    MemSet(&GAudit.Entries, 0, sizeof(GAudit.Entries));
    GAudit.Count = 0;
    GAudit.Warnings = 0;
    GAudit.Danger = 0;
    GAudit.Critical = 0;
    MemSet(GAudit.TotalHash, 0, 32);
    SpinLockRelease(&GAudit.Lock);
    
    AuditLog(AUDIT_LEVEL_INFO, AUDIT_EVENT_COMMAND, "Audit log cleared");
    ConsolePrint("Cleared\n");
}

NOPTR AuditSave(const CHAR *Path) {
    if (!GAudit.Initialized) {
        return;
    }
    
    if (!UserManagerIsAdmin()) {
        ConsolePrint("[Audit] Permission denied. Admin required.\n");
        return;
    }
    
    if (!Path || Path[0] == '\0') {
        ConsolePrint("Usage: Audit --Save <path>\n");
        return;
    }
    
    UINT32 Corrupted = AuditVerifyIntegrity();
    if (Corrupted > 0) {
        ConsolePrint("[Audit] WARNING: %u corrupted entries will be saved as-is!\n", Corrupted);
    }
    
    VfsFile *File;
    if (VfsOpen(CurrentDir, Path, O_WRITE | O_CREAT | O_TRUNC, &File) != 0) {
        ConsolePrint("Failed to open '%s'\n", Path);
        return;
    }
    
    CHAR Line[512];
    UINT32 Written;
    
    SpinLockAcquire(&GAudit.Lock);
    
    // Сохраняем хеш как первую строку
    CHAR HashStr[65];
    for (INT I = 0; I < 32; I++) {
        SnPrintf(HashStr + I * 2, 3, "%02x", GAudit.TotalHash[I]);
    }
    SnPrintf(Line, sizeof(Line), "#HASH=%s\n", HashStr);
    VfsWrite(File, Line, (UINT32)StrLen(Line), &Written);
    
    for (UINT32 I = 0; I < GAudit.Count; I++) {
        AuditEntry *E = &GAudit.Entries[I];
        UINT32 Sec = (UINT32)(E->Timestamp / TimerFreq());
        UINT32 Ms = (UINT32)((E->Timestamp % TimerFreq()) * 1000 / TimerFreq());
        
        SnPrintf(Line, sizeof(Line), 
                 "[%u.%03u] %s %s %s: %s\n",
                 Sec, Ms,
                 AuditLevelString(E->Level),
                 AuditTypeString(E->Type),
                 E->Username,
                 E->Message);
        
        VfsWrite(File, Line, (UINT32)StrLen(Line), &Written);
    }
    
    SpinLockRelease(&GAudit.Lock);
    VfsClose(File);
    
    ConsolePrint("Saved %u entries to '%s'\n", GAudit.Count, Path);
}

UINT32 AuditGetCount(NOPTR) {
    return GAudit.Count;
}

UINT32 AuditGetWarnings(NOPTR) {
    return GAudit.Warnings;
}

UINT32 AuditGetDanger(NOPTR) {
    return GAudit.Danger;
}

UINT32 AuditGetCritical(NOPTR) {
    return GAudit.Critical;
}

BOOL AuditIsDangerousCommand(const CHAR *Cmd) {
    static const CHAR *DangerousCommands[] = {
        "Format", "Reboot", "Shutdown", "Remove", "RemoveDir",
        "Task --Kill", "User --Add", "User --Remove",
        "User --Passwd", "Drive --Rename", "Execute", "Run",
        "Mount", "Decon --Clear", "Audit --Clear",
        NULLPTR
    };
    
    for (INT I = 0; DangerousCommands[I]; I++) {
        if (StrStr(Cmd, DangerousCommands[I]) != NULLPTR) {
            return TRUE;
        }
    }
    
    return FALSE;
}