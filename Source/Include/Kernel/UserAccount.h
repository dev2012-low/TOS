#pragma once

#include <Kernel/Types.h>
#include <Fs/Vfs.h>

#define USER_NAME_MAX        32
#define USER_PASS_MAX        64
#define USER_DB_PATH         "/etc/users.tdb"
#define USER_DB_MAGIC        0x544F5355u /* "TOSU" */
#define USER_DB_VERSION      1
#define USER_MAX_ACCOUNTS    32

/* ФСТЭК: усиленное хеширование паролей и защита от перебора */
#define USER_PASS_HASH_LEGACY       0u
#define USER_PASS_HASH_STRENGTHENED 1u
#define USER_PASS_ITERATIONS        4096u
#define USER_MAX_LOGIN_FAILS        5u
#define USER_LOCKOUT_SEC            300u

#define UID_ADMIN            0u
#define GID_ADMIN            0u
#define UID_USER_MIN         1000u
#define UID_NOBODY           65534u
#define GID_NOBODY           65534u

#define VFS_ACCESS_READ      1u
#define VFS_ACCESS_WRITE     2u

typedef enum {
    UserRoleGuest = 0,
    UserRoleUser  = 1,
    UserRoleAdmin = 2
} UserRole;

typedef struct {
    UINT32 Uid;
    UINT32 Gid;
    UserRole Role;
    BOOL Authenticated;
    CHAR Username[USER_NAME_MAX];
} TosSession;

typedef struct {
    UINT32 Uid;
    UINT32 Gid;
    UserRole Role;
    BOOL Authenticated;
} TosCredentials;

typedef struct {
    UINT32 Uid;
    UINT32 Gid;
    UserRole Role;
    BOOL Enabled;
    UINT8 HashVersion;
    UINT8 FailedAttempts;
    UINT16 Reserved;
    UINT64 LockUntilMs;
    CHAR Username[USER_NAME_MAX];
    UINT8 Salt[16];
    UINT8 PasswordHash[32];
} UserAccount;

NOPTR UserManagerInit(NOPTR);
NOPTR UserManagerRepairDefaults(NOPTR);
INT UserManagerTryAutoLoad(NOPTR);
INT UserManagerLoad(const CHAR *Path);
INT UserManagerSave(const CHAR *Path);

INT UserManagerLogin(const CHAR *Username, const CHAR *Password);
NOPTR UserManagerLogout(NOPTR);
BOOL UserManagerIsLoggedIn(NOPTR);
const TosSession *UserManagerGetSession(NOPTR);
UserRole UserManagerGetRole(NOPTR);
BOOL UserManagerIsAdmin(NOPTR);

BOOL UserManagerHasRole(UserRole Required);
BOOL UserManagerCanAccessInode(VfsInode *Inode, UINT32 Access);
TosCredentials UserManagerGetActiveCredentials(NOPTR);
NOPTR UserManagerApplyToCurrentTask(NOPTR);

INT UserManagerAddUser(const CHAR *Username, const CHAR *Password, UserRole Role);
INT UserManagerRemoveUser(const CHAR *Username);
INT UserManagerSetPassword(const CHAR *Username, const CHAR *OldPass, const CHAR *NewPass);
INT UserManagerListUsers(NOPTR);
const UserAccount *UserManagerFindByName(const CHAR *Username);
const UserAccount *UserManagerFindByUid(UINT32 Uid);

static inline const CHAR *UserRoleToString(UserRole Role) {
    switch (Role) {
        case UserRoleAdmin: return "Admin";
        case UserRoleUser:  return "User";
        default:            return "Guest";
    }
}
