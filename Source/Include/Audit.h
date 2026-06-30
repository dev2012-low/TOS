#pragma once

#include <Kernel/Types.h>
#include <Kernel/UserAccount.h>

// Уровни опасности
typedef enum {
    AUDIT_LEVEL_INFO = 0,
    AUDIT_LEVEL_WARNING = 1,
    AUDIT_LEVEL_DANGER = 2,
    AUDIT_LEVEL_CRITICAL = 3
} AuditLevel;

// Типы событий
typedef enum {
    AUDIT_EVENT_LOGIN = 0,
    AUDIT_EVENT_LOGOUT,
    AUDIT_EVENT_COMMAND,
    AUDIT_EVENT_FILE_ACCESS,
    AUDIT_EVENT_FILE_DELETE,
    AUDIT_EVENT_FILE_MODIFY,
    AUDIT_EVENT_FORMAT,
    AUDIT_EVENT_MOUNT,
    AUDIT_EVENT_REBOOT,
    AUDIT_EVENT_SHUTDOWN,
    AUDIT_EVENT_TASK_KILL,
    AUDIT_EVENT_USER_ADD,
    AUDIT_EVENT_USER_DELETE,
    AUDIT_EVENT_USER_CHANGE_PASS,
    AUDIT_EVENT_DRIVE_RENAME,
    AUDIT_EVENT_NETWORK_CONNECT,
    AUDIT_EVENT_EXECUTE,
    AUDIT_EVENT_LOGIN_FAILED
} AuditEventType;

// Структура записи аудита
typedef struct {
    UINT64      Timestamp;
    AuditLevel  Level;
    AuditEventType Type;
    UINT32      Uid;
    UINT32      Pid;
    CHAR        Username[USER_NAME_MAX];
    CHAR        Message[256];
    UINT8       Hash[32];
} AuditEntry;

// Инициализация аудита
NOPTR AuditInit(NOPTR);

// Запись события в аудит
NOPTR AuditLog(AuditLevel Level, AuditEventType Type, const CHAR *Format, ...);

// Показать аудит (только для Admin)
NOPTR AuditShow(NOPTR);

// Очистить аудит (только для Admin)
NOPTR AuditClear(NOPTR);

// Сохранить аудит в файл (только для Admin)
NOPTR AuditSave(const CHAR *Path);

// Получить статистику аудита
UINT32 AuditGetCount(NOPTR);
UINT32 AuditGetWarnings(NOPTR);
UINT32 AuditGetDanger(NOPTR);
UINT32 AuditGetCritical(NOPTR);

// Проверка, нужно ли логировать команду
BOOL AuditIsDangerousCommand(const CHAR *Cmd);

// Проверка целостности аудита
UINT32 AuditVerifyIntegrity(NOPTR);