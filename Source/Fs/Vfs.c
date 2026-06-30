#include <Fs/Vfs.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Console.h>
#include <Fs/Exfat.h>
#include <Fs/Fat32.h>

#define VFS_DRIVE_SEP "::"

typedef struct DriveMount {
    CHAR DriveName[DRIVE_NAME_MAX];
    CHAR FsName[16];
    Drive *Dev;
    VfsInode *Root;
    struct DriveMount *Next;
} DriveMount;

static FileSystem *FsList = NULLPTR;
static DriveMount *DriveMounts = NULLPTR;
static VfsOperations GNamespaceOps;
static UINT64 NextIno = 1;

VfsInode *RootInode = NULLPTR;
VfsInode *CurrentDir = NULLPTR;
CHAR CurrentPath[PATH_MAX] = "/";
CHAR CurrentDriveName[DRIVE_NAME_MAX] = "";

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static INT StrCaseEq(const CHAR *A, const CHAR *B) {
    if (!A || !B) {
        return 0;
    }
    while (*A && *B) {
        CHAR Ca = *A;
        CHAR Cb = *B;
        if (Ca >= 'A' && Ca <= 'Z') {
            Ca = (CHAR)(Ca + ('a' - 'A'));
        }
        if (Cb >= 'A' && Cb <= 'Z') {
            Cb = (CHAR)(Cb + ('a' - 'A'));
        }
        if (Ca != Cb) {
            return 0;
        }
        A++;
        B++;
    }
    return *A == *B;
}

static DriveMount *VfsFindDriveMount(const CHAR *Name) {
    DriveMount *M;

    if (!Name || Name[0] == '\0') {
        return NULLPTR;
    }

    for (M = DriveMounts; M; M = M->Next) {
        if (StrCaseEq(M->DriveName, Name)) {
            return M;
        }
    }
    return NULLPTR;
}

static DriveMount *VfsFindDriveMountByDev(Drive *Dev) {
    DriveMount *M;

    if (!Dev) {
        return NULLPTR;
    }

    for (M = DriveMounts; M; M = M->Next) {
        if (M->Dev == Dev) {
            return M;
        }
    }
    return NULLPTR;
}

static DriveMount *VfsFindDriveMountByRoot(VfsInode *Inode) {
    DriveMount *M;

    if (!Inode) {
        return NULLPTR;
    }

    for (M = DriveMounts; M; M = M->Next) {
        if (M->Root == Inode) {
            return M;
        }
    }
    return NULLPTR;
}

static BOOL VfsIsNamespaceRoot(VfsInode *Inode) {
    return Inode && Inode == RootInode;
}

static INT VfsNormalizeRelPath(const CHAR *Path, CHAR *Out, USIZE OutSize) {
    if (!Path || !Out || OutSize == 0) {
        return -1;
    }

    if (Path[0] == '\0') {
        StrCpy(Out, "/");
        return 0;
    }

    if (Path[0] == '/') {
        StrnCpy(Out, Path, OutSize - 1);
        Out[OutSize - 1] = '\0';
        return 0;
    }

    SnPrintf(Out, OutSize, "/%s", Path);
    return 0;
}

/*
 * Parse paths:
 *   NVME0::/dir/file  -> drive=NVME0, rel=/dir/file
 *   ::/dir/file       -> current drive, rel=/dir/file
 *   /                 -> namespace root
 *   dir/file          -> current drive relative (if set)
 */
INT VfsParsePath(const CHAR *Path, CHAR *DriveOut, CHAR *RelOut, USIZE OutSize) {
    const CHAR *Sep;
    const CHAR *After;
    USIZE DriveLen;

    if (!Path || !DriveOut || !RelOut || OutSize == 0) {
        return -1;
    }

    DriveOut[0] = '\0';
    RelOut[0] = '\0';

    if (Path[0] == ':' && Path[1] == ':') {
        if (CurrentDriveName[0] == '\0') {
            return -1;
        }
        StrCpy(DriveOut, CurrentDriveName);
        After = Path + 2;
        if (After[0] == '\0') {
            StrCpy(RelOut, "/");
        } else {
            return VfsNormalizeRelPath(After, RelOut, OutSize);
        }
        return 0;
    }

    Sep = StrStr(Path, VFS_DRIVE_SEP);
    if (Sep) {
        DriveLen = (USIZE)(Sep - Path);
        if (DriveLen == 0 || DriveLen >= DRIVE_NAME_MAX) {
            return -1;
        }
        StrnCpy(DriveOut, Path, DriveLen);
        DriveOut[DriveLen] = '\0';
        After = Sep + 2;
        if (After[0] == '\0') {
            StrCpy(RelOut, "/");
        } else {
            return VfsNormalizeRelPath(After, RelOut, OutSize);
        }
        return 0;
    }

    if (Path[0] == '/' && Path[1] == '\0') {
        return 0;
    }

    if (Path[0] == '/') {
        return VfsNormalizeRelPath(Path, RelOut, OutSize);
    }

    if (CurrentDriveName[0] == '\0') {
        return -1;
    }

    StrCpy(DriveOut, CurrentDriveName);
    return VfsNormalizeRelPath(Path, RelOut, OutSize);
}

static INT VfsWalkWithin(VfsInode *Root, const CHAR *RelPath, VfsInode **Result) {
    VfsInode *Current;
    CHAR PathCopy[PATH_MAX];
    CHAR *SavePtr;
    CHAR *Components[256];
    INT CompCount;
    CHAR *Token;
    INT I;

    if (!Root || !RelPath || !Result) {
        return -1;
    }

    if (RelPath[0] == '\0' || (RelPath[0] == '/' && RelPath[1] == '\0')) {
        *Result = Root;
        VfsInodeRef(*Result);
        return 0;
    }

    StrnCpy(PathCopy, RelPath, PATH_MAX - 1);
    PathCopy[PATH_MAX - 1] = '\0';

    CompCount = 0;
    Token = StrTokR(PathCopy, "/", &SavePtr);
    while (Token && CompCount < 256) {
        Components[CompCount++] = Token;
        Token = StrTokR(NULLPTR, "/", &SavePtr);
    }

    Current = Root;
    VfsInodeRef(Current);

    for (I = 0; I < CompCount; I++) {
        CHAR *Comp = Components[I];
        VfsInode *Next;
        VfsInode *Parent;

        if (StrCmp(Comp, ".") == 0) {
            continue;
        }

        if (StrCmp(Comp, "..") == 0) {
            if (Current == Root) {
                VfsInodeUnref(Current);
                return -1;
            }
            if (VfsParent(Current, &Parent) != 0) {
                VfsInodeUnref(Current);
                return -1;
            }
            VfsInodeUnref(Current);
            Current = Parent;
            continue;
        }

        if (!Current->IOp || !Current->IOp->Lookup) {
            VfsInodeUnref(Current);
            return -1;
        }

        if (Current->IOp->Lookup(Current, Comp, &Next) != 0) {
            VfsInodeUnref(Current);
            return -1;
        }

        VfsInodeUnref(Current);
        Current = Next;
    }

    *Result = Current;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Namespace root (lists mounted drives)                                        */
/* -------------------------------------------------------------------------- */

static INT VfsNamespaceLookup(VfsInode *Dir, const CHAR *Name, VfsInode **Result) {
    DriveMount *M;

    (void)Dir;
    M = VfsFindDriveMount(Name);
    if (!M) {
        return -1;
    }

    *Result = M->Root;
    VfsInodeRef(*Result);
    return 0;
}

static INT VfsNamespaceReadDir(VfsInode *Dir, UINT64 *Pos, CHAR *Name,
                               UINT32 *NameLen, UINT32 *Type) {
    DriveMount *M;
    UINT64 Index;

    (void)Dir;
    if (!Pos || !Name || !NameLen || !Type) {
        return -1;
    }

    Index = 0;
    for (M = DriveMounts; M; M = M->Next, Index++) {
        if (Index == *Pos) {
            StrCpy(Name, M->DriveName);
            *NameLen = StrLen(Name);
            *Type = FT_DIR;
            (*Pos)++;
            return 0;
        }
    }
    return -1;
}

static INT VfsNamespaceGetName(VfsInode *Inode, CHAR *Name, INT MaxLen) {
    (void)Inode;
    if (!Name || MaxLen <= 0) {
        return -1;
    }
    Name[0] = '\0';
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Path building                                                              */
/* -------------------------------------------------------------------------- */

static INT VfsBuildRelPath(VfsInode *Inode, VfsInode *MountRoot, CHAR *Buffer, INT MaxLen) {
    CHAR *Components[256];
    INT CompCount;
    VfsInode *Current;
    VfsInode *Parent;
    INT Pos;
    INT I;

    if (!Inode || !MountRoot || !Buffer || MaxLen <= 1) {
        return -1;
    }

    if (Inode == MountRoot) {
        Buffer[0] = '/';
        Buffer[1] = '\0';
        return 0;
    }

    CompCount = 0;
    Current = Inode;
    VfsInodeRef(Current);

    while (Current && Current != MountRoot && CompCount < 256) {
        CHAR Name[NAME_MAX];

        Name[0] = '\0';
        if (Current->IOp && Current->IOp->GetName) {
            if (Current->IOp->GetName(Current, Name, sizeof(Name)) != 0 || Name[0] == '\0') {
                VfsInodeUnref(Current);
                return -1;
            }
        } else {
            VfsInodeUnref(Current);
            return -1;
        }

        Components[CompCount] = MemoryAllocate(StrLen(Name) + 1);
        if (!Components[CompCount]) {
            for (I = 0; I < CompCount; I++) {
                MemoryFree(Components[I]);
            }
            VfsInodeUnref(Current);
            return -1;
        }
        StrCpy(Components[CompCount], Name);
        CompCount++;

        if (VfsParent(Current, &Parent) != 0) {
            for (I = 0; I < CompCount; I++) {
                MemoryFree(Components[I]);
            }
            VfsInodeUnref(Current);
            return -1;
        }
        VfsInodeUnref(Current);
        Current = Parent;
    }

    if (Current != MountRoot) {
        for (I = 0; I < CompCount; I++) {
            MemoryFree(Components[I]);
        }
        if (Current) {
            VfsInodeUnref(Current);
        }
        return -1;
    }
    VfsInodeUnref(Current);

    Buffer[0] = '/';
    Pos = 1;
    for (I = CompCount - 1; I >= 0; I--) {
        INT NameLen = StrLen(Components[I]);
        if (Pos + NameLen + 1 >= MaxLen) {
            for (I = 0; I < CompCount; I++) {
                MemoryFree(Components[I]);
            }
            return -1;
        }
        if (Pos > 1) {
            Buffer[Pos++] = '/';
        }
        StrCpy(Buffer + Pos, Components[I]);
        Pos += NameLen;
        MemoryFree(Components[I]);
    }
    Buffer[Pos] = '\0';
    return 0;
}

INT VfsBuildPath(VfsInode *Inode, CHAR *Buffer, INT MaxLen) {
    DriveMount *M;
    CHAR RelPath[PATH_MAX];

    if (!Inode || !Buffer || MaxLen <= 0) {
        return -1;
    }

    if (VfsIsNamespaceRoot(Inode)) {
        StrCpy(Buffer, "/");
        return 0;
    }

    M = VfsFindDriveMountByDev(Inode->IDev);
    if (!M) {
        M = VfsFindDriveMountByRoot(Inode);
    }
    if (!M) {
        return -1;
    }

    if (VfsBuildRelPath(Inode, M->Root, RelPath, sizeof(RelPath)) != 0) {
        return -1;
    }

    SnPrintf(Buffer, (USIZE)MaxLen, "%s::%s", M->DriveName, RelPath);
    return 0;
}

NOPTR UpdateCurrentPath(NOPTR) {
    if (!RootInode || !CurrentDir) {
        StrCpy(CurrentPath, "/");
        CurrentDriveName[0] = '\0';
        return;
    }

    if (VfsIsNamespaceRoot(CurrentDir)) {
        StrCpy(CurrentPath, "/");
        CurrentDriveName[0] = '\0';
        return;
    }

    if (VfsBuildPath(CurrentDir, CurrentPath, PATH_MAX) != 0) {
        StrCpy(CurrentPath, "/");
        CurrentDriveName[0] = '\0';
        return;
    }

    {
        const CHAR *Sep = StrStr(CurrentPath, VFS_DRIVE_SEP);
        if (Sep) {
            USIZE Len = (USIZE)(Sep - CurrentPath);
            if (Len < sizeof(CurrentDriveName)) {
                StrnCpy(CurrentDriveName, CurrentPath, Len);
                CurrentDriveName[Len] = '\0';
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Public path API                                                            */
/* -------------------------------------------------------------------------- */

INT VfsWalk(VfsInode *Dir, const CHAR *Path, VfsInode **Result) {
    CHAR DriveName[DRIVE_NAME_MAX];
    CHAR RelPath[PATH_MAX];
    DriveMount *M;

    if (!Path || !Result) {
        return -1;
    }

    if (StrStr(Path, VFS_DRIVE_SEP) == NULLPTR && Path[0] != '/' &&
        !(Path[0] == ':' && Path[1] == ':') && StrChr(Path, '/') == NULLPTR) {
        M = VfsFindDriveMount(Path);
        if (M) {
            *Result = M->Root;
            VfsInodeRef(*Result);
            (void)Dir;
            return 0;
        }
    }

    if (VfsParsePath(Path, DriveName, RelPath, sizeof(RelPath)) != 0) {
        return -1;
    }

    if (DriveName[0] == '\0') {
        if (RelPath[0] == '\0' || (RelPath[0] == '/' && RelPath[1] == '\0')) {
            *Result = RootInode;
            VfsInodeRef(*Result);
            return 0;
        }
        return -1;
    }

    M = VfsFindDriveMount(DriveName);
    if (!M) {
        return -1;
    }

    if (Dir && !VfsIsNamespaceRoot(Dir) && Path[0] != '/' &&
        StrStr(Path, VFS_DRIVE_SEP) == NULLPTR &&
        !(Path[0] == ':' && Path[1] == ':')) {
        CHAR InnerPath[PATH_MAX];

        SnPrintf(InnerPath, sizeof(InnerPath), "/%s", Path);
        return VfsWalkWithin(Dir, InnerPath, Result);
    }

    return VfsWalkWithin(M->Root, RelPath, Result);
}

INT VfsResolvePath(VfsInode *BaseDir, const CHAR *Path,
                   VfsInode **Parent, CHAR *Name, BOOL FollowLast) {
    CHAR FullPath[PATH_MAX];
    CHAR DriveName[DRIVE_NAME_MAX];
    CHAR RelPath[PATH_MAX];
    const CHAR *LastSlash;
    CHAR ParentRel[PATH_MAX];
    DriveMount *M;

    (void)FollowLast;

    if (!Path || !Parent || !Name) {
        return -1;
    }

    if (Path[0] == '\0' || (Path[0] == '/' && Path[1] == '\0')) {
        *Parent = RootInode;
        VfsInodeRef(*Parent);
        Name[0] = '\0';
        return 0;
    }

    if (BaseDir && Path[0] != '/' && StrStr(Path, VFS_DRIVE_SEP) == NULLPTR &&
        !(Path[0] == ':' && Path[1] == ':')) {
        if (VfsBuildPath(BaseDir, FullPath, sizeof(FullPath)) == 0) {
            if (FullPath[0] != '\0' && StrCmp(FullPath, "/") != 0) {
                SnPrintf(FullPath, sizeof(FullPath), "%s/%s", FullPath, Path);
            } else if (CurrentDriveName[0] != '\0') {
                SnPrintf(FullPath, sizeof(FullPath), "%s::/%s",
                         CurrentDriveName, Path);
            } else {
                StrCpy(FullPath, Path);
            }
        } else {
            StrCpy(FullPath, Path);
        }
        Path = FullPath;
    }

    if (VfsParsePath(Path, DriveName, RelPath, sizeof(RelPath)) != 0) {
        return -1;
    }

    if (DriveName[0] == '\0') {
        return -1;
    }

    M = VfsFindDriveMount(DriveName);
    if (!M) {
        return -1;
    }

    LastSlash = StrrChr(RelPath, '/');
    if (!LastSlash) {
        *Parent = M->Root;
        VfsInodeRef(*Parent);
        StrCpy(Name, RelPath);
        return 0;
    }

    if (LastSlash == RelPath && RelPath[1] == '\0') {
        *Parent = M->Root;
        VfsInodeRef(*Parent);
        Name[0] = '\0';
        return 0;
    }

    if (LastSlash == RelPath) {
        *Parent = M->Root;
        VfsInodeRef(*Parent);
        StrCpy(Name, LastSlash + 1);
        return 0;
    }

    StrnCpy(ParentRel, RelPath, (USIZE)(LastSlash - RelPath));
    ParentRel[LastSlash - RelPath] = '\0';
    if (VfsWalkWithin(M->Root, ParentRel, Parent) != 0) {
        return -1;
    }

    StrCpy(Name, LastSlash + 1);
    return 0;
}

INT VfsWalkParent(VfsInode *Dir, const CHAR *Path, VfsInode **Parent, CHAR *Name) {
    CHAR DriveName[DRIVE_NAME_MAX];
    CHAR RelPath[PATH_MAX];
    const CHAR *LastSlash;
    CHAR ParentRel[PATH_MAX];
    DriveMount *M;

    (void)Dir;

    if (!Path || !Parent || !Name) {
        return -1;
    }

    if (VfsParsePath(Path, DriveName, RelPath, sizeof(RelPath)) != 0) {
        return -1;
    }

    if (DriveName[0] == '\0') {
        return -1;
    }

    M = VfsFindDriveMount(DriveName);
    if (!M) {
        return -1;
    }

    LastSlash = StrrChr(RelPath, '/');
    if (!LastSlash || LastSlash == RelPath) {
        *Parent = M->Root;
        VfsInodeRef(*Parent);
        StrCpy(Name, (LastSlash && LastSlash[1]) ? LastSlash + 1 : RelPath);
        if (Name[0] == '\0' && LastSlash == RelPath) {
            StrCpy(Name, RelPath + 1);
        }
        return 0;
    }

    StrnCpy(ParentRel, RelPath, (USIZE)(LastSlash - RelPath));
    ParentRel[LastSlash - RelPath] = '\0';
    if (VfsWalkWithin(M->Root, ParentRel, Parent) != 0) {
        return -1;
    }

    StrCpy(Name, LastSlash + 1);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Mount / register                                                           */
/* -------------------------------------------------------------------------- */

static const CHAR *DetectFilesystem(Drive *Dev) {
    UINT8 Sector[512];
    ExfatVbr *Exfat;

    if (!Dev || Dev->Read(Dev, 0, 1, Sector) != 0) {
        return NULLPTR;
    }

    /* FAT32 / FAT16 */
    Fat32Bpb *Bpb = (Fat32Bpb*)Sector;
    if (Bpb->BytesPerSector == 512 && 
        Bpb->SectorsPerCluster > 0 &&
        Bpb->FatSize32 > 0 &&
        Sector[510] == 0x55 && Sector[511] == 0xAA) {
        
        if (Bpb->FsType[0] == 'F' && MemCmp(Bpb->FsType, "FAT32", 5) == 0)
            return "Fat32";
        if (Bpb->FsType[0] == 'F' && MemCmp(Bpb->FsType, "FAT16", 5) == 0)
            return "Fat32";
    }

    /* exFAT */
    Exfat = (ExfatVbr*)Sector;
    if (MemCmp(Exfat->FsName, "EXFAT   ", 8) == 0 &&
        Sector[510] == 0x55 && Sector[511] == 0xAA) {
        return "exfat";
    }

    return NULLPTR;
}

NOPTR VfsInit(NOPTR) {
    FsList = NULLPTR;
    DriveMounts = NULLPTR;
    NextIno = 1;

    MemSet(&GNamespaceOps, 0, sizeof(GNamespaceOps));
    GNamespaceOps.Lookup = VfsNamespaceLookup;
    GNamespaceOps.ReadDir = VfsNamespaceReadDir;
    GNamespaceOps.GetName = VfsNamespaceGetName;

    RootInode = VfsAllocInode();
    if (!RootInode) {
        return;
    }

    RootInode->IMode = FT_DIR;
    RootInode->IOp = &GNamespaceOps;
    RootInode->ISize = 0;
    StrCpy(RootInode->IFsName, "vfs");

    CurrentDir = RootInode;
    VfsInodeRef(CurrentDir);
    CurrentPath[0] = '/';
    CurrentPath[1] = '\0';
    CurrentDriveName[0] = '\0';
}

INT VfsRegisterFs(FileSystem *Fs) {
    if (!Fs) {
        return -1;
    }
    Fs->Next = FsList;
    FsList = Fs;
    return 0;
}

INT VfsMountDrive(const CHAR *FsName, Drive *Dev, VfsInode **RootOut) {
    FileSystem *Fs;
    const CHAR *RealFsName;
    DriveMount *M;
    VfsInode *FsRoot;

    if (!Dev) {
        return -1;
    }

    if (VfsFindDriveMount(Dev->Name)) {
        ConsolePrint("Drive '%s' is already mounted\n", Dev->Name);
        return -1;
    }

    if (!FsName || FsName[0] == '\0') {
        RealFsName = DetectFilesystem(Dev);
        if (!RealFsName) {
            ConsolePrint("Cannot detect filesystem on %s\n", Dev->Name);
            return -1;
        }
    } else {
        RealFsName = FsName;
    }

    for (Fs = FsList; Fs; Fs = Fs->Next) {
        if (!StrCaseEq(Fs->Name, RealFsName)) {
            continue;
        }

        if (Fs->Mount(Dev, &FsRoot) != 0) {
            return -1;
        }

        M = (DriveMount*)MemoryAllocate(sizeof(DriveMount));
        if (!M) {
            if (Fs->Unmount) {
                Fs->Unmount(FsRoot);
            }
            VfsInodeUnref(FsRoot);
            return -1;
        }

        StrCpy(M->DriveName, Dev->Name);
        StrCpy(M->FsName, Fs->Name);
        M->Dev = Dev;
        M->Root = FsRoot;
        VfsInodeRef(M->Root);
        M->Next = DriveMounts;
        DriveMounts = M;

        if (RootOut) {
            *RootOut = FsRoot;
        }
        return 0;
    }

    ConsolePrint("Filesystem %s not found\n", FsName ? FsName : RealFsName);
    return -1;
}

INT VfsMount(const CHAR *FsName, Drive *Dev, VfsInode **Root) {
    return VfsMountDrive(FsName, Dev, Root);
}

INT VfsUnmountDrive(const CHAR *DriveName) {
    DriveMount *M;
    DriveMount **Prev;

    M = VfsFindDriveMount(DriveName);
    if (!M) {
        return -1;
    }

    if (M->Root->IOp && M->Root->IOp->Unmount) {
        M->Root->IOp->Unmount(M->Root);
    }
    VfsSync(M->Root);
    VfsInodeUnref(M->Root);

    for (Prev = &DriveMounts; *Prev; Prev = &(*Prev)->Next) {
        if (*Prev == M) {
            *Prev = M->Next;
            break;
        }
    }

    if (CurrentDir && !VfsIsNamespaceRoot(CurrentDir)) {
        if (VfsFindDriveMountByDev(CurrentDir->IDev) == NULLPTR) {
            VfsInodeUnref(CurrentDir);
            CurrentDir = RootInode;
            VfsInodeRef(CurrentDir);
            UpdateCurrentPath();
        }
    }

    MemoryFree(M);
    return 0;
}

INT VfsUnmount(VfsInode *Root) {
    DriveMount *M;

    if (!Root) {
        return -1;
    }

    for (M = DriveMounts; M; M = M->Next) {
        if (M->Root == Root) {
            return VfsUnmountDrive(M->DriveName);
        }
    }
    return -1;
}

INT VfsListMountedDrives(CHAR *Buf, USIZE BufSize) {
    DriveMount *M;
    USIZE Pos;

    if (!Buf || BufSize == 0) {
        return -1;
    }

    Buf[0] = '\0';
    Pos = 0;
    for (M = DriveMounts; M; M = M->Next) {
        INT Written = SnPrintf(Buf + Pos, BufSize - Pos, "%s%s(%s)",
                                 Pos ? " " : "", M->DriveName, M->FsName);
        if (Written <= 0 || (USIZE)Written >= BufSize - Pos) {
            break;
        }
        Pos += (USIZE)Written;
    }
    return (INT)Pos;
}

/* -------------------------------------------------------------------------- */
/* File operations                                                            */
/* -------------------------------------------------------------------------- */

INT VfsLookup(VfsInode *Dir, const CHAR *Name, VfsInode **Result) {
    INT Ret;

    if (!Dir || !Name || !Result) {
        return -1;
    }
    if (Dir->IMode != FT_DIR) {
        return -1;
    }
    if (!Dir->IOp || !Dir->IOp->Lookup) {
        return -1;
    }

    Ret = Dir->IOp->Lookup(Dir, Name, Result);
    if (Ret == 0 && *Result) {
        VfsInodeRef(*Result);
    }
    return Ret;
}

INT VfsOpen(VfsInode *Dir, const CHAR *Path, UINT32 Flags, VfsFile **File) {
    VfsInode *Inode;
    VfsInode *Parent;
    CHAR Name[NAME_MAX];

    if (!Path || !File) {
        return -1;
    }

    if (VfsWalk(Dir, Path, &Inode) == 0) {
        (void)Flags;
    } else {
        if (!(Flags & O_CREAT)) {
            return -1;
        }
        if (VfsWalkParent(Dir, Path, &Parent, Name) != 0) {
            return -1;
        }
        if (!Parent->IOp || !Parent->IOp->Create) {
            VfsInodeUnref(Parent);
            return -1;
        }
        if (Parent->IOp->Create(Parent, Name, FT_REG_FILE, &Inode) != 0) {
            VfsInodeUnref(Parent);
            return -1;
        }
        VfsInodeUnref(Parent);
    }

    VfsFile *F = (VfsFile*)MemoryAllocate(sizeof(VfsFile));
    if (!F) {
        VfsInodeUnref(Inode);
        return -1;
    }

    F->FInode = Inode;
    F->FPos = (Flags & O_APPEND) ? Inode->ISize : 0;
    F->FFlags = Flags;
    F->FPrivate = NULLPTR;
    *File = F;
    return 0;
}

INT VfsClose(VfsFile *File) {
    if (!File) {
        return -1;
    }

    if (File->FInode && File->FInode->IDirty) {
        VfsSync(File->FInode);
    }
    if (File->FInode) {
        VfsInodeUnref(File->FInode);
    }
    MemoryFree(File);
    return 0;
}

INT VfsRead(VfsFile *File, NOPTR *Buf, UINT32 Size, UINT32 *Read) {
    if (!File || !File->FInode || !File->FInode->IFop || !File->FInode->IFop->Read) {
        return -1;
    }

    INT Ret = File->FInode->IFop->Read(File->FInode, File->FPos, Buf, Size, Read);
    if (Ret == 0 && Read) {
        File->FPos += *Read;
    }
    return Ret;
}

INT VfsWrite(VfsFile *File, const NOPTR *Buf, UINT32 Size, UINT32 *Written) {
    if (!File || !File->FInode || !File->FInode->IFop || !File->FInode->IFop->Write) {
        return -1;
    }

    INT Ret = File->FInode->IFop->Write(File->FInode, File->FPos, Buf, Size, Written);
    if (Ret == 0) {
        if (Written) {
            File->FPos += *Written;
        }
        if (File->FPos > File->FInode->ISize) {
            File->FInode->ISize = File->FPos;
        }
        File->FInode->IDirty = 1;
    }
    return Ret;
}

INT VfsSeek(VfsFile *File, UINT64 Offset, INT Whence) {
    if (!File || !File->FInode) {
        return -1;
    }

    switch (Whence) {
        case 0:
            File->FPos = Offset;
            break;
        case 1:
            File->FPos += Offset;
            break;
        case 2:
            File->FPos = File->FInode->ISize + Offset;
            break;
        default:
            return -1;
    }
    return 0;
}

INT VfsMkdir(VfsInode *Dir, const CHAR *Path, UINT32 Mode, VfsInode **Result) {
    VfsInode *Parent;
    CHAR Name[NAME_MAX];

    if (!Path) {
        return -1;
    }
    if (VfsResolvePath(Dir, Path, &Parent, Name, FALSE) != 0) {
        return -1;
    }
    if (Name[0] == '\0' || Parent->IMode != FT_DIR || !Parent->IOp || !Parent->IOp->Mkdir) {
        VfsInodeUnref(Parent);
        return -1;
    }

    INT Ret = Parent->IOp->Mkdir(Parent, Name, Mode ? Mode : FT_DIR, Result);
    VfsInodeUnref(Parent);
    return Ret;
}

INT VfsRmdir(VfsInode *Dir, const CHAR *Path) {
    VfsInode *Parent;
    CHAR Name[NAME_MAX];

    if (!Path) {
        return -1;
    }
    if (VfsResolvePath(Dir, Path, &Parent, Name, FALSE) != 0) {
        return -1;
    }
    if (Name[0] == '\0' || !Parent->IOp || !Parent->IOp->Rmdir) {
        VfsInodeUnref(Parent);
        return -1;
    }

    INT Ret = Parent->IOp->Rmdir(Parent, Name);
    VfsInodeUnref(Parent);
    return Ret;
}

INT VfsReadDir(VfsInode *Dir, UINT64 *Pos, CHAR *Name, UINT32 *NameLen, UINT32 *Type) {
    if (!Dir || !Pos || !Name || !NameLen || !Type) {
        return -1;
    }
    if (Dir->IMode != FT_DIR) {
        return -1;
    }
    if (!Dir->IOp || !Dir->IOp->ReadDir) {
        return -1;
    }
    return Dir->IOp->ReadDir(Dir, Pos, Name, NameLen, Type);
}

INT VfsCreate(VfsInode *Dir, const CHAR *Path, UINT32 Mode, VfsInode **Result) {
    VfsInode *Parent;
    CHAR Name[NAME_MAX];

    if (!Path) {
        return -1;
    }
    if (VfsResolvePath(Dir, Path, &Parent, Name, FALSE) != 0) {
        return -1;
    }
    if (Name[0] == '\0' || !Parent->IOp || !Parent->IOp->Create) {
        VfsInodeUnref(Parent);
        return -1;
    }

    INT Ret = Parent->IOp->Create(Parent, Name, Mode, Result);
    VfsInodeUnref(Parent);
    return Ret;
}

INT VfsUnlink(VfsInode *Dir, const CHAR *Path) {
    VfsInode *Parent;
    CHAR Name[NAME_MAX];

    if (!Path) {
        return -1;
    }
    if (VfsResolvePath(Dir, Path, &Parent, Name, FALSE) != 0) {
        return -1;
    }
    if (Name[0] == '\0' || !Parent->IOp || !Parent->IOp->Unlink) {
        VfsInodeUnref(Parent);
        return -1;
    }

    INT Ret = Parent->IOp->Unlink(Parent, Name);
    VfsInodeUnref(Parent);
    return Ret;
}

INT VfsRename(VfsInode *OldDir, const CHAR *OldPath,
              VfsInode *NewDir, const CHAR *NewPath) {
    VfsInode *OldParent;
    VfsInode *NewParent;
    CHAR OldName[NAME_MAX];
    CHAR NewName[NAME_MAX];

    if (!OldPath || !NewPath) {
        return -1;
    }
    if (VfsResolvePath(OldDir, OldPath, &OldParent, OldName, FALSE) != 0) {
        return -1;
    }
    if (VfsResolvePath(NewDir, NewPath, &NewParent, NewName, FALSE) != 0) {
        VfsInodeUnref(OldParent);
        return -1;
    }
    if (OldName[0] == '\0' || NewName[0] == '\0' || !OldParent->IOp || !OldParent->IOp->Rename) {
        VfsInodeUnref(OldParent);
        VfsInodeUnref(NewParent);
        return -1;
    }

    INT Ret = OldParent->IOp->Rename(OldParent, OldName, NewParent, NewName);
    VfsInodeUnref(OldParent);
    VfsInodeUnref(NewParent);
    return Ret;
}

INT VfsStat(VfsInode *Inode, VfsStatS *Stat) {
    if (!Inode || !Stat) {
        return -1;
    }

    Stat->StMode = Inode->IMode;
    Stat->StUid = Inode->IUid;
    Stat->StGid = Inode->IGid;
    Stat->StSize = Inode->ISize;
    Stat->StCtime = Inode->ICtime;
    Stat->StMtime = Inode->IMtime;
    Stat->StAtime = Inode->IAtime;
    Stat->StIno = Inode->IIno;
    Stat->StNlink = Inode->INlink;
    return 0;
}

INT VfsChmod(VfsInode *Inode, UINT32 Mode) {
    if (!Inode || !Inode->IOp || !Inode->IOp->Chmod) {
        return -1;
    }
    return Inode->IOp->Chmod(Inode, Mode);
}

INT VfsSync(VfsInode *Inode) {
    if (!Inode || !Inode->IFop || !Inode->IFop->Sync) {
        return -1;
    }
    INT Ret = Inode->IFop->Sync(Inode);
    if (Ret == 0) {
        Inode->IDirty = 0;
    }
    return Ret;
}

INT VfsChown(VfsInode *Inode, UINT32 Uid, UINT32 Gid) {
    if (!Inode) {
        return -1;
    }
    Inode->IUid = Uid;
    Inode->IGid = Gid;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Inode management                                                           */
/* -------------------------------------------------------------------------- */

VfsInode *VfsAllocInode(NOPTR) {
    VfsInode *Inode = (VfsInode*)MemoryAllocate(sizeof(VfsInode));
    if (Inode) {
        MemSet(Inode, 0, sizeof(VfsInode));
        Inode->IRefCount = 1;
        Inode->IIno = NextIno++;
    }
    return Inode;
}

NOPTR VfsFreeInode(VfsInode *Inode) {
    VfsInodeUnref(Inode);
}

NOPTR VfsInodeRef(VfsInode *Inode) {
    if (Inode) {
        Inode->IRefCount++;
    }
}

NOPTR VfsInodeUnref(VfsInode *Inode) {
    if (!Inode) {
        return;
    }
    if (Inode->IRefCount == 0) {
        return;
    }
    if (--Inode->IRefCount == 0) {
        if (Inode->IPrivate) {
            MemoryFree(Inode->IPrivate);
        }
        MemoryFree(Inode);
    }
}

INT VfsParent(VfsInode *Inode, VfsInode **Parent) {
    if (!Inode || !Parent) {
        return -1;
    }
    if (VfsIsNamespaceRoot(Inode)) {
        *Parent = RootInode;
        VfsInodeRef(*Parent);
        return 0;
    }
    if (VfsFindDriveMountByRoot(Inode)) {
        *Parent = RootInode;
        VfsInodeRef(*Parent);
        return 0;
    }
    if (Inode->IOp && Inode->IOp->Parent) {
        INT Ret = Inode->IOp->Parent(Inode, Parent);
        if (Ret == 0 && *Parent) {
            VfsInodeRef(*Parent);
        }
        return Ret;
    }
    return -1;
}

INT VfsGetPathDepth(VfsInode *Inode) {
    INT Depth = 0;
    VfsInode *Current = Inode;
    VfsInode *Parent = NULLPTR;

    while (Current && !VfsIsNamespaceRoot(Current) && Depth < 1000) {
        if (VfsFindDriveMountByRoot(Current)) {
            break;
        }
        if (VfsParent(Current, &Parent) != 0) {
            break;
        }
        if (Parent == Current) {
            VfsInodeUnref(Parent);
            break;
        }
        VfsInodeUnref(Current);
        Current = Parent;
        Depth++;
    }
    if (Current && Current != Inode) {
        VfsInodeUnref(Current);
    }
    return Depth;
}

INT BuildCurrentPath(VfsInode *Dir, const CHAR *Component, CHAR *Out) {
    CHAR BasePath[PATH_MAX];

    if (!Dir || !Out) {
        return -1;
    }
    if (VfsBuildPath(Dir, BasePath, PATH_MAX) != 0) {
        return -1;
    }
    if (!Component || Component[0] == '\0') {
        StrCpy(Out, BasePath);
        return 0;
    }

    if (StrCmp(BasePath, "/") == 0) {
        SnPrintf(Out, PATH_MAX, "%s", Component);
        return 0;
    }

    SnPrintf(Out, PATH_MAX, "%s/%s", BasePath, Component);
    return 0;
}

INT VfsMountPoint(const CHAR *Path, VfsInode *Inode) {
    (void)Path;
    (void)Inode;
    return -1;
}

INT VfsGetMountPoints(const CHAR *Path, MountEntry *Entries, INT MaxEntries) {
    DriveMount *M;
    INT Count;

    if (!Entries || MaxEntries <= 0) {
        return -1;
    }

    Count = 0;
    for (M = DriveMounts; M && Count < MaxEntries; M = M->Next) {
        SnPrintf(Entries[Count].Name, sizeof(Entries[Count].Name),
                 "%s::/", M->DriveName);
        Entries[Count].Inode = M->Root;
        Entries[Count].Type = FT_DIR;
        Count++;
    }

    (void)Path;
    return Count;
}

INT VfsCd(VfsInode **Dir, const CHAR *Path) {
    VfsInode *NewDir;

    if (!Dir || !Path) {
        return -1;
    }

    if (VfsWalk(*Dir, Path, &NewDir) != 0) {
        return -1;
    }
    if (NewDir->IMode != FT_DIR) {
        VfsInodeUnref(NewDir);
        return -1;
    }

    VfsInodeUnref(*Dir);
    *Dir = NewDir;
    UpdateCurrentPath();
    return 0;
}
