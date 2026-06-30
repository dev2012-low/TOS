#include <Fs/Fat32.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Console.h>
#include <Time/Clock.h>
#include <Time/Rtc.h>
#include <Time/Timer.h>

//==================== INTERNAL FUNCTIONS ====================

static INT Fat32ReadSectors(Fat32 *Fat, UINT64 Sector, UINT32 Count, NOPTR *Buf) {
    return Fat->Dev->Read(Fat->Dev, Sector, Count, Buf);
}

static INT Fat32WriteSectors(Fat32 *Fat, UINT64 Sector, UINT32 Count, const NOPTR *Buf) {
    return Fat->Dev->Write(Fat->Dev, Sector, Count, Buf);
}

static UINT32 Fat32NextCluster(Fat32 *Fat, UINT32 Cluster) {
    if (Cluster < 2 || Cluster >= Fat->FatEntries) return FAT32_EOC;
    UINT32 Next = Fat->FatCache[Cluster];
    if (Next >= FAT32_EOC) return FAT32_EOC;
    if (Next == FAT32_FREE || Next == FAT32_BAD) return FAT32_EOC;
    return Next;
}

static INT Fat32ReadCluster(Fat32 *Fat, UINT32 Cluster, UINT8 *Buffer) {
    if (Cluster < 2) return -1;
    UINT64 Sector = Fat->DataStart + (UINT64)(Cluster - 2) * Fat->SectorsPerCluster;
    return Fat32ReadSectors(Fat, Sector, Fat->SectorsPerCluster, Buffer);
}

static INT Fat32WriteCluster(Fat32 *Fat, UINT32 Cluster, const UINT8 *Buffer) {
    if (Cluster < 2) return -1;
    UINT64 Sector = Fat->DataStart + (UINT64)(Cluster - 2) * Fat->SectorsPerCluster;
    return Fat32WriteSectors(Fat, Sector, Fat->SectorsPerCluster, Buffer);
}

static UINT32 Fat32AllocCluster(Fat32 *Fat) {
    UINT32 Start = Fat->FsInfo.NextFree;
    if (Start < 2 || Start >= Fat->FatEntries) Start = 2;
    
    for (UINT32 I = 0; I < Fat->FatEntries - 2; I++) {
        UINT32 Cluster = Start + I;
        if (Cluster >= Fat->FatEntries) Cluster = 2 + (Cluster - Fat->FatEntries);
        
        if (Fat->FatCache[Cluster] == FAT32_FREE) {
            Fat->FatCache[Cluster] = FAT32_EOC;
            Fat->FsInfo.FreeCount--;
            Fat->FsInfo.NextFree = Cluster + 1;
            if (Fat->FsInfo.NextFree >= Fat->FatEntries) Fat->FsInfo.NextFree = 2;
            Fat->Dirty = 1;
            return Cluster;
        }
    }
    return 0;
}

static NOPTR Fat32FreeClusterChain(Fat32 *Fat, UINT32 Cluster) {
    while (Cluster >= 2 && Cluster < Fat->FatEntries) {
        UINT32 Next = Fat->FatCache[Cluster];
        Fat->FatCache[Cluster] = FAT32_FREE;
        Fat->FsInfo.FreeCount++;
        Cluster = (Next < FAT32_EOC) ? Next : 0;
        Fat->Dirty = 1;
    }
}

static INT Fat32AllocClusters(Fat32 *Fat, UINT32 Count, UINT32 *First) {
    *First = 0;
    UINT32 Prev = 0;
    
    for (UINT32 I = 0; I < Count; I++) {
        UINT32 Cluster = Fat32AllocCluster(Fat);
        if (!Cluster) {
            if (*First) Fat32FreeClusterChain(Fat, *First);
            return 0;
        }
        
        if (!*First) *First = Cluster;
        if (Prev) Fat->FatCache[Prev] = Cluster;
        Prev = Cluster;
    }
    return Count;
}

static NOPTR Fat32UpdateFsInfo(Fat32 *Fat) {
    if (!Fat->Dirty) return;
    
    UINT8 Sector[512];
    MemSet(Sector, 0, 512);
    
    Fat32FsInfo *FsInfo = (Fat32FsInfo*)Sector;
    FsInfo->LeadSignature = 0x41615252;
    FsInfo->StructSignature = 0x61417272;
    FsInfo->FreeCount = Fat->FsInfo.FreeCount;
    FsInfo->NextFree = Fat->FsInfo.NextFree;
    FsInfo->TrailSignature = 0xAA550000;
    
    Fat32WriteSectors(Fat, Fat->FsinfoSector, 1, Sector);
    Fat->Dirty = 0;
}

static UINT8 Fat32Checksum(const UINT8 *Name) {
    UINT8 Sum = 0;
    for (INT I = 0; I < 11; I++) {
        Sum = ((Sum & 1) ? 0x80 : 0) + (Sum >> 1) + Name[I];
    }
    return Sum;
}

static NOPTR Fat32NameFromEntry(CHAR *Dest, const UINT8 *Name) {
    INT I, J = 0;
    
    // Copy name
    for (I = 0; I < 8 && Name[I] != ' '; I++) {
        CHAR C = Name[I];
        Dest[J++] = (C >= 'A' && C <= 'Z') ? C + 32 : C;
    }
    
    // Copy extension if exists
    if (Name[8] != ' ') {
        Dest[J++] = '.';
        for (I = 8; I < 11 && Name[I] != ' '; I++) {
            CHAR C = Name[I];
            Dest[J++] = (C >= 'A' && C <= 'Z') ? C + 32 : C;
        }
    }
    
    Dest[J] = '\0';
}

static NOPTR Fat32NameToEntry(UINT8 *Dest, const CHAR *Name, UINT8 *Checksum) {
    // Clear with spaces
    for (INT I = 0; I < 11; I++) Dest[I] = ' ';
    
    INT I = 0;
    INT J = 0;
    
    // Find dot
    while (Name[I] && Name[I] != '.' && I < 8) {
        CHAR C = Name[I];
        Dest[J++] = (C >= 'a' && C <= 'z') ? C - 32 : C;
        I++;
    }
    
    // Skip dot
    if (Name[I] == '.') I++;
    
    // Extension
    J = 8;
    while (Name[I] && J < 11) {
        CHAR C = Name[I];
        Dest[J++] = (C >= 'a' && C <= 'z') ? C - 32 : C;
        I++;
    }
    
    if (Checksum) *Checksum = Fat32Checksum(Dest);
}

static INT Fat32CreateLfnEntries(Fat32 *Fat, UINT8 *ClusterBuf, INT *Offset,
                                  const CHAR *Name, UINT8 Checksum, INT IsLast) {
    USIZE NameLen = StrLen(Name);
    INT NumEntries = (NameLen + 12) / 13;
    
    for (INT I = NumEntries - 1; I >= 0; I--) {
        Fat32LfnEntry *Lfn = (Fat32LfnEntry*)(ClusterBuf + *Offset);
        MemSet(Lfn, 0, sizeof(Fat32LfnEntry));
        
        Lfn->Order = (I + 1) | (IsLast && I == NumEntries - 1 ? 0x40 : 0);
        Lfn->Attr = FAT_ATTR_LFN;
        Lfn->Type = 0;
        Lfn->Checksum = Checksum;
        Lfn->FirstCluster = 0;
        
        INT NamePos = I * 13;
        for (INT J = 0; J < 5 && NamePos < (INT)NameLen; J++, NamePos++)
            Lfn->Name1[J] = Name[NamePos];
        for (INT J = 0; J < 6 && NamePos < (INT)NameLen; J++, NamePos++)
            Lfn->Name2[J] = Name[NamePos];
        for (INT J = 0; J < 2 && NamePos < (INT)NameLen; J++, NamePos++)
            Lfn->Name3[J] = Name[NamePos];
        
        *Offset += 32;
    }
    
    return NumEntries;
}

//==================== VFS OPERATIONS ====================

static INT Fat32Lookup(VfsInode *Dir, const CHAR *Name, VfsInode **Result) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Dir->IPrivate;
    Fat32 *Fat = Priv->Fat;
    
    UINT32 Cluster = Priv->FirstCluster;
    UINT8 Buf[Fat->BytesPerCluster];
    UINT32 EntryIndex = 0;
    
    while (Cluster && Cluster < FAT32_EOC) {
        if (Fat32ReadCluster(Fat, Cluster, Buf) != 0) return -1;
        
        Fat32DirEntry *Entry = (Fat32DirEntry*)Buf;
        for (UINT32 I = 0; I < Fat->BytesPerCluster / 32; I++, EntryIndex++) {
            if (Entry->Name[0] == 0) return -1;
            if (Entry->Name[0] == 0xE5) {
                Entry++;
                continue;
            }
            
            if (Entry->Attr == FAT_ATTR_LFN) {
                Entry++;
                continue;
            }
            
            CHAR EntryName[256];
            Fat32NameFromEntry(EntryName, Entry->Name);
            
            if (StrCmp(EntryName, Name) == 0) {
                VfsInode *Inode = VfsAllocInode();
                Fat32InodePrivate *NewPriv = (Fat32InodePrivate*)MemoryAllocate(sizeof(Fat32InodePrivate));
                
                UINT32 FirstCluster = (Entry->FirstClusterHi << 16) | Entry->FirstClusterLo;
                
                NewPriv->Fat = Fat;
                NewPriv->FirstCluster = FirstCluster;
                NewPriv->DirCluster = Cluster;
                NewPriv->DirEntry = EntryIndex;
                NewPriv->EntryOffset = I * 32;
                NewPriv->ParentCluster = Priv->FirstCluster;
                
                Inode->IMode = (Entry->Attr & FAT_ATTR_DIRECTORY) ? FT_DIR : FT_REG_FILE;
                Inode->ISize = Entry->FileSize;
                Inode->IPrivate = NewPriv;
                Inode->IOp = Dir->IOp;
                Inode->IFop = Dir->IFop;
                
                *Result = Inode;
                return 0;
            }
            
            Entry++;
        }
        
        Cluster = Fat32NextCluster(Fat, Cluster);
    }
    
    return -1;
}

static INT Fat32Read(VfsInode *Inode, UINT64 Offset, NOPTR *Buf, UINT32 Size, UINT32 *Read) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Inode->IPrivate;
    Fat32 *Fat = Priv->Fat;
    
    if (Offset >= Inode->ISize) {
        *Read = 0;
        return 0;
    }
    
    UINT32 ToRead = Size;
    if (Offset + ToRead > Inode->ISize)
        ToRead = Inode->ISize - Offset;
    
    UINT8 *Buffer = (UINT8*)Buf;
    UINT32 Done = 0;
    UINT32 Cluster = Priv->FirstCluster;
    UINT32 ClusterSize = Fat->BytesPerCluster;
    
    // Skip to offset
    UINT32 SkipClusters = Offset / ClusterSize;
    for (UINT32 I = 0; I < SkipClusters && Cluster && Cluster < FAT32_EOC; I++)
        Cluster = Fat32NextCluster(Fat, Cluster);
    
    if (!Cluster || Cluster >= FAT32_EOC) return -1;
    
    UINT32 ClusterOffset = Offset % ClusterSize;
    UINT8 *Temp = (UINT8*)MemoryAllocate(ClusterSize);
    if (!Temp) return -1;
    
    while (Done < ToRead && Cluster && Cluster < FAT32_EOC) {
        if (Fat32ReadCluster(Fat, Cluster, Temp) != 0) {
            MemoryFree(Temp);
            return -1;
        }
        
        UINT32 Copy = ClusterSize - ClusterOffset;
        if (Copy > ToRead - Done) Copy = ToRead - Done;
        
        MemCpy(Buffer + Done, Temp + ClusterOffset, Copy);
        
        Done += Copy;
        ClusterOffset = 0;
        Cluster = Fat32NextCluster(Fat, Cluster);
    }
    
    MemoryFree(Temp);
    *Read = Done;
    return 0;
}

static INT Fat32Write(VfsInode *Inode, UINT64 Offset, const NOPTR *Buf, UINT32 Size, UINT32 *Written) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Inode->IPrivate;
    Fat32 *Fat = Priv->Fat;
    
    if (Size == 0) {
        *Written = 0;
        return 0;
    }
    
    UINT32 ClusterSize = Fat->BytesPerCluster;
    const UINT8 *Buffer = (const UINT8*)Buf;
    UINT32 ToWrite = Size;
    UINT32 Done = 0;
    
    // Calculate clusters needed
    UINT32 FirstClusterIdx = Offset / ClusterSize;
    UINT32 LastClusterIdx = (Offset + Size - 1) / ClusterSize;
    UINT32 ClustersNeeded = LastClusterIdx - FirstClusterIdx + 1;
    
    // Get or allocate cluster chain
    UINT32 Cluster = Priv->FirstCluster;
    
    // Skip to needed cluster
    for (UINT32 I = 0; I < FirstClusterIdx; I++) {
        if (!Cluster || Cluster >= FAT32_EOC) {
            if (!Priv->FirstCluster) {
                if (!Fat32AllocClusters(Fat, 1, &Priv->FirstCluster))
                    return -1;
                Cluster = Priv->FirstCluster;
            } else {
                return -1;
            }
        } else {
            Cluster = Fat32NextCluster(Fat, Cluster);
        }
    }
    
    // Ensure we have enough clusters
    UINT32 *Clusters = (UINT32*)MemoryAllocate(ClustersNeeded * sizeof(UINT32));
    if (!Clusters) return -1;
    
    UINT32 C = 0;
    while (C < ClustersNeeded) {
        if (!Cluster || Cluster >= FAT32_EOC) {
            UINT32 NewCluster = Fat32AllocCluster(Fat);
            if (!NewCluster) {
                MemoryFree(Clusters);
                return -1;
            }
            
            if (C == 0 && !Priv->FirstCluster) {
                Priv->FirstCluster = NewCluster;
            } else if (C > 0) {
                Fat->FatCache[Clusters[C-1]] = NewCluster;
            }
            
            Clusters[C] = NewCluster;
            Cluster = NewCluster;
        } else {
            Clusters[C] = Cluster;
            Cluster = Fat32NextCluster(Fat, Cluster);
        }
        C++;
    }
    
    // Write data
    for (UINT32 I = 0; I < ClustersNeeded; I++) {
        UINT32 Current = Clusters[I];
        UINT32 ClusterOffset = (I == 0) ? (Offset % ClusterSize) : 0;
        UINT32 WriteSize = ClusterSize - ClusterOffset;
        if (WriteSize > ToWrite - Done) WriteSize = ToWrite - Done;
        
        if (WriteSize == ClusterSize) {
            Fat32WriteCluster(Fat, Current, Buffer + Done);
        } else {
            UINT8 *Temp = (UINT8*)MemoryAllocate(ClusterSize);
            if (!Temp) {
                MemoryFree(Clusters);
                return -1;
            }
            Fat32ReadCluster(Fat, Current, Temp);
            MemCpy(Temp + ClusterOffset, Buffer + Done, WriteSize);
            Fat32WriteCluster(Fat, Current, Temp);
            MemoryFree(Temp);
        }
        
        Done += WriteSize;
    }
    
    MemoryFree(Clusters);
    
    // Update file size
    if (Offset + Done > Inode->ISize) {
        Inode->ISize = Offset + Done;
        
        // Update directory entry
        UINT8 *DirBuf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
        if (!DirBuf) return -1;
        
        Fat32ReadCluster(Fat, Priv->DirCluster, DirBuf);
        
        Fat32DirEntry *Entry = (Fat32DirEntry*)(DirBuf + Priv->EntryOffset);
        Entry->FileSize = Inode->ISize;
        
        // Update time
        DateTime Dt;
        RtcReadTime(&Dt);
        
        Entry->WrtDate = FAT_DATE(Dt.Year, Dt.Month, Dt.Day);
        Entry->WrtTime = FAT_TIME(Dt.Hour, Dt.Minute, Dt.Second);
        
        Fat32WriteCluster(Fat, Priv->DirCluster, DirBuf);
        MemoryFree(DirBuf);
    }
    
    *Written = Done;
    return 0;
}

static INT Fat32ReadDir(VfsInode *Dir, UINT64 *Pos, CHAR *Name, UINT32 *NameLen, UINT32 *Type) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Dir->IPrivate;
    Fat32 *Fat = Priv->Fat;
    
    UINT32 EntryIndex = *Pos;
    UINT32 EntriesPerCluster = Fat->BytesPerCluster / 32;
    UINT32 ClusterIndex = EntryIndex / EntriesPerCluster;
    UINT32 ClusterOffset = EntryIndex % EntriesPerCluster;
    
    UINT32 Cluster = Priv->FirstCluster;
    for (UINT32 I = 0; I < ClusterIndex; I++) {
        if (!Cluster || Cluster >= FAT32_EOC) return -1;
        Cluster = Fat32NextCluster(Fat, Cluster);
    }
    
    if (!Cluster || Cluster >= FAT32_EOC) return -1;
    
    UINT8 *Buf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!Buf) return -1;
    
    if (Fat32ReadCluster(Fat, Cluster, Buf) != 0) {
        MemoryFree(Buf);
        return -1;
    }
    
    Fat32DirEntry *Entry = (Fat32DirEntry*)(Buf + ClusterOffset * 32);
    
    // Skip deleted and LFN entries
    while (Entry->Name[0] == 0xE5 || Entry->Attr == FAT_ATTR_LFN) {
        EntryIndex++;
        ClusterOffset++;
        
        if (ClusterOffset >= EntriesPerCluster) {
            Cluster = Fat32NextCluster(Fat, Cluster);
            if (!Cluster || Cluster >= FAT32_EOC) {
                MemoryFree(Buf);
                return -1;
            }
            if (Fat32ReadCluster(Fat, Cluster, Buf) != 0) {
                MemoryFree(Buf);
                return -1;
            }
            ClusterOffset = 0;
            Entry = (Fat32DirEntry*)Buf;
        } else {
            Entry++;
        }
    }
    
    if (Entry->Name[0] == 0) {
        MemoryFree(Buf);
        return -1;
    }
    
    Fat32NameFromEntry(Name, Entry->Name);
    *NameLen = StrLen(Name);
    *Type = (Entry->Attr & FAT_ATTR_DIRECTORY) ? FT_DIR : FT_REG_FILE;
    *Pos = EntryIndex + 1;
    
    MemoryFree(Buf);
    return 0;
}

static INT Fat32Create(VfsInode *Dir, const CHAR *Name, UINT32 Mode, VfsInode **Result) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Dir->IPrivate;
    Fat32 *Fat = Priv->Fat;
    
    // Check if already exists
    VfsInode *Existing;
    if (Fat32Lookup(Dir, Name, &Existing) == 0) {
        VfsInodeUnref(Existing);
        return -1;
    }
    
    // Find free entry in directory
    UINT32 Cluster = Priv->FirstCluster;
    UINT8 *Buf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!Buf) return -1;
    
    UINT32 EntryIndex = 0;
    UINT32 TargetCluster = 0;
    UINT32 TargetOffset = 0;
    INT FoundFree = 0;
    
    while (Cluster && Cluster < FAT32_EOC) {
        if (Fat32ReadCluster(Fat, Cluster, Buf) != 0) {
            MemoryFree(Buf);
            return -1;
        }
        
        for (UINT32 I = 0; I < Fat->BytesPerCluster / 32; I++) {
            Fat32DirEntry *Entry = (Fat32DirEntry*)Buf + I;
            
            if (Entry->Name[0] == 0 || Entry->Name[0] == 0xE5) {
                TargetCluster = Cluster;
                TargetOffset = I * 32;
                FoundFree = 1;
                break;
            }
            EntryIndex++;
        }
        if (FoundFree) break;
        
        Cluster = Fat32NextCluster(Fat, Cluster);
    }
    
    if (!FoundFree) {
        UINT32 NewCluster = Fat32AllocCluster(Fat);
        if (!NewCluster) {
            MemoryFree(Buf);
            return -1;
        }
        
        if (!Priv->FirstCluster) {
            Priv->FirstCluster = NewCluster;
        } else {
            UINT32 Last = Priv->FirstCluster;
            UINT32 Next;
            while ((Next = Fat32NextCluster(Fat, Last)) && Next < FAT32_EOC)
                Last = Next;
            Fat->FatCache[Last] = NewCluster;
        }
        
        TargetCluster = NewCluster;
        TargetOffset = 0;
        
        MemSet(Buf, 0, Fat->BytesPerCluster);
        Fat32WriteCluster(Fat, NewCluster, Buf);
    }
    
    // Read target cluster
    if (TargetCluster != Cluster)
        Fat32ReadCluster(Fat, TargetCluster, Buf);
    
    // Create LFN entries if needed
    UINT8 ShortName[11];
    UINT8 Checksum;
    Fat32NameToEntry(ShortName, Name, &Checksum);
    
    USIZE NameLen = StrLen(Name);
    INT LfnEntries = (NameLen > 12) ? (NameLen + 12) / 13 : 0;
    INT TotalEntries = 1 + LfnEntries;
    
    INT Offset = TargetOffset;
    
    // Create LFN entries
    if (LfnEntries > 0) {
        Fat32CreateLfnEntries(Fat, Buf, &Offset, Name, Checksum, 1);
    }
    
    // Create main entry
    Fat32DirEntry *Entry = (Fat32DirEntry*)(Buf + Offset);
    MemSet(Entry, 0, sizeof(Fat32DirEntry));
    MemCpy(Entry->Name, ShortName, 11);
    Entry->Attr = (Mode == FT_DIR) ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    
    // Set time
    DateTime Dt;
    RtcReadTime(&Dt);
    
    Entry->CrtDate = FAT_DATE(Dt.Year, Dt.Month, Dt.Day);
    Entry->CrtTime = FAT_TIME(Dt.Hour, Dt.Minute, Dt.Second);
    Entry->WrtDate = Entry->CrtDate;
    Entry->WrtTime = Entry->CrtTime;
    Entry->LstAccDate = Entry->CrtDate;
    
    // Allocate first cluster for file/dir
    if (Mode == FT_DIR) {
        UINT32 FirstCluster = Fat32AllocCluster(Fat);
        if (!FirstCluster) {
            MemoryFree(Buf);
            return -1;
        }
        
        Entry->FirstClusterLo = FirstCluster & 0xFFFF;
        Entry->FirstClusterHi = (FirstCluster >> 16) & 0xFFFF;
        
        // Initialize directory with . and ..
        UINT8 *DirBuf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
        if (!DirBuf) {
            MemoryFree(Buf);
            return -1;
        }
        MemSet(DirBuf, 0, Fat->BytesPerCluster);
        
        // . entry
        Fat32DirEntry *Dot = (Fat32DirEntry*)DirBuf;
        MemSet(Dot->Name, ' ', 11);
        Dot->Name[0] = '.';
        Dot->Attr = FAT_ATTR_DIRECTORY;
        Dot->FirstClusterLo = FirstCluster & 0xFFFF;
        Dot->FirstClusterHi = (FirstCluster >> 16) & 0xFFFF;
        Dot->CrtDate = Entry->CrtDate;
        Dot->CrtTime = Entry->CrtTime;
        
        // .. entry
        Fat32DirEntry *DotDot = (Fat32DirEntry*)(DirBuf + 32);
        MemSet(DotDot->Name, ' ', 11);
        DotDot->Name[0] = '.';
        DotDot->Name[1] = '.';
        DotDot->Attr = FAT_ATTR_DIRECTORY;
        DotDot->FirstClusterLo = Priv->FirstCluster & 0xFFFF;
        DotDot->FirstClusterHi = (Priv->FirstCluster >> 16) & 0xFFFF;
        DotDot->CrtDate = Entry->CrtDate;
        DotDot->CrtTime = Entry->CrtTime;
        
        Fat32WriteCluster(Fat, FirstCluster, DirBuf);
        MemoryFree(DirBuf);
    }
    
    Fat32WriteCluster(Fat, TargetCluster, Buf);
    MemoryFree(Buf);
    
    // Create inode
    VfsInode *Inode = VfsAllocInode();
    Fat32InodePrivate *NewPriv = (Fat32InodePrivate*)MemoryAllocate(sizeof(Fat32InodePrivate));
    
    UINT32 FirstCluster = (Entry->FirstClusterHi << 16) | Entry->FirstClusterLo;
    
    NewPriv->Fat = Fat;
    NewPriv->FirstCluster = FirstCluster;
    NewPriv->DirCluster = TargetCluster;
    NewPriv->DirEntry = EntryIndex;
    NewPriv->EntryOffset = Offset;
    NewPriv->ParentCluster = Priv->FirstCluster;
    
    Inode->IMode = Mode;
    Inode->ISize = 0;
    Inode->IPrivate = NewPriv;
    Inode->IOp = Dir->IOp;
    Inode->IFop = Dir->IFop;
    StrCpy(Inode->IFsName, "fat32");
    
    *Result = Inode;
    return 0;
}

static INT Fat32Mkdir(VfsInode *Dir, const CHAR *Name, UINT32 Mode, VfsInode **Result) {
    return Fat32Create(Dir, Name, FT_DIR, Result);
}

static INT Fat32Unlink(VfsInode *Dir, const CHAR *Name) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Dir->IPrivate;
    Fat32 *Fat = Priv->Fat;
    
    VfsInode *Inode;
    if (Fat32Lookup(Dir, Name, &Inode) != 0) return -1;
    
    Fat32InodePrivate *FilePriv = (Fat32InodePrivate*)Inode->IPrivate;

    UINT32 Cluster = FilePriv->FirstCluster;
    UINT8 *ZeroBuf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    
    if (ZeroBuf) {
        MemSet(ZeroBuf, 0, Fat->BytesPerCluster);
        while (Cluster && Cluster < FAT32_EOC) {
            Fat32WriteCluster(Fat, Cluster, ZeroBuf);
            Cluster = Fat32NextCluster(Fat, Cluster);
        }
        MemoryFree(ZeroBuf);
    }
    
    // Free clusters
    if (FilePriv->FirstCluster)
        Fat32FreeClusterChain(Fat, FilePriv->FirstCluster);
    
    // Mark directory entry as deleted
    UINT8 *Buf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!Buf) {
        VfsInodeUnref(Inode);
        return -1;
    }
    
    Fat32ReadCluster(Fat, FilePriv->DirCluster, Buf);
    
    Fat32DirEntry *Entry = (Fat32DirEntry*)(Buf + FilePriv->EntryOffset);
    Entry->Name[0] = 0xE5;
    
    Fat32WriteCluster(Fat, FilePriv->DirCluster, Buf);
    MemoryFree(Buf);
    
    VfsInodeUnref(Inode);
    return 0;
}

static INT Fat32Rmdir(VfsInode *Dir, const CHAR *Name) {
    return Fat32Unlink(Dir, Name);
}

static INT Fat32Rename(VfsInode *OldDir, const CHAR *OldName,
                        VfsInode *NewDir, const CHAR *NewName) {
    Fat32InodePrivate *OldPriv = (Fat32InodePrivate*)OldDir->IPrivate;
    Fat32 *Fat = OldPriv->Fat;
    
    // Find file
    VfsInode *File;
    if (Fat32Lookup(OldDir, OldName, &File) != 0) return -1;
    
    Fat32InodePrivate *FilePriv = (Fat32InodePrivate*)File->IPrivate;
    
    // Read old directory cluster
    UINT8 *OldBuf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!OldBuf) {
        VfsInodeUnref(File);
        return -1;
    }
    
    Fat32ReadCluster(Fat, FilePriv->DirCluster, OldBuf);
    
    // Save entry data (simplified - just main entry)
    UINT8 EntryData[32];
    MemCpy(EntryData, OldBuf + FilePriv->EntryOffset, 32);
    
    // Mark old entry as deleted
    Fat32DirEntry *OldEntry = (Fat32DirEntry*)(OldBuf + FilePriv->EntryOffset);
    OldEntry->Name[0] = 0xE5;
    Fat32WriteCluster(Fat, FilePriv->DirCluster, OldBuf);
    
    // Find free space in new directory
    Fat32InodePrivate *NewPriv = (Fat32InodePrivate*)NewDir->IPrivate;
    UINT32 NewCluster = NewPriv->FirstCluster;
    UINT8 *NewBuf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!NewBuf) {
        MemoryFree(OldBuf);
        VfsInodeUnref(File);
        return -1;
    }
    
    UINT32 NewOffset = 0;
    INT Found = 0;
    
    while (NewCluster && NewCluster < FAT32_EOC) {
        Fat32ReadCluster(Fat, NewCluster, NewBuf);
        
        for (UINT32 I = 0; I < Fat->BytesPerCluster / 32; I++) {
            Fat32DirEntry *E = (Fat32DirEntry*)NewBuf + I;
            if (E->Name[0] == 0 || E->Name[0] == 0xE5) {
                NewOffset = I * 32;
                Found = 1;
                break;
            }
        }
        if (Found) break;
        
        NewCluster = Fat32NextCluster(Fat, NewCluster);
    }
    
    if (!Found) {
        // Need to extend directory
        UINT32 Extra = Fat32AllocCluster(Fat);
        if (!Extra) {
            // Restore old entry
            MemCpy(OldBuf + FilePriv->EntryOffset, EntryData, 32);
            Fat32WriteCluster(Fat, FilePriv->DirCluster, OldBuf);
            MemoryFree(OldBuf);
            MemoryFree(NewBuf);
            VfsInodeUnref(File);
            return -1;
        }
        
        if (!NewPriv->FirstCluster) {
            NewPriv->FirstCluster = Extra;
        } else {
            UINT32 Last = NewPriv->FirstCluster;
            UINT32 Next;
            while ((Next = Fat32NextCluster(Fat, Last)) && Next < FAT32_EOC)
                Last = Next;
            Fat->FatCache[Last] = Extra;
        }
        
        NewCluster = Extra;
        MemSet(NewBuf, 0, Fat->BytesPerCluster);
        NewOffset = 0;
    }
    
    // Update name
    UINT8 ShortName[11];
    Fat32NameToEntry(ShortName, NewName, NULLPTR);
    
    Fat32DirEntry *NewEntry = (Fat32DirEntry*)(EntryData);
    MemCpy(NewEntry->Name, ShortName, 11);
    
    // Write to new directory
    MemCpy(NewBuf + NewOffset, EntryData, 32);
    Fat32WriteCluster(Fat, NewCluster, NewBuf);
    
    // Update inode private data
    FilePriv->DirCluster = NewCluster;
    FilePriv->EntryOffset = NewOffset;
    FilePriv->ParentCluster = NewPriv->FirstCluster;
    
    MemoryFree(OldBuf);
    MemoryFree(NewBuf);
    VfsInodeUnref(File);
    return 0;
}

static INT Fat32GetName(VfsInode *Inode, CHAR *Name, INT MaxLen) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Inode->IPrivate;
    Fat32 *Fat = Priv->Fat;
    
    if (Priv->FirstCluster == Fat->RootCluster) {
        Name[0] = '\0';
        return 0;
    }
    
    UINT8 *Buf = (UINT8*)MemoryAllocate(Fat->BytesPerCluster);
    if (!Buf) return -1;
    
    if (Fat32ReadCluster(Fat, Priv->DirCluster, Buf) != 0) {
        MemoryFree(Buf);
        return -1;
    }
    
    Fat32DirEntry *Entry = (Fat32DirEntry*)(Buf + Priv->EntryOffset);
    Fat32NameFromEntry(Name, Entry->Name);
    
    MemoryFree(Buf);
    return 0;
}

static INT Fat32Chmod(VfsInode *Inode, UINT32 Mode) {
    Inode->IMode = Mode;
    return 0;
}

static INT Fat32Stat(VfsInode *Inode, NOPTR *StatBuf) {
    return VfsStat(Inode, (VfsStatS*)StatBuf);
}

static INT Fat32Sync(VfsInode *Inode) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Inode->IPrivate;
    Fat32 *Fat = Priv->Fat;
    
    if (Fat->Dirty) {
        Fat32WriteSectors(Fat, Fat->FatStart, Fat->Bpb.FatSize32, Fat->FatCache);
        Fat32UpdateFsInfo(Fat);
    }
    
    return 0;
}

static INT Fat32Parent(VfsInode *Inode, VfsInode **Parent) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Inode->IPrivate;
    Fat32 *Fat = Priv->Fat;
    
    if (Priv->FirstCluster == Fat->RootCluster) {
        *Parent = Inode;
        return 0;
    }
    
    VfsInode *ParentInode = VfsAllocInode();
    Fat32InodePrivate *ParentPriv = (Fat32InodePrivate*)MemoryAllocate(sizeof(Fat32InodePrivate));
    
    ParentPriv->Fat = Fat;
    ParentPriv->FirstCluster = Priv->ParentCluster;
    ParentPriv->DirCluster = 0;
    ParentPriv->DirEntry = 0;
    ParentPriv->EntryOffset = 0;
    ParentPriv->ParentCluster = Fat->RootCluster;
    
    ParentInode->IMode = FT_DIR;
    ParentInode->IPrivate = ParentPriv;
    ParentInode->IOp = Inode->IOp;
    ParentInode->IFop = Inode->IFop;
    
    *Parent = ParentInode;
    return 0;
}

static INT Fat32Unmount(VfsInode *Root) {
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)Root->IPrivate;
    Fat32 *Fat = Priv->Fat;
    
    if (Fat->Dirty) {
        Fat32WriteSectors(Fat, Fat->FatStart, Fat->Bpb.FatSize32, Fat->FatCache);
        Fat32UpdateFsInfo(Fat);
    }
    
    if (Fat->FatCache) MemoryFree(Fat->FatCache);
    MemoryFree(Fat);
    MemoryFree(Priv);
    VfsInodeUnref(Root);
    
    return 0;
}

static VfsOperations GFat32NodeOps = {
    .Lookup = Fat32Lookup,
    .ReadDir = Fat32ReadDir,
    .Create = Fat32Create,
    .Mkdir = Fat32Mkdir,
    .Unlink = Fat32Unlink,
    .Rmdir = Fat32Rmdir,
    .Rename = Fat32Rename,
    .Chmod = Fat32Chmod,
    .Stat = Fat32Stat,
    .GetName = Fat32GetName,
    .Parent = Fat32Parent,
    .Unmount = Fat32Unmount,
};

static VfsFileOperations GFat32FileOps = {
    .Read = Fat32Read,
    .Write = Fat32Write,
    .Sync = Fat32Sync,
};

//==================== MOUNTING ====================

static INT Fat32Mount(Drive *Dev, VfsInode **Root) {
    ConsolePrint("[FAT32] Mounting...\n");
    
    Fat32 *Fat = (Fat32*)MemoryAllocate(sizeof(Fat32));
    if (!Fat) return -1;
    MemSet(Fat, 0, sizeof(Fat32));
    
    Fat->Dev = Dev;
    
    // Read BPB
    UINT8 Sector[512];
    if (Fat32ReadSectors(Fat, 0, 1, Sector) != 0) {
        MemoryFree(Fat);
        return -1;
    }
    
    Fat32Bpb *Bpb = (Fat32Bpb*)Sector;
    
    // Check FAT32 signature
    if (Bpb->BytesPerSector != 512 || Bpb->SectorsPerCluster == 0 ||
        Bpb->FatSize32 == 0) {
        ConsolePrint("[FAT32] Not a FAT32 volume\n");
        MemoryFree(Fat);
        return -1;
    }
    
    Fat->Bpb = *Bpb;
    Fat->BytesPerSector = Bpb->BytesPerSector;
    Fat->SectorsPerCluster = Bpb->SectorsPerCluster;
    Fat->BytesPerCluster = Fat->BytesPerSector * Fat->SectorsPerCluster;
    
    Fat->FatStart = Bpb->ReservedSectors;
    Fat->DataStart = Fat->FatStart + Bpb->NumFats * Bpb->FatSize32;
    Fat->RootCluster = Bpb->RootCluster;
    Fat->FsinfoSector = Bpb->FsInfo;
    
    Fat->FatEntries = Bpb->FatSize32 * Fat->BytesPerSector / 4;
    Fat->TotalClusters = (Bpb->TotalSectors32 - Fat->DataStart) / Fat->SectorsPerCluster;
    
    ConsolePrint("[FAT32] Cluster size: %d, Root cluster: %d, Total clusters: %d\n",
               Fat->BytesPerCluster, Fat->RootCluster, Fat->TotalClusters);
    
    // Read FAT
    Fat->FatCache = (UINT32*)MemoryAllocate(Bpb->FatSize32 * 512);
    if (!Fat->FatCache) {
        MemoryFree(Fat);
        return -1;
    }
    
    if (Fat32ReadSectors(Fat, Fat->FatStart, Bpb->FatSize32, Fat->FatCache) != 0) {
        MemoryFree(Fat->FatCache);
        MemoryFree(Fat);
        return -1;
    }
    
    // Read FSInfo
    if (Fat->FsinfoSector) {
        Fat32ReadSectors(Fat, Fat->FsinfoSector, 1, &Fat->FsInfo);
    } else {
        Fat->FsInfo.FreeCount = 0;
        Fat->FsInfo.NextFree = 2;
        for (UINT32 I = 2; I < Fat->FatEntries; I++) {
            if (Fat->FatCache[I] == FAT32_FREE)
                Fat->FsInfo.FreeCount++;
        }
    }
    
    // Create root inode
    VfsInode *RootInode = VfsAllocInode();
    Fat32InodePrivate *Priv = (Fat32InodePrivate*)MemoryAllocate(sizeof(Fat32InodePrivate));
    
    Priv->Fat = Fat;
    Priv->FirstCluster = Fat->RootCluster;
    Priv->DirCluster = 0;
    Priv->DirEntry = 0;
    Priv->EntryOffset = 0;
    Priv->ParentCluster = Fat->RootCluster;
    
    RootInode->IMode = FT_DIR;
    RootInode->ISize = 0;
    RootInode->IPrivate = Priv;
    RootInode->IOp = &GFat32NodeOps;
    RootInode->IFop = &GFat32FileOps;
    RootInode->IDev = Dev;
    StrCpy(RootInode->IFsName, "fat32");
    
    *Root = RootInode;
    
    ConsolePrint("[FAT32] Mounted successfully\n");
    return 0;
}

//==================== FORMATTING ====================

INT Fat32Format(Drive *Dev) {
    if (!Dev || !Dev->Write) {
        ConsolePrint("[FAT32] Device not ready\n");
        return -1;
    }
    
    UINT32 SectorSize = Dev->SectorSize;
    UINT64 TotalSectors = Dev->TotalSectors;
    
    // Calculate parameters
    UINT8 SectorsPerCluster;
    if (TotalSectors < 0x100000) SectorsPerCluster = 1;      // <512MB
    else if (TotalSectors < 0x400000) SectorsPerCluster = 4; // <2GB
    else if (TotalSectors < 0x1000000) SectorsPerCluster = 8; // <8GB
    else SectorsPerCluster = 16; // >8GB
    
    UINT32 ReservedSectors = 32;
    UINT32 FatSize = (TotalSectors + SectorsPerCluster - 1) / SectorsPerCluster;
    FatSize = (FatSize * 4 + SectorSize - 1) / SectorSize;
    
    UINT32 DataStart = ReservedSectors + 2 * FatSize;
    UINT32 RootCluster = 2;
    
    // Create BPB
    Fat32Bpb Bpb;
    MemSet(&Bpb, 0, sizeof(Bpb));
    
    Bpb.JumpBoot[0] = 0xEB;
    Bpb.JumpBoot[1] = 0x58;
    Bpb.JumpBoot[2] = 0x90;
    MemCpy(Bpb.OemName, "TOS     ", 8);
    Bpb.BytesPerSector = 512;
    Bpb.SectorsPerCluster = SectorsPerCluster;
    Bpb.ReservedSectors = ReservedSectors;
    Bpb.NumFats = 2;
    Bpb.RootEntries = 0;
    Bpb.TotalSectors16 = 0;
    Bpb.MediaDescriptor = 0xF8;
    Bpb.FatSize16 = 0;
    Bpb.SectorsPerTrack = 63;
    Bpb.NumHeads = 255;
    Bpb.HiddenSectors = 0;
    Bpb.TotalSectors32 = TotalSectors;
    
    Bpb.FatSize32 = FatSize;
    Bpb.ExtendedFlags = 0;
    Bpb.FsVersion = 0;
    Bpb.RootCluster = RootCluster;
    Bpb.FsInfo = 1;
    Bpb.BackupBootSector = 6;
    Bpb.DriveNumber = 0x80;
    Bpb.Reserved1 = 0;
    Bpb.BootSignature = 0x29;
    Bpb.VolumeId = 0x12345678;
    MemCpy(Bpb.VolumeLabel, "TOSDISK    ", 11);
    MemCpy(Bpb.FsType, "FAT32   ", 8);
    
    // Boot sector
    UINT8 BootSector[512];
    MemSet(BootSector, 0, 512);
    MemCpy(BootSector, &Bpb, sizeof(Bpb));
    BootSector[510] = 0x55;
    BootSector[511] = 0xAA;
    
    // Write boot sector
    if (Dev->Write(Dev, 0, 1, BootSector) != 0) {
        ConsolePrint("[FAT32] Failed to write boot sector\n");
        return -1;
    }
    
    // Write backup boot sector
    Dev->Write(Dev, 6, 1, BootSector);
    
    // Create FSInfo
    Fat32FsInfo FsInfo;
    MemSet(&FsInfo, 0, sizeof(FsInfo));
    FsInfo.LeadSignature = 0x41615252;
    FsInfo.StructSignature = 0x61417272;
    FsInfo.FreeCount = (TotalSectors - DataStart) / SectorsPerCluster;
    FsInfo.NextFree = RootCluster + 1;
    FsInfo.TrailSignature = 0xAA550000;
    
    Dev->Write(Dev, 1, 1, &FsInfo);
    Dev->Write(Dev, 7, 1, &FsInfo);
    
    // Create FAT
    UINT32 *Fat = (UINT32*)MemoryAllocate(FatSize * SectorSize);
    if (!Fat) return -1;
    MemSet(Fat, 0, FatSize * SectorSize);
    
    Fat[0] = 0x0FFFFFF8;
    Fat[1] = 0x0FFFFFFF;
    Fat[RootCluster] = 0x0FFFFFFF;
    
    // Write FATs with progress bar
    UINT32 BlockSize = 256;
    UINT8 *BlockBuf = (UINT8*)MemoryAllocate(BlockSize * SectorSize);
    if (!BlockBuf) {
        MemoryFree(Fat);
        return -1;
    }
    
    UINT32 LastPercent = 0;
    ConsolePrint("[");
    
    // Write both FATs
    for (INT FatNum = 0; FatNum < 2; FatNum++) {
        UINT32 FatStartSector = ReservedSectors + FatNum * FatSize;
        
        for (UINT32 I = 0; I < FatSize; I += BlockSize) {
            UINT32 Chunk = (I + BlockSize > FatSize) ? FatSize - I : BlockSize;
            
            for (UINT32 K = 0; K < Chunk; K++) {
                MemCpy(BlockBuf + K * SectorSize, 
                       (UINT8*)Fat + (I + K) * SectorSize, 
                       SectorSize);
            }
            
            if (Dev->Write(Dev, FatStartSector + I, Chunk, BlockBuf) != 0) {
                MemoryFree(BlockBuf);
                MemoryFree(Fat);
                return -1;
            }
            
            // Calculate overall progress (0-100 for both FATs)
            UINT32 TotalWritten = (FatNum * FatSize + I + Chunk);
            UINT32 TotalToWrite = 2 * FatSize;
            UINT32 Percent = (TotalWritten * 100) / TotalToWrite;
            
            if (Percent != LastPercent) {
                UINT32 Pos = (Percent * 50) / 100;
                
                ConsolePrint("\r[");
                for (UINT32 J = 0; J < 50; J++) {
                    ConsolePrint(J < Pos ? "=" : (J == Pos ? ">" : " "));
                }
                ConsolePrint("] %3u%%", Percent);
                LastPercent = Percent;
            }
        }
    }
    
    ConsolePrint("\n");
    MemoryFree(BlockBuf);
    MemoryFree(Fat);
    
    // Initialize root directory
    UINT32 RootSectors = SectorsPerCluster;
    UINT8 *RootBuf = (UINT8*)MemoryAllocate(RootSectors * SectorSize);
    if (!RootBuf) return -1;
    MemSet(RootBuf, 0, RootSectors * SectorSize);
    
    // . entry
    Fat32DirEntry *Dot = (Fat32DirEntry*)RootBuf;
    MemSet(Dot->Name, ' ', 11);
    Dot->Name[0] = '.';
    Dot->Attr = FAT_ATTR_DIRECTORY;
    Dot->FirstClusterLo = RootCluster & 0xFFFF;
    Dot->FirstClusterHi = (RootCluster >> 16) & 0xFFFF;
    
    // .. entry
    Fat32DirEntry *DotDot = (Fat32DirEntry*)(RootBuf + 32);
    MemSet(DotDot->Name, ' ', 11);
    DotDot->Name[0] = '.';
    DotDot->Name[1] = '.';
    DotDot->Attr = FAT_ATTR_DIRECTORY;
    DotDot->FirstClusterLo = RootCluster & 0xFFFF;
    DotDot->FirstClusterHi = (RootCluster >> 16) & 0xFFFF;
    
    Dev->Write(Dev, DataStart, RootSectors, RootBuf);
    MemoryFree(RootBuf);
    
    if (Dev->Sync) Dev->Sync(Dev);
    
    ConsolePrint("Done.\n");
    return 0;
}

//==================== INITIALIZATION ====================

static FileSystem GFat32Fs = {
    .Name = "Fat32",
    .Mount = Fat32Mount,
    .Unmount = Fat32Unmount,
    .Next = NULLPTR
};

NOPTR Fat32Init(NOPTR) {
    VfsRegisterFs(&GFat32Fs);
    ConsolePrint("[FAT32] Driver initialized\n");
}