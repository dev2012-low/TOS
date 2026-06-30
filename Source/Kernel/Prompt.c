#include <Kernel/Types.h>
#include <Asm/Cpu.h>
#include <Lib/String.h>
#include <Console.h>
#include <Acpi.h>
#include <Kernel/Scheduler.h>
#include <Memory/Allocator.h>
#include <Drive/Drive.h>
#include <Kernel/Return.h>
#include <Fs/Vfs.h>
#include <Fs/Exfat.h>
#include <Fs/Fat32.h>
#include <Lib/StdIo.h>
#include <Gui/Off.h>
#include <Elf.h>
#include <Kernel/TosAsm.h>
#include <Kernel/UserAccount.h>
#include <Network/ICmp.h>
#include <Decon.h>
#include <Audit.h>
#include <Kernel/Integrity.h>
#include <Games/Snake.h>
#include <Games/Breakout.h>

#define REDIR_BUFFER_SIZE 4096

static CHAR GRedirectBuffer[REDIR_BUFFER_SIZE];
static UINT32 GRedirectBufferPos = 0;
static VfsFile *GRedirectFile = NULLPTR;

static INT LastCommandResult = 0;


EXTERN(CHAR, GDbPath[PATH_MAX]);

typedef enum {
    REDIR_NONE,
    REDIR_TRUNCATE,   // >
    REDIR_APPEND      // >>
} RedirType;

static NOPTR FlushRedirectBuffer(NOPTR) {
    if (GRedirectBufferPos == 0 || !GRedirectFile) return;
    
    UINT32 Written;
    VfsWrite(GRedirectFile, GRedirectBuffer, GRedirectBufferPos, &Written);
    GRedirectBufferPos = 0;
}

static CHAR* ParseRedirection(CHAR *CmdLine, RedirType *Redir, CHAR **OutFile) {
    CHAR *Pos = CmdLine;
    CHAR *RedirPos = NULLPTR;
    RedirType Type = REDIR_NONE;
    
    // Ищем > или >>
    while (*Pos) {
        if (*Pos == '>') {
            if (*(Pos + 1) == '>') {
                Type = REDIR_APPEND;
                RedirPos = Pos;
                *Pos = '\0';           // Обрезаем команду
                *(Pos + 1) = '\0';     // Обрезаем второй >
                break;
            } else {
                Type = REDIR_TRUNCATE;
                RedirPos = Pos;
                *Pos = '\0';
                break;
            }
        }
        Pos++;
    }
    
    if (!RedirPos) {
        *Redir = REDIR_NONE;
        return CmdLine;
    }
    
    // Ищем имя файла после >>
    CHAR *FileName = RedirPos + (Type == REDIR_APPEND ? 2 : 1);
    while (*FileName == ' ') FileName++;
    
    if (*FileName == '\0') {
        ConsolePrint("Syntax error: missing file name after redirection\n");
        *Redir = REDIR_NONE;
        return CmdLine;
    }
    
    // Убираем пробел в конце имени файла
    CHAR *End = FileName;
    while (*End && *End != ' ' && *End != '\t' && *End != '\n') End++;
    *End = '\0';
    
    *Redir = Type;
    *OutFile = FileName;
    return CmdLine;
}

// Разбиваем команду по &&
static INT ParseAndChain(const CHAR *Input, CHAR *Commands[], INT MaxCommands) {
    CHAR Buffer[1024];
    CHAR *SavePtr;
    CHAR *Token;
    INT Count = 0;

    StrCpy(Buffer, Input);
    Token = StrTokR(Buffer, "&", &SavePtr);

    while (Token && Count < MaxCommands) {
        // Убираем пробелы в начале
        while (*Token == ' ') Token++;

        // Убираем пробелы в конце
        CHAR *End = Token + StrLen(Token) - 1;
        while (End > Token && *End == ' ') {
            *End = '\0';
            End--;
        }

        if (StrLen(Token) > 0) {
            Commands[Count++] = Token;
        }

        Token = StrTokR(NULLPTR, "&", &SavePtr);
    }

    return Count;
}

static NOPTR HandleSayWithRedirection(CHAR *Args, RedirType Redir, const CHAR *OutFile) {
    CHAR Output[512];
    UINT32 Len;

    // <-- ЕСЛИ НЕТ ПЕРЕНАПРАВЛЕНИЯ — ВЫВОДИМ В КОНСОЛЬ
    if (Redir == REDIR_NONE || OutFile == NULLPTR || OutFile[0] == '\0') {
        if (!Args || Args[0] == '\0') {
            ConsolePrint("\n");
        } else {
            ConsolePrint("%s\n", Args);
        }
        LastCommandResult = 0;
        return;
    }

    // Формируем вывод
    if (!Args || Args[0] == '\0') {
        Output[0] = '\n';
        Output[1] = '\0';
        Len = 1;
    } else {
        Len = (UINT32)SnPrintf(Output, sizeof(Output), "%s\n", Args);
    }
    
    // Если это первый вызов в цепочке — открываем файл
    if (GRedirectFile == NULLPTR) {
        UINT32 Flags = O_WRITE | O_CREAT;
        if (Redir == REDIR_TRUNCATE) {
            Flags |= O_TRUNC;
        } else {
            Flags |= O_APPEND;
        }
        
        if (VfsOpen(CurrentDir, OutFile, Flags, &GRedirectFile) != 0) {
            ConsolePrint("Say: cannot open '%s' for writing\n", OutFile);
            LastCommandResult = -1;
            return;
        }
    }
    
    // Добавляем в буфер
    UINT32 Remaining = REDIR_BUFFER_SIZE - GRedirectBufferPos;
    if (Len >= Remaining) {
        FlushRedirectBuffer();
        Remaining = REDIR_BUFFER_SIZE;
    }
    
    if (Len < Remaining) {
        MemCpy(GRedirectBuffer + GRedirectBufferPos, Output, Len);
        GRedirectBufferPos += Len;
    } else {
        UINT32 Written;
        VfsWrite(GRedirectFile, Output, Len, &Written);
    }
    
    LastCommandResult = 0;
}

static NOPTR CloseRedirectFile(NOPTR) {
    if (GRedirectFile) {
        FlushRedirectBuffer();
        VfsClose(GRedirectFile);
        GRedirectFile = NULLPTR;
    }
}

volatile INT Init64Ready = 0;

extern VfsInode *RootInode;

static BOOL ShellRequireLogin(NOPTR) {
    if (UserManagerIsLoggedIn()) {
        return TRUE;
    }
    ConsolePrint("\033[31mLogin required.\033[0m Use: Login <username> <password>\n");
    return FALSE;
}

static BOOL ShellRequireAdmin(NOPTR) {
    if (!ShellRequireLogin()) {
        return FALSE;
    }
    if (UserManagerIsAdmin()) {
        return TRUE;
    }
    ConsolePrint("\033[31mPermission denied.\033[0m Admin role required.\n");
    return FALSE;
}

static NOPTR ShellInteractiveLogin(NOPTR) {
    CHAR Username[USER_NAME_MAX];
    CHAR Password[USER_PASS_MAX];
    INT Ret;
    INT InputResult;

    ConsolePrint("\n\033[36mThunder OS Login\033[0m\n");
    ConsolePrint("Default: \033[32madmin\033[0m / \033[32madmin\033[0m  (Ctrl+C = Guest)\n\n");

    for (;;) {
        ConsolePrint("Username: ");
        ConsoleSetEcho(TRUE);
        InputResult = ConsoleReadLine(Username, sizeof(Username));
        
        // Обработка Ctrl+C
        if (InputResult == -2) {  // Ctrl+C
            ConsolePrint("\nLogging in as Guest...\n");

            if (UserManagerLogin("Guest", "") != SUCCESS) {
                ConsolePrint("\033[31mGuest login failed.\033[0m\n");
                continue;
            }
            
            // Сохраняем БД на диск (если диск смонтирован)
            if (GDbPath[0] != '\0') {
                UserManagerSave(GDbPath);
            }
            
            ConsolePrint("\033[32mLogged in as Guest\033[0m (uid=%u)\n", 
                         UserManagerGetSession()->Uid);
            ConsolePrint("Use 'Mount' to mount a drive, then 'User --Save' to save accounts.\n\n");
            return;
        }
        
        if (InputResult <= 0 || Username[0] == '\0') {
            continue;
        }

        ConsolePrint("Password: ");
        ConsoleSetInputEcho(FALSE);
        InputResult = ConsoleReadLine(Password, sizeof(Password));
        ConsoleSetInputEcho(TRUE);
        ConsolePrint("\n");
        
        if (InputResult == -2) {  // Ctrl+C во время пароля
            ConsolePrint("Login cancelled.\n");
            continue;
        }

        Ret = UserManagerLogin(Username, Password);
        SecureMemZero(Password, sizeof(Password));
        if (Ret == SUCCESS) {
            const TosSession *Session = UserManagerGetSession();
            ConsolePrint("\033[32mWelcome, %s\033[0m (%s, uid=%u)\n\n",
                         Session->Username,
                         UserRoleToString(Session->Role),
                         Session->Uid);
            return;
        }

        if (Ret == CHECK_ERROR) {
            ConsolePrint("\033[31mInvalid password.\033[0m\n");
        } else if (Ret == NOT_FOUND) {
            ConsolePrint("\033[31mUser not found.\033[0m\n");
        } else if (Ret == PERMISSION_DENIED) {
            ConsolePrint("\033[31mAccount disabled or temporarily locked.\033[0m\n");
        } else {
            ConsolePrint("\033[31mLogin failed (%s).\033[0m\n", ReturnCode2String(Ret));
        }
    }
}

static NOPTR HandleLoginCommand(CHAR *Args) {
    CHAR *Username;
    CHAR *Password;
    INT Ret;

    if (UserManagerIsLoggedIn()) {
        ConsolePrint("Already logged in as %s. Use Logout first.\n",
                     UserManagerGetSession()->Username);
        return;
    }

    Username = StrTokR(Args, " ", &Args);
    Password = Args;
    if (!Username || !Password || Username[0] == '\0' || Password[0] == '\0') {
        ConsolePrint("Usage: Login <username> <password>\n");
        return;
    }

    Ret = UserManagerLogin(Username, Password);
    if (Password) {
        SecureMemZero(Password, StrLen(Password) + 1);
    }
    if (Ret == SUCCESS) {
        const TosSession *Session = UserManagerGetSession();
        ConsolePrint("Logged in as %s (%s, uid=%u)\n",
                     Session->Username,
                     UserRoleToString(Session->Role),
                     Session->Uid);
        AuditLog(AUDIT_LEVEL_DANGER, AUDIT_EVENT_LOGIN, "User '%s' logged as '%s'", Session->Username, UserRoleToString(Session->Role));
	LastCommandResult = 0;
        return;
    }

    if (Ret == CHECK_ERROR) {
        ConsolePrint("Invalid password.\n");
    } else if (Ret == NOT_FOUND) {
        ConsolePrint("User not found.\n");
    } else if (Ret == PERMISSION_DENIED) {
        ConsolePrint("Account disabled or temporarily locked.\n");
    } else {
        ConsolePrint("Login failed (%s).\n", ReturnCode2String(Ret));
    }
    LastCommandResult = -1;
}

static NOPTR HandleLogoutCommand(CHAR *Args) {
    (NOPTR)Args;
    if (!UserManagerIsLoggedIn()) {
        ConsolePrint("Not logged in.\n");
	LastCommandResult = -1;
        return;
    }

    const TosSession *Session = UserManagerGetSession();
    CHAR Username[USER_NAME_MAX];
    StrCpy(Username, Session->Username);

    AuditLog(AUDIT_LEVEL_DANGER, AUDIT_EVENT_LOGOUT, 
             "User '%s' logged out", Username);

    ConsolePrint("Goodbye, %s.\n", Username);

    UserManagerLogout();

    LastCommandResult = 0;
}

static NOPTR HandleWhoamiCommand(CHAR *Args) {
    (NOPTR)Args;
    if (!UserManagerIsLoggedIn()) {
        ConsolePrint("guest (not authenticated)\n");
	LastCommandResult = -1;
        return;
    }

    ConsolePrint("%s uid=%u gid=%u role=%s\n",
                 UserManagerGetSession()->Username,
                 UserManagerGetSession()->Uid,
                 UserManagerGetSession()->Gid,
                 UserRoleToString(UserManagerGetSession()->Role));
    LastCommandResult = 0;
}

static NOPTR HandleUserCommand(CHAR *Args) {
    CHAR *Sub = StrTokR(Args, " ", &Args);

    if (!Sub) {
        ConsolePrint("Usage:\n");
        ConsolePrint("  User --List\n");
        ConsolePrint("  User --Add <name> <password> [--Admin|--User]\n");
        ConsolePrint("  User --Remove <name>\n");
        ConsolePrint("  User --Passwd <name> <old> <new>\n");
        ConsolePrint("  User --Passwd <name> <new>          (Admin only)\n");
        ConsolePrint("  User --Save [path]\n");
        ConsolePrint("  User --Load <path>\n");
	LastCommandResult = -1;
        return;
    }

    if (StrCmp(Sub, "--List") == 0 || StrCmp(Sub, "-l") == 0) {
        if (!ShellRequireAdmin()) {
	    LastCommandResult = -1;
	    return;
	}
        UserManagerListUsers();
	LastCommandResult = 0;
        return;
    }

    if (StrCmp(Sub, "--Add") == 0) {
        CHAR *Name;
        CHAR *Pass;
        UserRole Role = UserRoleUser;

        if (!ShellRequireAdmin()) {
	    LastCommandResult = -1;
	    return;
	}

        Name = StrTokR(Args, " ", &Args);
        Pass = StrTokR(Args, " ", &Args);
        if (!Name || !Pass) {
            ConsolePrint("Usage: User --Add <name> <password> [--Admin|--User]\n");
	    LastCommandResult = -1;
            return;
        }

        if (Args) {
            if (StrCaseCmp(Args, "--Admin") == 0 || StrCaseCmp(Args, "-a") == 0) {
                Role = UserRoleAdmin;
	        LastCommandResult = 0;
            }
        }

        if (UserManagerAddUser(Name, Pass, Role) == SUCCESS) {
            ConsolePrint("User '%s' created (%s).\n", Name, UserRoleToString(Role));
	    AuditLog(AUDIT_LEVEL_DANGER, AUDIT_EVENT_USER_ADD, "User '%s' created by '%s'", Name, UserManagerGetSession()->Username);
	    LastCommandResult = 0;
        } else {
            ConsolePrint("Failed to create user '%s'.\n", Name);
	    LastCommandResult = -1;
        }
        return;
    }

    if (StrCmp(Sub, "--Remove") == 0) {
        CHAR *Name = StrTokR(Args, " ", &Args);
        if (!ShellRequireAdmin()) {
	    LastCommandResult = -1;
	    return;
	}
        if (!Name) {
            ConsolePrint("Usage: User --Remove <name>\n");
	    LastCommandResult = -1;
            return;
        }
        if (UserManagerRemoveUser(Name) == SUCCESS) {
            ConsolePrint("User '%s' removed.\n", Name);
	    AuditLog(AUDIT_LEVEL_CRITICAL, AUDIT_EVENT_USER_DELETE, "User '%s' deleted by '%s'", Name, UserManagerGetSession()->Username);
	    LastCommandResult = 0;
        } else {
            ConsolePrint("Failed to remove user '%s'.\n", Name);
	    LastCommandResult = -1;
        }
        return;
    }

    if (StrCmp(Sub, "--Passwd") == 0) {
        CHAR *Name = StrTokR(Args, " ", &Args);
        CHAR *OldPass = NULLPTR;
        CHAR *NewPass;

        if (!ShellRequireLogin()) {
	    LastCommandResult = -1;
	    return;
        }

        if (!Name) {
            ConsolePrint("Usage: User --Passwd <name> <old> <new>\n");
	    LastCommandResult = -1;
            return;
        }

        if (UserManagerIsAdmin()) {
            NewPass = StrTokR(Args, " ", &Args);
            if (!NewPass) {
                ConsolePrint("Usage: User --Passwd <name> <new>\n");
		LastCommandResult = -1;
                return;
            }
            if (UserManagerSetPassword(Name, NULLPTR, NewPass) == SUCCESS) {
                ConsolePrint("Password updated for '%s'.\n", Name);
		AuditLog(AUDIT_LEVEL_CRITICAL, AUDIT_EVENT_USER_CHANGE_PASS, "Password for '%s' changed bu '%s'", Name, UserManagerGetSession()->Username);
		LastCommandResult = 0;
            } else {
                ConsolePrint("Failed to update password for '%s'.\n", Name);
		LastCommandResult = -1;
            }
            return;
        }

        OldPass = StrTokR(Args, " ", &Args);
        NewPass = StrTokR(Args, " ", &Args);
        if (!OldPass || !NewPass) {
            ConsolePrint("Usage: User --Passwd <name> <old> <new>\n");
	    LastCommandResult = -1;
            return;
        }
        if (UserManagerSetPassword(Name, OldPass, NewPass) == SUCCESS) {
            ConsolePrint("Password updated.\n");
	    AuditLog(AUDIT_LEVEL_CRITICAL, AUDIT_EVENT_USER_CHANGE_PASS, "Password for '%s' changed bu '%s'", Name, UserManagerGetSession()->Username);
	    LastCommandResult = 0;
        } else {
            ConsolePrint("Failed to update password.\n");
	    LastCommandResult = -1;
        }
        return;
    }

    if (StrCmp(Sub, "--Save") == 0) {
        CHAR *Path = Args;
        if (!ShellRequireAdmin()) {
	    LastCommandResult = -1;
	    return;
	}
        while (Path && *Path == ' ') Path++;
        if (!Path || Path[0] == '\0') {
            if (UserManagerSave(NULLPTR) != SUCCESS) {
                ConsolePrint("Save failed. Specify path: User --Save DRIVE::/etc/users.tdb\n");
	        LastCommandResult = -1;
            } else {
                ConsolePrint("User database saved.\n");
		LastCommandResult = 0;
            }
            return;
        }
        if (UserManagerSave(Path) == SUCCESS) {
            ConsolePrint("User database saved to %s\n", Path);
	    LastCommandResult = 0;
        } else {
            ConsolePrint("Failed to save user database.\n");
	    LastCommandResult = -1;
        }
        return;
    }

    if (StrCmp(Sub, "--Load") == 0) {
        CHAR *Path = Args;
        if (!ShellRequireAdmin()) {
	    LastCommandResult = -1;
	    return;
	}
        while (Path && *Path == ' ') Path++;
        if (!Path || Path[0] == '\0') {
            ConsolePrint("Usage: User --Load <path>\n");
	    LastCommandResult = -1;
            return;
        }
        if (UserManagerLoad(Path) == SUCCESS) {
            ConsolePrint("User database loaded from %s\n", Path);
	    LastCommandResult = 0;
        } else {
            ConsolePrint("Failed to load user database.\n");
	    LastCommandResult = -1;
        }
        return;
    }

    ConsolePrint("Unknown User subcommand: %s\n", Sub);
    LastCommandResult = -1;
}

static NOPTR HandlePingCommand(CHAR *Args) {
    CHAR *IpStr;
    IpV4Addr Addr;
    PingStats Stats;
    INT Result;
    
    if (!Args || Args[0] == '\0') {
        ConsolePrint("Usage: Ping <IP> [count] [timeout_ms]\n");
        ConsolePrint("Example: Ping 8.8.8.8\n");
        ConsolePrint("         Ping 8.8.8.8 4 1000\n");
	LastCommandResult = -1;
        return;
    }
    
    // Парсим IP
    IpStr = StrTokR(Args, " ", &Args);
    Addr = IpV4PTon(IpStr);
    
    if (Addr.Addr == 0 && StrCmp(IpStr, "0.0.0.0") != 0) {
        ConsolePrint("Invalid IP address: %s\n", IpStr);
	LastCommandResult = -1;
        return;
    }
    
    // Парсим count (опционально)
    UINT32 Count = 4;
    if (Args && Args[0] != '\0') {
        CHAR *CountStr = StrTokR(Args, " ", &Args);
        Count = (UINT32)AToI(CountStr);
        if (Count == 0 || Count > 100) {
            Count = 4;
        }
    }
    
    // Парсим timeout (опционально)
    UINT32 TimeoutMs = 1000;
    if (Args && Args[0] != '\0') {
        CHAR *TimeoutStr = StrTokR(Args, " ", &Args);
        TimeoutMs = (UINT32)AToI(TimeoutStr);
        if (TimeoutMs < 100 || TimeoutMs > 10000) {
            TimeoutMs = 1000;
        }
    }
    
    ConsolePrint("PING %s (%u packets, %u ms timeout):\n", IpStr, Count, TimeoutMs);
    
    Result = ICmpPing(Addr, Count, TimeoutMs, &Stats);
    
    if (Result != SUCCESS) {
        ConsolePrint("Ping failed: %s\n", ReturnCode2String(Result));
	LastCommandResult = -1;
    }
    LastCommandResult = 0;
}

static NOPTR PrintHelp(NOPTR) {
    ConsolePrint("  Login <user> <pass> - sign in\n");
    ConsolePrint("  Logout              - sign out\n");
    ConsolePrint("  Whoami              - current user\n");
    ConsolePrint("  User --List         - list accounts (Admin)\n");
    ConsolePrint("  User --Add ...      - create account (Admin)\n");
    ConsolePrint("  User --Remove ...   - delete account (Admin)\n");
    ConsolePrint("  User --Passwd ...   - change password\n");
    ConsolePrint("  User --Save/Load    - persist accounts (Admin)\n");
    ConsolePrint("  Help / ?          - this message\n");
    ConsolePrint("  Say <Text>        - print text (supports > and >>)\n");
    ConsolePrint("  Ping <IP> [count] [timeout_ms] - ping\n");
    ConsolePrint("  Clear             - clear screen\n");
    ConsolePrint("  Reboot / Shutdown - power control\n");
    ConsolePrint("  Version / SysInfo - system information\n");
    ConsolePrint("  Execute/Run <path> [args] - run ELF program\n");
    ConsolePrint("  Assemble <src.asm> [out.elf] - NASM-like assembler\n");
    ConsolePrint("  Task --List                      - Show all tasks\n");
    ConsolePrint("  Task --Kill <PID>                - Kill task by PID\n");
    ConsolePrint("  Task --Change -Name <name> -Pid <PID> - Change task name\n");
    ConsolePrint("  Task --Change -Priority <prio> -Pid <PID> - Change priority\n");
    ConsolePrint("  Drive --List              - Show all drives\n");
    ConsolePrint("  Drive --Rename <label> <name> - Rename drive\n");
    ConsolePrint("  Format <drive> <fs>  - Format drive (exfat)\n");
    ConsolePrint("  Mount <drive> [fs]   - Mount drive (paths: DRIVE::/path)\n");
    ConsolePrint("  Ls [path]            - List directory\n");
    ConsolePrint("  Cd [path]            - Change directory (e.g. NVME0::/)\n");
    ConsolePrint("  Pwd                  - Print working directory\n");
    ConsolePrint("  MakeDir <path>       - Create directory\n");
    ConsolePrint("  RemoveDir <path>     - Remove empty directory\n");
    ConsolePrint("  Remove <path>        - Remove file\n");
    ConsolePrint("  Read <path>          - Show file content\n");
    ConsolePrint("  Create <path>        - Create empty file\n");
    ConsolePrint("  Copy <src> <dst>     - Copy file\n");
    ConsolePrint("  Move <src> <dst>     - Move/rename file\n");
    ConsolePrint("  Decon [args]         - Debug console\n");
    ConsolePrint("  Audit [args]         - Audit control\n");
    ConsolePrint("  Snake                - Snake game\n");
    ConsolePrint("  Breakout             - Breakout game\n");
    LastCommandResult = 0;
}

static NOPTR ParseCommand(CHAR *Cmdline, CHAR *Cmd, CHAR **Args) {
    INT I = 0;
    
    while (Cmdline[I] != ' ' && Cmdline[I] != '\0' && I < 63) {
        Cmd[I] = Cmdline[I];
        I++;
    }
    Cmd[I] = '\0';
    
    while (Cmdline[I] == ' ') I++;
    if (Cmdline[I] != '\0') {
        *Args = &Cmdline[I];
    } else {
        *Args = NULLPTR;
    }
}

static NOPTR HandleTaskCommand(CHAR *Args) {
    CHAR *Cmd = StrTokR(Args, " ", &Args);
    
    if (!Cmd) {
        ConsolePrint("No arguments. Enter 'Help'.\n");
	LastCommandResult = -1;
        return;
    }
    
    if (StrCmp(Cmd, "--List") == 0 || StrCmp(Cmd, "-l") == 0) {
        SchedulerListTasks();
	LastCommandResult = 0;
        return;
    }
    
    if (StrCmp(Cmd, "--Kill") == 0 || StrCmp(Cmd, "-k") == 0) {
    	CHAR *PidStr = StrTokR(Args, " ", &Args);
    	if (!ShellRequireAdmin()) {
	    LastCommandResult = -1;
	    return;
	}
    	if (!PidStr) {
            ConsolePrint("Usage: Task --Kill <PID>\n");
	    LastCommandResult = -1;
            return;
    	}
    	UINT32 Pid = AToI(PidStr);
    
    	// 1. НАХОДИМ задачу ДО убийства
    	KTask *Task = SchedulerFindTaskByPid(Pid);
    	if (!Task) {
            ConsolePrint("Task with PID %u not found!\n", Pid);
	    LastCommandResult = -1;
            return;
    	}
    
    	// 2. СОХРАНЯЕМ данные
    	CHAR TaskName[TASK_NAME_MAX];
    	StrCpy(TaskName, Task->Name);
    	const TosSession *Session = UserManagerGetSession();
    	CHAR Username[USER_NAME_MAX];
    	StrCpy(Username, Session->Authenticated ? Session->Username : "unknown");
    
    	// 3. ЛОГИРУЕМ ДО убийства
    	AuditLog(AUDIT_LEVEL_CRITICAL, AUDIT_EVENT_TASK_KILL,
             "Task '%s' (%u) was killed by '%s'",
             TaskName, Pid, Username);
    
    	// 4. ТЕПЕРЬ убиваем
    	SchedulerKillTask(Pid);
        LastCommandResult = 0;
    	return;
    }
    
    if (StrCmp(Cmd, "--Change") == 0 || StrCmp(Cmd, "-c") == 0) {
        if (!ShellRequireAdmin()) {
	    LastCommandResult = -1;
	    return;
	}
        CHAR *SubCmd = StrTokR(Args, " ", &Args);
        if (!SubCmd) {
            ConsolePrint("Usage: Task --Change -Name <name> -Pid <PID>\n");
            ConsolePrint("       Task --Change -Priority <prio> -Pid <PID>\n");
	    LastCommandResult = -1;
            return;
        }
        
        if (StrCmp(SubCmd, "-Name") == 0) {
            CHAR *NewName = StrTokR(Args, " ", &Args);
            CHAR *PidFlag = StrTokR(Args, " ", &Args);
            CHAR *PidStr = StrTokR(Args, " ", &Args);
            
            if (!NewName || !PidFlag || !PidStr || StrCmp(PidFlag, "-Pid") != 0) {
                ConsolePrint("Usage: Task --Change -Name <name> -Pid <PID>\n");
		LastCommandResult = -1;
                return;
            }
            
            UINT32 Pid = AToI(PidStr);
            SchedulerChangeTaskName(Pid, NewName);
	    LastCommandResult = 0;
            return;
        }
        
        if (StrCmp(SubCmd, "-Priority") == 0) {
            CHAR *PrioStr = StrTokR(Args, " ", &Args);
            CHAR *PidFlag = StrTokR(Args, " ", &Args);
            CHAR *PidStr = StrTokR(Args, " ", &Args);
            
            if (!PrioStr || !PidFlag || !PidStr || StrCmp(PidFlag, "-Pid") != 0) {
                ConsolePrint("Usage: Task --Change -Priority <prio> -Pid <PID>\n");
		LastCommandResult = -1;
                return;
            }
            
            UINT8 Priority = (UINT8)AToI(PrioStr);
            UINT32 Pid = AToI(PidStr);
            SchedulerChangeTaskPriority(Pid, Priority);
	    LastCommandResult = 0;
            return;
        }
        
        ConsolePrint("Unknown subcommand: %s\n", SubCmd);
	LastCommandResult = -1;
        return;
    }
    
    ConsolePrint("Unknown option: %s\n", Cmd);
    LastCommandResult = -1;
}

static NOPTR HandleFormatCommand(CHAR *Args) {
    CHAR *DriveName = StrTokR(Args, " ", &Args);
    CHAR *FsType = StrTokR(Args, " ", &Args);
    
    if (!DriveName) {
        ConsolePrint("Usage: Format <drive> <fstype>\n");
        ConsolePrint("  fstype: Exfat Fat32\n");
        ConsolePrint("Example: Format NVME0 Exfat\n");
	LastCommandResult = -1;
        return;
    }
    
    Drive *D = DriveGetByName(DriveName);
    if (!D) {
        ConsolePrint("Drive '%s' not found\n", DriveName);
	LastCommandResult = -1;
        return;
    }
    
    if (!FsType) {
        ConsolePrint("Unknown filesystem: (none)\n");
	LastCommandResult = -1;
        return;
    }
    
    // Исправлено: проверяем сначала Exfat, потом Fat32
    if (StrCmp(FsType, "Exfat") == 0 || StrCmp(FsType, "exfat") == 0) {
        if (ExfatFormat(D) == 0) {
            ConsolePrint("Done.\n");
            AuditLog(AUDIT_LEVEL_CRITICAL, AUDIT_EVENT_FORMAT, "Drive '%s' formatted to EXFAT by '%s'", DriveName, UserManagerGetSession()->Username);
	    LastCommandResult = 0;
        } else {
            ConsolePrint("Format failed!\n");
	    LastCommandResult = -1;
        }
    }
    else if (StrCmp(FsType, "Fat32") == 0 || StrCmp(FsType, "fat32") == 0) {
        if (Fat32Format(D) == 0) {
            ConsolePrint("Done.\n");
	    AuditLog(AUDIT_LEVEL_CRITICAL, AUDIT_EVENT_FORMAT, "Drive '%s' formatted to FAT32 by '%s'", DriveName, UserManagerGetSession()->Username);
	    LastCommandResult = 0;
        } else {
            ConsolePrint("Format failed!\n");
	    LastCommandResult = -1;
        }
    }
    else {
        ConsolePrint("Unknown filesystem: %s\n", FsType);
	LastCommandResult = -1;
    }
}

static NOPTR HandleMountCommand(CHAR *Args) {
    CHAR *DriveName = StrTokR(Args, " ", &Args);
    CHAR *FsType = StrTokR(Args, " ", &Args);
    CHAR MountPath[DRIVE_NAME_MAX + 4];
    
    if (!DriveName) {
        ConsolePrint("Usage: Mount <drive> [fstype]\n");
        ConsolePrint("Example: Mount NVME0 exfat\n");
        ConsolePrint("Paths use DRIVE::/path (e.g. NVME0::/folder)\n");
	LastCommandResult = -1;
        return;
    }
    
    Drive *D = DriveGetByName(DriveName);
    if (!D) {
        ConsolePrint("Drive '%s' not found\n", DriveName);
	LastCommandResult = -1;
        return;
    }

    VfsInode *Root;
    if (VfsMount(FsType, D, &Root) == 0) {
	AuditLog(AUDIT_LEVEL_WARNING, AUDIT_EVENT_MOUNT, "Drive '%s' mounted to '%s' by '%s'", DriveName, MountPath, UserManagerGetSession()->Username);
	LastCommandResult = 0;
        SnPrintf(MountPath, sizeof(MountPath), "%s::/", DriveName);
        ConsolePrint("Mounted %s\n", MountPath);
        if (VfsCd(&CurrentDir, MountPath) == 0) {
            ConsolePrint("Current: %s\n", CurrentPath);
        }
    } else {
        ConsolePrint("Mount failed!\n");
	LastCommandResult = -1;
    }
}

static BOOL IsNamespaceRootDir(VfsInode *Dir) {
    return Dir && Dir == RootInode;
}

static NOPTR HandleLsCommand(CHAR *Args) {
    CHAR *Path = Args;
    VfsInode *Dir;
    INT EntryCount = 0;
    
    if (!Path || Path[0] == '\0') {
        Dir = CurrentDir;
        VfsInodeRef(Dir);
        Path = (CHAR*)CurrentPath;
    } else if (VfsWalk(CurrentDir, Path, &Dir) != 0) {
        ConsolePrint("Ls: cannot access '%s'\n", Path);
	LastCommandResult = -1;
        return;
    }
    
    if (Dir->IMode != FT_DIR) {
        ConsolePrint("Ls: '%s' is not a directory\n", Path);
        VfsInodeUnref(Dir);
	LastCommandResult = -1;
        return;
    }

    if (IsNamespaceRootDir(Dir)) {
        ConsolePrint("(mounted drives — use Cd DRIVE::/ to browse files)\n");
    }
    
    UINT64 Pos = 0;
    CHAR Name[256];
    UINT32 NameLen = sizeof(Name);
    UINT32 Type;
    
    ConsolePrint("\n");
    while (VfsReadDir(Dir, &Pos, Name, &NameLen, &Type) == 0) {
        const CHAR *TypeChar = (Type == FT_DIR) ? "d" : "-";
        ConsolePrint("%s %s\n", TypeChar, Name);
        EntryCount++;
        if (EntryCount > 4096) {
            ConsolePrint("Ls: listing truncated\n");
            break;
        }
    }
    ConsolePrint("\n");
    
    VfsInodeUnref(Dir);
    LastCommandResult = 0;
}

static NOPTR HandleCdCommand(CHAR *Args) {
    CHAR *NewPath = Args;
    
    if (!NewPath || NewPath[0] == '\0') {
        NewPath = "/";
    }
    
    if (VfsCd(&CurrentDir, NewPath) != 0) {
        ConsolePrint("Cd: no such directory: %s\n", NewPath);
	LastCommandResult = -1;
    }
    LastCommandResult = 0;
}

static NOPTR HandlePwdCommand(CHAR *Args) {
    (void)Args;
    CHAR Path[PATH_MAX];
    if (VfsBuildPath(CurrentDir, Path, PATH_MAX) == 0) {
        ConsolePrint("%s\n", Path);
    } else {
        ConsolePrint("/\n");
    }
    LastCommandResult = 0;
}

static NOPTR HandleMakeDirCommand(CHAR *Args) {
    CHAR *Path = Args;
    VfsInode *Parent;
    CHAR Name[NAME_MAX];
    
    if (!Path || Path[0] == '\0') {
        ConsolePrint("MakeDir: missing operand\n");
	LastCommandResult = -1;
        return;
    }
    
    if (VfsResolvePath(CurrentDir, Path, &Parent, Name, FALSE) != 0) {
        ConsolePrint("MakeDir: failed to resolve path '%s'\n", Path);
	LastCommandResult = -1;
        return;
    }
    
    if (Name[0] == '\0') {
        ConsolePrint("MakeDir: cannot create root\n");
        VfsInodeUnref(Parent);
	LastCommandResult = -1;
        return;
    }
    
    if (Parent->IMode != FT_DIR) {
        ConsolePrint("MakeDir: parent is not a directory\n");
        VfsInodeUnref(Parent);
	LastCommandResult = -1;
        return;
    }
    
    VfsInode *Result;
    if (Parent->IOp->Mkdir(Parent, Name, FT_DIR, &Result) != 0) {
        ConsolePrint("MakeDir: failed to create '%s'\n", Path);
        VfsInodeUnref(Parent);
	LastCommandResult = -1;
        return;
    }
    
    VfsInodeUnref(Result);
    VfsInodeUnref(Parent);
    ConsolePrint("Created: %s\n", Path);
    LastCommandResult = 0;
}

static NOPTR HandleRemoveCommand(CHAR *Args) {
    CHAR *Path = Args;
    VfsInode *Parent;
    CHAR Name[NAME_MAX];
    
    if (!Path || Path[0] == '\0') {
        ConsolePrint("Remove: missing operand\n");
	LastCommandResult = -1;
        return;
    }
    
    if (VfsResolvePath(CurrentDir, Path, &Parent, Name, FALSE) != 0) {
        ConsolePrint("Remove: cannot resolve '%s'\n", Path);
	LastCommandResult = -1;
        return;
    }
    
    if (Name[0] == '\0') {
        ConsolePrint("Remove: cannot remove root\n");
        VfsInodeUnref(Parent);
	LastCommandResult = -1;
        return;
    }
    
    if (Parent->IOp->Unlink(Parent, Name) != 0) {
        ConsolePrint("Remove: cannot remove '%s'\n", Path);
	VfsInodeUnref(Parent);
	LastCommandResult = -1;
	return;
    } else {
        ConsolePrint("Removed: %s\n", Path);
	LastCommandResult = 0;
    }
    
    VfsInodeUnref(Parent);
}

static NOPTR HandleRemoveDirCommand(CHAR *Args) {
    CHAR *Path = Args;
    VfsInode *Parent;
    CHAR Name[NAME_MAX];
    
    if (!Path || Path[0] == '\0') {
        ConsolePrint("RemoveDir: missing operand\n");
	LastCommandResult = -1;
        return;
    }
    
    if (VfsResolvePath(CurrentDir, Path, &Parent, Name, FALSE) != 0) {
        ConsolePrint("RemoveDir: cannot resolve '%s'\n", Path);
	LastCommandResult = -1;
        return;
    }
    
    if (Name[0] == '\0') {
        ConsolePrint("RemoveDir: cannot remove root\n");
	LastCommandResult = -1;
        VfsInodeUnref(Parent);
        return;
    }
    
    if (Parent->IOp->Rmdir(Parent, Name) != 0) {
        ConsolePrint("RemoveDir: failed to remove '%s'\n", Path);
	VfsInodeUnref(Parent);
	LastCommandResult = -1;
	return;
    } else {
        ConsolePrint("Removed directory: %s\n", Path);
	LastCommandResult = 0;
    }
    
    VfsInodeUnref(Parent);
}

static NOPTR HandleReadCommand(CHAR *Args) {
    CHAR *Path = Args;
    VfsFile *File;
    
    if (!Path || Path[0] == '\0') {
        ConsolePrint("Read: missing operand\n");
	LastCommandResult = -1;
        return;
    }
    
    if (VfsOpen(CurrentDir, Path, O_READ, &File) != 0) {
        ConsolePrint("Read: cannot open '%s'\n", Path);
	LastCommandResult = -1;
        return;
    }
    
    CHAR Buf[1024];
    UINT32 Read;
    while (VfsRead(File, Buf, sizeof(Buf) - 1, &Read) == 0 && Read > 0) {
        Buf[Read] = '\0';
        ConsolePrint("%s", Buf);
    }
    
    VfsClose(File);
    ConsolePrint("\n");
    LastCommandResult = 0;
}

static NOPTR HandleCreateCommand(CHAR *Args) {
    CHAR *Path = Args;
    VfsFile *File;
    
    if (!Path || Path[0] == '\0') {
        ConsolePrint("Create: missing operand\n");
	LastCommandResult = -1;
        return;
    }
    
    if (IsNamespaceRootDir(CurrentDir) && StrStr(Path, "::") == NULLPTR) {
        ConsolePrint("Create: use DRIVE::/name or Cd DRIVE::/ first\n");
	LastCommandResult = -1;
        return;
    }

    if (VfsOpen(CurrentDir, Path, O_CREAT | O_WRITE, &File) == 0) {
        VfsClose(File);
        ConsolePrint("Created: %s\n", Path);
	LastCommandResult = 0;
    } else {
        ConsolePrint("Create: failed (use DRIVE::/path or Cd DRIVE::/ first)\n");
	LastCommandResult = -1;
    }
}

static NOPTR HandleCopyCommand(CHAR *Args) {
    CHAR *Src = StrTokR(Args, " ", &Args);
    CHAR *Dst = StrTokR(Args, " ", &Args);
    VfsFile *SrcFile, *DstFile;
    
    if (!Src || !Dst) {
        ConsolePrint("Copy: missing operands\n");
        ConsolePrint("Usage: Copy <source> <dest>\n");
	LastCommandResult = -1;
        return;
    }
    
    if (VfsOpen(CurrentDir, Src, O_READ, &SrcFile) != 0) {
        ConsolePrint("Copy: cannot open '%s'\n", Src);
	LastCommandResult = -1;
        return;
    }
    
    if (VfsOpen(CurrentDir, Dst, O_WRITE | O_CREAT | O_TRUNC, &DstFile) != 0) {
        ConsolePrint("Copy: cannot create '%s'\n", Dst);
        VfsClose(SrcFile);
	LastCommandResult = -1;
        return;
    }
    
    CHAR Buf[4096];
    UINT32 Read, Written;
    while (VfsRead(SrcFile, Buf, sizeof(Buf), &Read) == 0 && Read > 0) {
        if (VfsWrite(DstFile, Buf, Read, &Written) != 0 || Written != Read) {
            ConsolePrint("Copy: write error\n");
	    LastCommandResult = -1;
            break;
        }
    }
    
    ConsolePrint("Copied %s -> %s\n", Src, Dst);
    VfsClose(SrcFile);
    VfsClose(DstFile);
    LastCommandResult = 0;
}

static NOPTR HandleMoveCommand(CHAR *Args) {
    CHAR *Src = StrTokR(Args, " ", &Args);
    CHAR *Dst = StrTokR(Args, " ", &Args);
    VfsInode *SrcNode;
    
    if (!Src || !Dst) {
        ConsolePrint("Move: missing operands\n");
	LastCommandResult = -1;
        return;
    }
    
    // Проверяем существует ли источник
    if (VfsWalk(CurrentDir, Src, &SrcNode) != 0) {
        ConsolePrint("Move: cannot find '%s'\n", Src);
	LastCommandResult = -1;
        return;
    }
    VfsInodeUnref(SrcNode);
    
    // Пробуем rename (если ФС поддерживает)
    VfsInode *SrcParent, *DstParent;
    CHAR SrcName[NAME_MAX], DstName[NAME_MAX];
    
    if (VfsResolvePath(CurrentDir, Src, &SrcParent, SrcName, FALSE) == 0 &&
        VfsResolvePath(CurrentDir, Dst, &DstParent, DstName, FALSE) == 0 &&
        SrcParent->IOp->Rename && 
        SrcParent->IOp->Rename(SrcParent, SrcName, DstParent, DstName) == 0) {
        
        ConsolePrint("Moved %s -> %s\n", Src, Dst);
        VfsInodeUnref(SrcParent);
        VfsInodeUnref(DstParent);
	LastCommandResult = 0;
        return;
    }
    
    if (SrcParent) VfsInodeUnref(SrcParent);
    if (DstParent) VfsInodeUnref(DstParent);
    
    // Fallback: copy + unlink
    CHAR CopyArgs[PATH_MAX * 2 + 2];
    SnPrintf(CopyArgs, sizeof(CopyArgs), "%s %s", Src, Dst);
    HandleCopyCommand(CopyArgs);
    HandleRemoveCommand(Src);
    LastCommandResult = 0;
}

static NOPTR HandleAssembleCommand(CHAR *Args) {
    CHAR *Src = StrTokR(Args, " ", &Args);
    CHAR *Dst;
    CHAR OutPath[PATH_MAX];

    if (!Src || Src[0] == '\0') {
        ConsolePrint("Usage: Assemble <source.asm> [output.elf]\n");
        ConsolePrint("Example: Assemble hello.asm hello.elf\n");
	LastCommandResult = -1;
        return;
    }

    Dst = Args;
    if (!Dst || Dst[0] == '\0') {
        StrnCpy(OutPath, Src, sizeof(OutPath) - 5);
        {
            CHAR *Dot = StrrChr(OutPath, '.');
            if (Dot) {
                *Dot = '\0';
            }
        }
        StrCat(OutPath, ".elf");
        Dst = OutPath;
    }

    if (TosAsmAssembleFile(CurrentDir, Src, Dst) != SUCCESS) {
        ConsolePrint("Assemble: failed\n");
	LastCommandResult = -1;
    }
    LastCommandResult = 0;
}

static NOPTR HandleVersionCommand(CHAR *Args) {
    (void)Args;
    ConsolePrint("Thunder OS (TOS) version 0.04\n");
    ConsolePrint("Monolithic x86_64 kernel with VFS, scheduler, syscalls\n");
    LastCommandResult = 0;
}

static NOPTR HandleSysInfoCommand(CHAR *Args) {
    (void)Args;
    if (UserManagerIsLoggedIn()) {
        ConsolePrint("User: %s (%s, uid=%u gid=%u)\n",
                     UserManagerGetSession()->Username,
                     UserRoleToString(UserManagerGetSession()->Role),
                     UserManagerGetSession()->Uid,
                     UserManagerGetSession()->Gid);
    } else {
        ConsolePrint("User: guest (not authenticated)\n");
    }
    ConsolePrint("PID: %u  Parent: %u\n",
                   SchedulerGetPid(), SchedulerGetParentPid());
    ConsolePrint("CWD: %s\n", CurrentPath);
    ConsolePrint("Syscalls: 23 (exit, io, fs, exec, wait, kill, uid, ...)\n");
    ConsolePrint("Use Drive --List, Mount, Cd DRIVE::/ to start\n");
    LastCommandResult = 0;
}

static NOPTR HandleDeconCommand(CHAR *Args) {
    if (!Args || Args[0] == '\0') {
        ConsolePrint("Usage:\n");
        ConsolePrint("  Decon --Show   - Show debug buffer\n");
        ConsolePrint("  Decon --Clear  - Clear debug buffer\n");
        ConsolePrint("  Decon --Size   - Show buffer size\n");
        ConsolePrint("  Decon --Save <path> - Save buffer to file\n");
	LastCommandResult = -1;
        return;
    }
    
    if (StrCmp(Args, "--Show") == 0 || StrCmp(Args, "-s") == 0) {
        DeconShow();
	LastCommandResult = 0;
        return;
    }
    
    if (StrCmp(Args, "--Clear") == 0 || StrCmp(Args, "-c") == 0) {
        DeconClear();
	LastCommandResult = 0;
        return;
    }
    
    if (StrCmp(Args, "--Size") == 0 || StrCmp(Args, "-z") == 0) {
        ConsolePrint("Decon buffer: %u bytes used\n", DeconGetSize());
	LastCommandResult = 0;
        return;
    }
    
    if (StrCmp(Args, "--Save") == 0) {
        CHAR *Path = Args + 6;
        while (*Path == ' ') Path++;
        if (*Path == '\0') {
            ConsolePrint("Usage: Decon --Save <path>\n");
	    LastCommandResult = -1;
            return;
        }
        
        // Сохраняем буфер в файл
        VfsFile *File;
        if (VfsOpen(CurrentDir, Path, O_WRITE | O_CREAT | O_TRUNC, &File) != 0) {
            ConsolePrint("Failed to open '%s'\n", Path);
	    LastCommandResult = -1;
            return;
        }
        
        UINT32 Written;
        const CHAR *Buf = DeconGetBuffer();
        UINT32 Size = DeconGetSize();
        
        if (Buf && Size > 0) {
            VfsWrite(File, Buf, Size, &Written);
        }
        
        VfsClose(File);
        ConsolePrint("Saved %u bytes to '%s'\n", Size, Path);
	LastCommandResult = 0;
        return;
    }
    
    ConsolePrint("Unknown Decon option: %s\n", Args);
    LastCommandResult = -1;
}

static NOPTR HandleAuditCommand(CHAR *Args) {
    if (!Args || Args[0] == '\0') {
        ConsolePrint("Usage:\n");
        ConsolePrint("  Audit --Show   - Show audit log (Admin only)\n");
        ConsolePrint("  Audit --Clear  - Clear audit log (Admin only)\n");
        ConsolePrint("  Audit --Save <path> - Save to file (Admin only)\n");
        ConsolePrint("  Audit --Stats  - Show statistics\n");
	LastCommandResult = -1;
        return;
    }
    
    if (StrCmp(Args, "--Show") == 0 || StrCmp(Args, "-s") == 0) {
        AuditShow();
	LastCommandResult = 0;
        return;
    }
    
    if (StrCmp(Args, "--Clear") == 0 || StrCmp(Args, "-c") == 0) {
        AuditClear();
	LastCommandResult = 0;
        return;
    }
    
    if (StrCmp(Args, "--Stats") == 0) {
        ConsolePrint("Audit statistics:\n");
        ConsolePrint("  Total entries: %u\n", AuditGetCount());
        ConsolePrint("  Warnings:     %u\n", AuditGetWarnings());
        ConsolePrint("  Danger:       %u\n", AuditGetDanger());
        ConsolePrint("  CRITICAL:     %u\n", AuditGetCritical());
	LastCommandResult = 0;
        return;
    }
    
    if (StrCmp(Args, "--Save") == 0) {
        CHAR *Path = Args + 6;
        while (*Path == ' ') Path++;
        AuditSave(Path);
	LastCommandResult = 0;
        return;
    }
    
    ConsolePrint("Unknown Audit option: %s\n", Args);
    LastCommandResult = -1;
}

static NOPTR NormalizeCommand(CHAR *Cmd);
static NOPTR ShellDispatchCommand(const CHAR *Cmd, CHAR *Args, RedirType Redir, const CHAR *OutFile);

// В ExecuteCommandChain:
static NOPTR ExecuteCommandChain(const CHAR *CmdLine) {
    CHAR *Commands[16];
    INT Count;
    INT I;

    Count = ParseAndChain(CmdLine, Commands, 16);
    if (Count == 0) {
        return;
    }

    for (I = 0; I < Count; I++) {
        CHAR Cmd[64];
        CHAR *Args;
        RedirType Redir = REDIR_NONE;
        CHAR *OutFile = NULLPTR;
        CHAR TempCmdLine[1024];

        StrCpy(TempCmdLine, Commands[I]);
        ParseRedirection(TempCmdLine, &Redir, &OutFile);
        ParseCommand(TempCmdLine, Cmd, &Args);
        NormalizeCommand(Cmd);

        ConsoleBeginCommandOutput();
        ShellDispatchCommand(Cmd, Args, Redir, OutFile);
        ConsoleEndCommandOutput();

        if (LastCommandResult != 0 && I < Count - 1) {
            ConsolePrint("[!] Command failed (exit code %d), stopping chain\n", LastCommandResult);
            return;
        }
    }
}

static BOOL CmdMatches(const CHAR *Cmd, const CHAR *Canon) {
    return StrCaseCmp(Cmd, Canon) == 0;
}

static NOPTR NormalizeCommand(CHAR *Cmd) {
    if (CmdMatches(Cmd, "?") || CmdMatches(Cmd, "h")) {
        StrCpy(Cmd, "Help");
    } else if (CmdMatches(Cmd, "ls")) {
        StrCpy(Cmd, "Ls");
    } else if (CmdMatches(Cmd, "cd")) {
        StrCpy(Cmd, "Cd");
    } else if (CmdMatches(Cmd, "pwd")) {
        StrCpy(Cmd, "Pwd");
    } else if (CmdMatches(Cmd, "mkdir")) {
        StrCpy(Cmd, "MakeDir");
    } else if (CmdMatches(Cmd, "rm")) {
        StrCpy(Cmd, "Remove");
    } else if (CmdMatches(Cmd, "rmdir")) {
        StrCpy(Cmd, "RemoveDir");
    } else if (CmdMatches(Cmd, "cat") || CmdMatches(Cmd, "type")) {
        StrCpy(Cmd, "Read");
    } else if (CmdMatches(Cmd, "cp")) {
        StrCpy(Cmd, "Copy");
    } else if (CmdMatches(Cmd, "mv")) {
        StrCpy(Cmd, "Move");
    } else if (CmdMatches(Cmd, "run") || CmdMatches(Cmd, "exec")) {
        StrCpy(Cmd, "Execute");
    } else if (CmdMatches(Cmd, "asm") || CmdMatches(Cmd, "nasm")) {
        StrCpy(Cmd, "Assemble");
    } else if (CmdMatches(Cmd, "ver")) {
        StrCpy(Cmd, "Version");
    } else if (CmdMatches(Cmd, "sysinfo") || CmdMatches(Cmd, "info")) {
        StrCpy(Cmd, "SysInfo");
    } else if (CmdMatches(Cmd, "help")) {
        StrCpy(Cmd, "Help");
    } else if (CmdMatches(Cmd, "clear") || CmdMatches(Cmd, "cls")) {
        StrCpy(Cmd, "Clear");
    } else if (CmdMatches(Cmd, "reboot")) {
        StrCpy(Cmd, "Reboot");
    } else if (CmdMatches(Cmd, "shutdown") || CmdMatches(Cmd, "poweroff")) {
        StrCpy(Cmd, "Shutdown");
    } else if (CmdMatches(Cmd, "format")) {
        StrCpy(Cmd, "Format");
    } else if (CmdMatches(Cmd, "mount")) {
        StrCpy(Cmd, "Mount");
    } else if (CmdMatches(Cmd, "task")) {
        StrCpy(Cmd, "Task");
    } else if (CmdMatches(Cmd, "drive")) {
        StrCpy(Cmd, "Drive");
    } else if (CmdMatches(Cmd, "say") || CmdMatches(Cmd, "echo")) {
        StrCpy(Cmd, "Say");
    } else if (CmdMatches(Cmd, "create") || CmdMatches(Cmd, "touch")) {
        StrCpy(Cmd, "Create");
    } else if (CmdMatches(Cmd, "assemble")) {
        StrCpy(Cmd, "Assemble");
    } else if (CmdMatches(Cmd, "version")) {
        StrCpy(Cmd, "Version");
    } else if (CmdMatches(Cmd, "login")) {
        StrCpy(Cmd, "Login");
    } else if (CmdMatches(Cmd, "logout")) {
        StrCpy(Cmd, "Logout");
    } else if (CmdMatches(Cmd, "whoami")) {
        StrCpy(Cmd, "Whoami");
    } else if (CmdMatches(Cmd, "user") || CmdMatches(Cmd, "users")) {
        StrCpy(Cmd, "User");
    } else if (CmdMatches(Cmd, "ping")) {
    	StrCpy(Cmd, "Ping");
    } else if (CmdMatches(Cmd, "decon")) {
	    StrCpy(Cmd, "Decon");
    } else if (CmdMatches(Cmd, "audit")) {
	    StrCpy(Cmd, "Audit");
    } else if (CmdMatches(Cmd, "snake")) {
        StrCpy(Cmd, "Snake");
    } else if (CmdMatches(Cmd, "breakout")) {
        StrCpy(Cmd, "Breakout");
    }
}

static NOPTR HandleExecuteCommand(CHAR *Args) {
    CHAR *Path = Args;
    VfsFile *File;
    UINT8 *FileData;
    UINT64 FileSize;
    UINT32 Read;
    ElfLoadedImage Image;
    ElfLoadResult Result;
    CHAR *Argv[16];
    INT Argc = 0;
    CHAR *SavePtr;
    CHAR *Token;
    CHAR PathCopy[PATH_MAX];
    
    if (!Path || Path[0] == '\0') {
        ConsolePrint("Execute: missing operand\n");
        ConsolePrint("Usage: Execute <path> [args...]\n");
        return;
    }
    
    /* Parse arguments: first is path, rest are program arguments */
    StrCpy(PathCopy, Path);
    Token = StrTokR(PathCopy, " ", &SavePtr);
    if (!Token) {
        ConsolePrint("Execute: invalid path\n");
        return;
    }
    
    /* Store arguments */
    Argv[Argc++] = Token;
    while ((Token = StrTokR(NULLPTR, " ", &SavePtr)) && Argc < 15) {
        Argv[Argc++] = Token;
    }
    Argv[Argc] = NULLPTR;
    
    /* Open ELF file */
    if (VfsOpen(CurrentDir, Argv[0], O_READ, &File) != 0) {
        ConsolePrint("Execute: cannot open '%s'\n", Argv[0]);
        return;
    }
    
    /* Get file size */
    FileSize = File->FInode->ISize;
    if (FileSize == 0 || FileSize > 16 * 1024 * 1024) {  /* Limit to 16 MB */
        ConsolePrint("Execute: invalid file size (%llu bytes)\n", FileSize);
        VfsClose(File);
        return;
    }
    
    /* Allocate buffer for file */
    FileData = (UINT8*)MemoryAllocate((USIZE)FileSize);
    if (!FileData) {
        ConsolePrint("Execute: out of memory\n");
        VfsClose(File);
        return;
    }
    
    /* Read file */
    if (VfsRead(File, FileData, (UINT32)FileSize, &Read) != 0 || Read != FileSize) {
        ConsolePrint("Execute: failed to read file\n");
        MemoryFree(FileData);
        VfsClose(File);
        return;
    }
    VfsClose(File);
    
    /* Parse and load ELF */
    Result = ElfLoad(FileData, (USIZE)FileSize, &Image);
    MemoryFree(FileData);
    
    if (Result != ELF_LOAD_SUCCESS) {
        switch (Result) {
            case ELF_LOAD_INVALID_MAGIC:
                ConsolePrint("Execute: invalid ELF magic (not an ELF file)\n");
                break;
            case ELF_LOAD_WRONG_CLASS:
                ConsolePrint("Execute: not a 64-bit ELF file\n");
                break;
            case ELF_LOAD_NOT_EXEC:
                ConsolePrint("Execute: not an executable file\n");
                break;
            case ELF_LOAD_NO_MEMORY:
                ConsolePrint("Execute: out of memory\n");
                break;
            case ELF_LOAD_INVALID_PHDR:
                ConsolePrint("Execute: invalid program headers\n");
                break;
            case ELF_LOAD_RELOCATION_ERROR:
                ConsolePrint("Execute: relocation error\n");
                break;
            default:
                ConsolePrint("Execute: unknown error (%d)\n", Result);
                break;
        }
        return;
    }
    
    /* Store program name */
    Image.ProgramName = MemoryAllocate(StrLen(Argv[0]) + 1);
    if (Image.ProgramName) {
        StrCpy((CHAR*)Image.ProgramName, Argv[0]);
    }
    
    ConsolePrint("Execute: running '%s' (entry=0x%llX)\n", Argv[0], Image.EntryPoint);
    
    Result = ElfExecute(&Image, Argc, Argv);
    if (Result != ELF_LOAD_SUCCESS) {
        ConsolePrint("Execute: failed to start program (%d)\n", Result);
        ElfUnload(&Image);
        return;
    }

    ElfUnload(&Image);

    AuditLog(AUDIT_LEVEL_DANGER, AUDIT_EVENT_EXECUTE, "'%s' executed by '%s'", Argv[0], UserManagerGetSession()->Username);
}

static NOPTR ShellDispatchCommand(const CHAR *Cmd, CHAR *Args, RedirType Redir, const CHAR *OutFile) {
    if (StrCmp(Cmd, "Help") == 0) {
        PrintHelp();
    }
    else if (StrCmp(Cmd, "Login") == 0) {
        HandleLoginCommand(Args);
    }
    else if (StrCmp(Cmd, "Logout") == 0) {
        HandleLogoutCommand(Args);
    }
    else if (StrCmp(Cmd, "Whoami") == 0) {
        HandleWhoamiCommand(Args);
    }
    else if (StrCmp(Cmd, "User") == 0) {
        HandleUserCommand(Args);
    }
    else if (StrCmp(Cmd, "Clear") == 0) {
        if (!ShellRequireLogin()) return;
        ConsoleClear();
    }
    else if (StrCmp(Cmd, "Reboot") == 0) {
        if (!ShellRequireAdmin()) return;
	GuiReboot();
        AcpiReboot();
    }
    else if (StrCmp(Cmd, "Ping") == 0) {
        if (!ShellRequireLogin()) return;
    	HandlePingCommand(Args);
    }
    else if (StrCmp(Cmd, "Audit") == 0) {
	if (!ShellRequireAdmin()) return;
	HandleAuditCommand(Args);
    }
    else if (StrCmp(Cmd, "Shutdown") == 0) {
        if (!ShellRequireAdmin()) return;
		GuiShutdown();
        AcpiShutdown();
    }
    else if (StrCmp(Cmd, "Task") == 0) {
        if (!ShellRequireLogin()) return;
        HandleTaskCommand(Args);
    }
    else if (StrCmp(Cmd, "Say") == 0) {
        if (!ShellRequireLogin()) return;
        HandleSayWithRedirection(Args, Redir, OutFile);
    }
    else if (StrCmp(Cmd, "Drive") == 0) {
        if (!ShellRequireLogin()) return;
        if (!Args || StrCmp(Args, "--List") == 0) {
            DrivePrintInfo();
        }
        else if (StrnCmp(Args, "--Rename", 8) == 0) {
            if (!ShellRequireAdmin()) return;
            CHAR *Label = Args + 8;
            while (*Label == ' ') Label++;
            CHAR *NewName = StrChr(Label, ' ');
            if (!NewName) {
                ConsolePrint("Usage: Drive --Rename <label> <newname>\n");
                return;
            }
            *NewName++ = '\0';
            while (*NewName == ' ') NewName++;

            if (DriveSetNameByLabel(Label, NewName) != SUCCESS) {
                ConsolePrint("Failed to rename drive with label '%s'\n", Label);
            }
        }
        else {
            ConsolePrint("Unknown subcommand: %s\n", Args);
        }
    }
    else if (StrCmp(Cmd, "Format") == 0) {
        if (!ShellRequireAdmin()) return;
        HandleFormatCommand(Args);
    }
    else if (StrCmp(Cmd, "Mount") == 0) {
        if (!ShellRequireAdmin()) return;
        HandleMountCommand(Args);
    }
    else if (StrCmp(Cmd, "Ls") == 0) {
        if (!ShellRequireLogin()) return;
        HandleLsCommand(Args);
    }
    else if (StrCmp(Cmd, "Cd") == 0) {
        if (!ShellRequireLogin()) return;
        HandleCdCommand(Args);
    }
    else if (StrCmp(Cmd, "Pwd") == 0) {
        if (!ShellRequireLogin()) return;
        HandlePwdCommand(Args);
    }
    else if (StrCmp(Cmd, "MakeDir") == 0) {
        if (!ShellRequireLogin()) return;
        HandleMakeDirCommand(Args);
    }
    else if (StrCmp(Cmd, "RemoveDir") == 0) {
        if (!ShellRequireLogin()) return;
        HandleRemoveDirCommand(Args);
    }
    else if (StrCmp(Cmd, "Remove") == 0) {
        if (!ShellRequireLogin()) return;
        HandleRemoveCommand(Args);
    }
    else if (StrCmp(Cmd, "Read") == 0) {
        if (!ShellRequireLogin()) return;
        HandleReadCommand(Args);
    }
    else if (StrCmp(Cmd, "Create") == 0) {
        if (!ShellRequireLogin()) return;
        HandleCreateCommand(Args);
    }
    else if (StrCmp(Cmd, "Copy") == 0) {
        if (!ShellRequireLogin()) return;
        HandleCopyCommand(Args);
    }
    else if (StrCmp(Cmd, "Move") == 0) {
        if (!ShellRequireLogin()) return;
        HandleMoveCommand(Args);
    }
    else if (StrCmp(Cmd, "Execute") == 0 || StrCmp(Cmd, "Exec") == 0 || StrCmp(Cmd, "Run") == 0) {
        if (!ShellRequireLogin()) return;
        HandleExecuteCommand(Args);
    }
    else if (StrCmp(Cmd, "Assemble") == 0) {
        if (!ShellRequireAdmin()) return;
        HandleAssembleCommand(Args);
    }
    else if (StrCmp(Cmd, "Version") == 0) {
        HandleVersionCommand(Args);
    }
    else if (StrCmp(Cmd, "SysInfo") == 0) {
        HandleSysInfoCommand(Args);
    }
    else if (StrCmp(Cmd, "Decon") == 0) {
	    if (!ShellRequireAdmin()) return;
	    HandleDeconCommand(Args);
    } 
    else if (StrCmp(Cmd, "Snake") == 0) {
        if (!ShellRequireLogin()) return;
        SnakeStart();
    }
    else if (StrCmp(Cmd, "Breakout") == 0) {
        if (!ShellRequireLogin()) return;
        BreakoutStart();
    }
    else if (Cmd[0] != '\0') {
        ConsolePrint("\033[31mUnknown command:\033[0m %s\n", Cmd);
	    LastCommandResult = -1;
    }
}

NOPTR Shell64Entry(NOPTR *Arg) {
    (NOPTR)Arg;
    while (Init64Ready != 1) {
        SchedulerYield();
    }

    UserManagerTryAutoLoad();
    UserManagerRepairDefaults();

    while (!UserManagerIsLoggedIn()) {
        ShellInteractiveLogin();
    }

    CHAR RawCmdLine[1024];
    CHAR CmdLine[1024];
    CHAR Cmd[64];
    CHAR *Args;
    CHAR *OutFile;
    RedirType Redir;
    
    for (;;) {
        if (!UserManagerIsLoggedIn()) {
            ShellInteractiveLogin();
        }

        ConsoleShowPrompt();

        if (ConsoleReadLine(RawCmdLine, sizeof(RawCmdLine)) <= 0) {
            continue;
        }
        
        // <-- ОДИН РАЗ ПАРСИМ
        StrCpy(CmdLine, RawCmdLine);
        ParseRedirection(CmdLine, &Redir, &OutFile);
        ParseCommand(CmdLine, Cmd, &Args);
        NormalizeCommand(Cmd);
        
        ConsoleBeginCommandOutput();

        if (StrStr(RawCmdLine, "&&") != NULLPTR) {
            ExecuteCommandChain(RawCmdLine);
        } else {
            // Обычная команда
            if (StrCaseCmp(Cmd, "Say") == 0 && Redir != REDIR_NONE) {
                HandleSayWithRedirection(Args, Redir, OutFile);
            } else {
                ShellDispatchCommand(Cmd, Args, Redir, OutFile);
            }
        }
        
        // Закрываем файл после выполнения команды
        CloseRedirectFile();
        
        ConsoleEndCommandOutput();
    }
}
