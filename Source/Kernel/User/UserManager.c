#include <Kernel/UserAccount.h>
#include <Kernel/Task.h>
#include <Kernel/Scheduler.h>
#include <Kernel/SpinLock.h>
#include <Kernel/Return.h>
#include <Crypto/Sha256.h>
#include <Crypto/Rng.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Memory/Allocator.h>
#include <Console.h>
#include <Time/Timer.h>
#include <Audit.h>

typedef struct ATTRIBUTE(packed) {
    UINT32 Magic;
    UINT32 Version;
    UINT32 Count;
    UINT32 Reserved;
} UserDbHeader;

typedef struct ATTRIBUTE(packed) {
    UINT32 Uid;
    UINT32 Gid;
    UINT8 Role;
    UINT8 Enabled;
    UINT8 Reserved[2];
    CHAR Username[USER_NAME_MAX];
    UINT8 Salt[16];
    UINT8 PasswordHash[32];
} UserDbRecord;

static UserAccount GAccounts[USER_MAX_ACCOUNTS];
static UINT32 GAccountCount = 0;
static TosSession GSession;
static SpinLock GUserLock;
CHAR GDbPath[PATH_MAX];
static BOOL GUserInitialized = FALSE;

static NOPTR UserManagerHashPassword(const UINT8 *Salt, const CHAR *Password,
                                     UINT8 HashVersion, UINT8 *OutHash) {
    UINT8 Buffer[16 + USER_PASS_MAX];
    UINT8 Temp[32];
    UINT32 PassLen;
    UINT32 I;
    UINT32 Iter;

    PassLen = (UINT32)StrLen(Password);
    if (PassLen > USER_PASS_MAX) {
        PassLen = USER_PASS_MAX;
    }

    for (I = 0; I < 16; I++) {
        Buffer[I] = Salt[I];
    }
    for (I = 0; I < PassLen; I++) {
        Buffer[16 + I] = (UINT8)Password[I];
    }

    Sha256Hash(Buffer, 16 + PassLen, OutHash);

    if (HashVersion < USER_PASS_HASH_STRENGTHENED) {
        return;
    }

    for (Iter = 1; Iter < USER_PASS_ITERATIONS; Iter++) {
        MemCpy(Temp, OutHash, 32);
        for (I = 0; I < 16; I++) {
            Buffer[I] = Salt[I];
        }
        MemCpy(Buffer + 16, Temp, 32);
        Sha256Hash(Buffer, 16 + 32, OutHash);
    }
}

static BOOL UserManagerVerifyPassword(UserAccount *Account, const CHAR *Password) {
    UINT8 Hash[32];

    if (!Account || !Password) {
        return FALSE;
    }

    UserManagerHashPassword(Account->Salt, Password, Account->HashVersion, Hash);
    if (SecureMemCmp(Hash, Account->PasswordHash, 32) != 0) {
        return FALSE;
    }

    if (Account->HashVersion < USER_PASS_HASH_STRENGTHENED) {
        Account->HashVersion = USER_PASS_HASH_STRENGTHENED;
        UserManagerHashPassword(Account->Salt, Password, Account->HashVersion,
                                Account->PasswordHash);
    }

    return TRUE;
}

static NOPTR UserManagerSetPasswordInternal(UserAccount *Account, const CHAR *Password) {
    if (!Account || !Password) {
        return;
    }

    RngGetRandomBytes(Account->Salt, sizeof(Account->Salt));
    Account->HashVersion = USER_PASS_HASH_STRENGTHENED;
    UserManagerHashPassword(Account->Salt, Password, Account->HashVersion,
                            Account->PasswordHash);
}

static INT UserManagerFindIndex(const CHAR *Username) {
    UINT32 I;

    if (!Username) {
        return -1;
    }

    for (I = 0; I < GAccountCount; I++) {
        if (StrCaseCmp(GAccounts[I].Username, Username) == 0) {
            return (INT)I;
        }
    }
    return -1;
}

static UINT32 UserManagerAllocUid(UserRole Role) {
    UINT32 I;
    UINT32 MaxUid = (Role == UserRoleAdmin) ? UID_USER_MIN - 1 : UID_NOBODY - 1;
    UINT32 Start = (Role == UserRoleAdmin) ? UID_ADMIN : UID_USER_MIN;
    UINT32 Candidate;
    BOOL Used;

    for (Candidate = Start; Candidate <= MaxUid; Candidate++) {
        Used = FALSE;
        for (I = 0; I < GAccountCount; I++) {
            if (GAccounts[I].Uid == Candidate) {
                Used = TRUE;
                break;
            }
        }
        if (!Used) {
            return Candidate;
        }
    }

    for (Candidate = UID_USER_MIN; Candidate < UID_NOBODY; Candidate++) {
        Used = FALSE;
        for (I = 0; I < GAccountCount; I++) {
            if (GAccounts[I].Uid == Candidate) {
                Used = TRUE;
                break;
            }
        }
        if (!Used) {
            return Candidate;
        }
    }

    return UID_NOBODY;
}

static NOPTR UserManagerClearSession(NOPTR) {
    MemSet(&GSession, 0, sizeof(GSession));
    GSession.Role = UserRoleGuest;
}

static NOPTR UserManagerEnsureDefaults(NOPTR) {
    UserAccount *Admin;
    UserAccount *Guest;

    if (GAccountCount > 0) {
        return;
    }

    Admin = &GAccounts[GAccountCount++];
    MemSet(Admin, 0, sizeof(*Admin));
    Admin->Uid = UID_ADMIN;
    Admin->Gid = GID_ADMIN;
    Admin->Role = UserRoleAdmin;
    Admin->Enabled = TRUE;
    StrnCpy(Admin->Username, "admin", USER_NAME_MAX - 1);
    UserManagerSetPasswordInternal(Admin, "admin");

    Guest = &GAccounts[GAccountCount++];
    MemSet(Guest, 0, sizeof(*Guest));
    Guest->Uid = UID_NOBODY;
    Guest->Gid = GID_NOBODY;
    Guest->Role = UserRoleGuest;
    Guest->Enabled = TRUE;
    StrnCpy(Guest->Username, "Guest", USER_NAME_MAX - 1);
    UserManagerSetPasswordInternal(Guest, "");
}

NOPTR UserManagerInit(NOPTR) {
    if (GUserInitialized) {
        return;
    }

    SpinLockInit(&GUserLock);
    MemSet(GAccounts, 0, sizeof(GAccounts));
    MemSet(GDbPath, 0, sizeof(GDbPath));
    GAccountCount = 0;
    UserManagerClearSession();
    UserManagerEnsureDefaults();
    GUserInitialized = TRUE;
}

NOPTR UserManagerRepairDefaults(NOPTR) {
    SpinLockAcquire(&GUserLock);
    UserManagerEnsureDefaults();
    SpinLockRelease(&GUserLock);
}

static INT UserManagerLoadBuffer(const UINT8 *Data, UINT32 Size) {
    UserDbHeader *Header;
    UserDbRecord *Rec;
    UINT32 I;
    UserAccount *Acc;

    if (!Data || Size < sizeof(UserDbHeader)) {
        return INCORRECT_VALUE;
    }

    Header = (UserDbHeader*)Data;
    if (Header->Magic != USER_DB_MAGIC || Header->Version != USER_DB_VERSION) {
        return INCORRECT_VALUE;
    }
    if (Header->Count > USER_MAX_ACCOUNTS) {
        return INCORRECT_VALUE;
    }
    if (Size < sizeof(UserDbHeader) + Header->Count * sizeof(UserDbRecord)) {
        return INCORRECT_VALUE;
    }

    SpinLockAcquire(&GUserLock);
    GAccountCount = 0;
    Rec = (UserDbRecord*)(Data + sizeof(UserDbHeader));

    for (I = 0; I < Header->Count; I++) {
        Acc = &GAccounts[GAccountCount++];
        Acc->Uid = Rec[I].Uid;
        Acc->Gid = Rec[I].Gid;
        Acc->Role = (UserRole)Rec[I].Role;
        Acc->Enabled = Rec[I].Enabled ? TRUE : FALSE;
        StrnCpy(Acc->Username, Rec[I].Username, USER_NAME_MAX - 1);
        Acc->HashVersion = Rec[I].Reserved[0];
        MemCpy(Acc->Salt, Rec[I].Salt, 16);
        MemCpy(Acc->PasswordHash, Rec[I].PasswordHash, 32);
    }

    UserManagerClearSession();
    SpinLockRelease(&GUserLock);

    if (GAccountCount == 0) {
        UserManagerEnsureDefaults();
    }

    return SUCCESS;
}

INT UserManagerLoad(const CHAR *Path) {
    VfsFile *File;
    UINT8 *Data;
    UINT64 Size;
    UINT32 Read;
    INT Ret;

    if (!Path || Path[0] == '\0') {
        return INCORRECT_VALUE;
    }

    if (VfsOpen(CurrentDir, Path, O_READ, &File) != SUCCESS) {
        return NOT_FOUND;
    }

    Size = File->FInode->ISize;
    if (Size == 0 || Size > 256 * 1024) {
        VfsClose(File);
        return INCORRECT_VALUE;
    }

    Data = (UINT8*)MemoryAllocate((USIZE)Size);
    if (!Data) {
        VfsClose(File);
        return NO_MEMORY;
    }

    if (VfsRead(File, Data, (UINT32)Size, &Read) != SUCCESS || Read != (UINT32)Size) {
        MemoryFree(Data);
        VfsClose(File);
        return IO_ERROR;
    }
    VfsClose(File);

    Ret = UserManagerLoadBuffer(Data, (UINT32)Size);
    MemoryFree(Data);

    if (Ret == SUCCESS) {
        StrnCpy(GDbPath, Path, PATH_MAX - 1);
    }

    return Ret;
}

INT UserManagerTryAutoLoad(NOPTR) {
    MountEntry Entries[16];
    CHAR Path[PATH_MAX];
    INT Count;
    INT I;

    if (GDbPath[0] != '\0') {
        if (UserManagerLoad(GDbPath) == SUCCESS) {
            return SUCCESS;
        }
    }

    Count = VfsGetMountPoints("", Entries, 16);
    for (I = 0; I < Count; I++) {
        SnPrintf(Path, sizeof(Path), "%s::%s", Entries[I].Name, USER_DB_PATH);
        if (UserManagerLoad(Path) == SUCCESS) {
            return SUCCESS;
        }
    }

    return NOT_FOUND;
}

INT UserManagerSave(const CHAR *Path) {
    UserDbHeader Header;
    UserDbRecord *Records;
    VfsFile *File;
    UINT32 TotalSize;
    UINT32 Written;
    UINT32 I;
    INT Ret = SUCCESS;
    const CHAR *SavePath = Path;

    if (!SavePath || SavePath[0] == '\0') {
        SavePath = GDbPath;
    }
    if (!SavePath || SavePath[0] == '\0') {
        return INCORRECT_VALUE;
    }

    TotalSize = (UINT32)(sizeof(UserDbHeader) + GAccountCount * sizeof(UserDbRecord));
    Records = (UserDbRecord*)MemoryAllocate(TotalSize);
    if (!Records) {
        return NO_MEMORY;
    }

    SpinLockAcquire(&GUserLock);
    MemSet(&Header, 0, sizeof(Header));
    Header.Magic = USER_DB_MAGIC;
    Header.Version = USER_DB_VERSION;
    Header.Count = GAccountCount;

    for (I = 0; I < GAccountCount; I++) {
        Records[I].Uid = GAccounts[I].Uid;
        Records[I].Gid = GAccounts[I].Gid;
        Records[I].Role = (UINT8)GAccounts[I].Role;
        Records[I].Enabled = GAccounts[I].Enabled ? 1 : 0;
        StrnCpy(Records[I].Username, GAccounts[I].Username, USER_NAME_MAX - 1);
        Records[I].Reserved[0] = GAccounts[I].HashVersion;
        MemCpy(Records[I].Salt, GAccounts[I].Salt, 16);
        MemCpy(Records[I].PasswordHash, GAccounts[I].PasswordHash, 32);
    }
    SpinLockRelease(&GUserLock);

    if (VfsOpen(CurrentDir, SavePath, O_WRITE | O_CREAT | O_TRUNC, &File) != SUCCESS) {
        MemoryFree(Records);
        return IO_ERROR;
    }

    if (VfsWrite(File, &Header, sizeof(Header), &Written) != SUCCESS ||
        Written != sizeof(Header)) {
        Ret = IO_ERROR;
    } else if (GAccountCount > 0) {
        if (VfsWrite(File, Records, GAccountCount * sizeof(UserDbRecord), &Written) != SUCCESS ||
            Written != GAccountCount * sizeof(UserDbRecord)) {
            Ret = IO_ERROR;
        }
    }

    VfsClose(File);
    MemoryFree(Records);

    if (Ret == SUCCESS) {
        StrnCpy(GDbPath, SavePath, PATH_MAX - 1);
    }

    return Ret;
}

INT UserManagerLogin(const CHAR *Username, const CHAR *Password) {
    INT Index;
    UserAccount *Account;
    UINT64 NowMs;

    if (!Username || Username[0] == '\0') {
        return INCORRECT_VALUE;
    }

    BOOL IsGuest = (StrCaseCmp(Username, "Guest") == 0);
    if (!IsGuest && (!Password || Password[0] == '\0')) {
        return INCORRECT_VALUE;
    }

    SpinLockAcquire(&GUserLock);
    Index = UserManagerFindIndex(Username);
    if (Index < 0) {
        SpinLockRelease(&GUserLock);
        AuditLog(AUDIT_LEVEL_WARNING, AUDIT_EVENT_LOGIN_FAILED,
                 "Unknown user '%s'", Username);
        return NOT_FOUND;
    }

    Account = &GAccounts[Index];
    if (!Account->Enabled) {
        SpinLockRelease(&GUserLock);
        AuditLog(AUDIT_LEVEL_WARNING, AUDIT_EVENT_LOGIN_FAILED,
                 "Disabled account '%s'", Username);
        return PERMISSION_DENIED;
    }

    if (IsGuest) {
        GSession.Uid = Account->Uid;
        GSession.Gid = Account->Gid;
        GSession.Role = Account->Role;
        GSession.Authenticated = TRUE;
        StrnCpy(GSession.Username, Account->Username, USER_NAME_MAX - 1);
        SpinLockRelease(&GUserLock);
        UserManagerApplyToCurrentTask();
        return SUCCESS;
    }

    NowMs = TimerApicMs();
    if (Account->LockUntilMs > NowMs) {
        SpinLockRelease(&GUserLock);
        AuditLog(AUDIT_LEVEL_DANGER, AUDIT_EVENT_LOGIN_FAILED,
                 "Locked account '%s'", Username);
        return PERMISSION_DENIED;
    }

    if (!UserManagerVerifyPassword(Account, Password)) {
        Account->FailedAttempts++;
        if (Account->FailedAttempts >= USER_MAX_LOGIN_FAILS) {
            Account->LockUntilMs = NowMs + (UINT64)USER_LOCKOUT_SEC * 1000u;
            Account->FailedAttempts = 0;
            AuditLog(AUDIT_LEVEL_DANGER, AUDIT_EVENT_LOGIN_FAILED,
                     "Account '%s' locked after %u failed attempts",
                     Username, USER_MAX_LOGIN_FAILS);
        } else {
            AuditLog(AUDIT_LEVEL_WARNING, AUDIT_EVENT_LOGIN_FAILED,
                     "Bad password for '%s' (attempt %u/%u)",
                     Username, Account->FailedAttempts, USER_MAX_LOGIN_FAILS);
        }
        SpinLockRelease(&GUserLock);
        return CHECK_ERROR;
    }

    Account->FailedAttempts = 0;
    Account->LockUntilMs = 0;

    GSession.Uid = Account->Uid;
    GSession.Gid = Account->Gid;
    GSession.Role = Account->Role;
    GSession.Authenticated = TRUE;
    StrnCpy(GSession.Username, Account->Username, USER_NAME_MAX - 1);
    SpinLockRelease(&GUserLock);

    UserManagerApplyToCurrentTask();
    return SUCCESS;
}

NOPTR UserManagerLogout(NOPTR) {
    SpinLockAcquire(&GUserLock);
    UserManagerClearSession();
    SpinLockRelease(&GUserLock);
    UserManagerApplyToCurrentTask();
}

BOOL UserManagerIsLoggedIn(NOPTR) {
    BOOL LoggedIn;

    SpinLockAcquire(&GUserLock);
    LoggedIn = GSession.Authenticated;
    SpinLockRelease(&GUserLock);
    return LoggedIn;
}

const TosSession *UserManagerGetSession(NOPTR) {
    return &GSession;
}

UserRole UserManagerGetRole(NOPTR) {
    return GSession.Role;
}

BOOL UserManagerIsAdmin(NOPTR) {
    return GSession.Authenticated && GSession.Role == UserRoleAdmin;
}

BOOL UserManagerHasRole(UserRole Required) {
    if (!GSession.Authenticated) {
        return Required == UserRoleGuest;
    }
    if (GSession.Role == UserRoleAdmin) {
        return TRUE;
    }
    if (Required == UserRoleGuest) {
        return TRUE;
    }
    if (Required == UserRoleUser) {
        return GSession.Role == UserRoleUser;
    }
    return FALSE;
}

TosCredentials UserManagerGetActiveCredentials(NOPTR) {
    TosCredentials Creds;
    KTask *Task;

    MemSet(&Creds, 0, sizeof(Creds));
    Creds.Role = UserRoleGuest;
    Creds.Uid = UID_NOBODY;
    Creds.Gid = GID_NOBODY;

    if (GSession.Authenticated) {
        Creds.Uid = GSession.Uid;
        Creds.Gid = GSession.Gid;
        Creds.Role = GSession.Role;
        Creds.Authenticated = TRUE;
        return Creds;
    }

    Task = SchedulerGetCurrent();
    if (Task && Task->Authenticated) {
        Creds.Uid = Task->Euid;
        Creds.Gid = Task->Egid;
        Creds.Role = Task->Role;
        Creds.Authenticated = TRUE;
    }

    return Creds;
}

BOOL UserManagerCanAccessInode(VfsInode *Inode, UINT32 Access) {
    TosCredentials Creds;

    if (!Inode) {
        return FALSE;
    }

    Creds = UserManagerGetActiveCredentials();
    if (!Creds.Authenticated) {
        return FALSE;
    }
    if (Creds.Role == UserRoleAdmin) {
        return TRUE;
    }

    if (Inode->IUid == Creds.Uid || Inode->IUid == 0) {
        return TRUE;
    }

    if (Access == VFS_ACCESS_READ) {
        return Creds.Role >= UserRoleUser;
    }

    return FALSE;
}

NOPTR UserManagerApplyToCurrentTask(NOPTR) {
    KTask *Task = SchedulerGetCurrent();

    if (!Task) {
        return;
    }

    if (GSession.Authenticated) {
        Task->Uid = GSession.Uid;
        Task->Gid = GSession.Gid;
        Task->Euid = GSession.Uid;
        Task->Egid = GSession.Gid;
        Task->Role = GSession.Role;
        Task->Authenticated = TRUE;
    } else {
        Task->Uid = UID_NOBODY;
        Task->Gid = GID_NOBODY;
        Task->Euid = UID_NOBODY;
        Task->Egid = GID_NOBODY;
        Task->Role = UserRoleGuest;
        Task->Authenticated = FALSE;
    }
}

INT UserManagerAddUser(const CHAR *Username, const CHAR *Password, UserRole Role) {
    UserAccount *Acc;

    if (!Username || Username[0] == '\0') {
        return INCORRECT_VALUE;
    }
    
    // Guest может быть создан с пустым паролем
    if (Role != UserRoleGuest && (!Password || Password[0] == '\0')) {
        return INCORRECT_VALUE;
    }

    SpinLockAcquire(&GUserLock);
    if (GAccountCount >= USER_MAX_ACCOUNTS) {
        SpinLockRelease(&GUserLock);
        return NO_MEMORY;
    }
    if (UserManagerFindIndex(Username) >= 0) {
        SpinLockRelease(&GUserLock);
        return ALREADY_EXISTS;
    }

    Acc = &GAccounts[GAccountCount++];
    MemSet(Acc, 0, sizeof(*Acc));
    StrnCpy(Acc->Username, Username, USER_NAME_MAX - 1);
    Acc->Role = Role;
    Acc->Enabled = TRUE;
    Acc->Uid = UserManagerAllocUid(Role);
    Acc->Gid = Acc->Uid;
    
    // Guest не должен иметь пароль
    if (Role == UserRoleGuest) {
        MemSet(Acc->Salt, 0, 16);
        MemSet(Acc->PasswordHash, 0, 32);
    } else {
        UserManagerSetPasswordInternal(Acc, Password);
    }
    
    SpinLockRelease(&GUserLock);

    if (GDbPath[0] != '\0') {
        UserManagerSave(GDbPath);
    }

    return SUCCESS;
}

INT UserManagerRemoveUser(const CHAR *Username) {
    INT Index;
    UINT32 I;

    if (!Username || Username[0] == '\0') {
        return INCORRECT_VALUE;
    }
    if (StrCaseCmp(Username, "admin") == 0) {
        return PERMISSION_DENIED;
    }

    SpinLockAcquire(&GUserLock);
    Index = UserManagerFindIndex(Username);
    if (Index < 0) {
        SpinLockRelease(&GUserLock);
        return NOT_FOUND;
    }

    if (GSession.Authenticated && StrCmp(GSession.Username, Username) == 0) {
        SpinLockRelease(&GUserLock);
        return BUSY;
    }

    for (I = (UINT32)Index + 1; I < GAccountCount; I++) {
        GAccounts[I - 1] = GAccounts[I];
    }
    GAccountCount--;
    MemSet(&GAccounts[GAccountCount], 0, sizeof(GAccounts[0]));
    SpinLockRelease(&GUserLock);

    if (GDbPath[0] != '\0') {
        UserManagerSave(GDbPath);
    }

    return SUCCESS;
}

INT UserManagerSetPassword(const CHAR *Username, const CHAR *OldPass, const CHAR *NewPass) {
    INT Index;
    UserAccount *Acc;
    BOOL IsSelf;
    BOOL IsAdmin;

    if (!Username || !NewPass || NewPass[0] == '\0') {
        return INCORRECT_VALUE;
    }

    SpinLockAcquire(&GUserLock);
    Index = UserManagerFindIndex(Username);
    if (Index < 0) {
        SpinLockRelease(&GUserLock);
        return NOT_FOUND;
    }

    Acc = &GAccounts[Index];
    IsSelf = GSession.Authenticated && StrCmp(GSession.Username, Username) == 0;
    IsAdmin = GSession.Authenticated && GSession.Role == UserRoleAdmin;

    if (!IsAdmin && !IsSelf) {
        SpinLockRelease(&GUserLock);
        return PERMISSION_DENIED;
    }

    if (IsSelf && !IsAdmin) {
        if (!OldPass || !UserManagerVerifyPassword(Acc, OldPass)) {
            SpinLockRelease(&GUserLock);
            return CHECK_ERROR;
        }
    }

    UserManagerSetPasswordInternal(Acc, NewPass);
    SpinLockRelease(&GUserLock);

    if (GDbPath[0] != '\0') {
        UserManagerSave(GDbPath);
    }

    return SUCCESS;
}

INT UserManagerListUsers(NOPTR) {
    UINT32 I;

    ConsolePrint("\033[36mUID\tGID\tRole\tEnabled\tUsername\033[0m\n");
    for (I = 0; I < GAccountCount; I++) {
        ConsolePrint("%u\t%u\t%s\t%s\t%s\n",
                     GAccounts[I].Uid,
                     GAccounts[I].Gid,
                     UserRoleToString(GAccounts[I].Role),
                     GAccounts[I].Enabled ? "yes" : "no",
                     GAccounts[I].Username);
    }
    return SUCCESS;
}

const UserAccount *UserManagerFindByName(const CHAR *Username) {
    INT Index = UserManagerFindIndex(Username);
    if (Index < 0) {
        return NULLPTR;
    }
    return &GAccounts[Index];
}

const UserAccount *UserManagerFindByUid(UINT32 Uid) {
    UINT32 I;

    for (I = 0; I < GAccountCount; I++) {
        if (GAccounts[I].Uid == Uid) {
            return &GAccounts[I];
        }
    }
    return NULLPTR;
}
