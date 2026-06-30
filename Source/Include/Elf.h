#pragma once

#include <Kernel/Types.h>

/* ELF magic numbers */
#define EI_MAG0         0
#define EI_MAG1         1
#define EI_MAG2         2
#define EI_MAG3         3
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION   8
#define EI_PAD          9
#define EI_NIDENT       16

#define ELFMAG0         0x7F
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'
#define ELFMAG          "\177ELF"

#define ELFCLASSNONE    0
#define ELFCLASS32      1
#define ELFCLASS64      2

#define ELFDATANONE     0
#define ELFDATA2LSB     1      /* Little endian */
#define ELFDATA2MSB     2      /* Big endian */

#define EV_NONE         0
#define EV_CURRENT      1

#define ET_NONE         0
#define ET_REL          1      /* Relocatable file */
#define ET_EXEC         2      /* Executable file */
#define ET_DYN          3      /* Shared object */
#define ET_CORE         4      /* Core file */

#define EM_X86_64       0x3E   /* AMD64 */

/* Program header types */
#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_TLS          7
#define PT_LOOS         0x60000000
#define PT_HIOS         0x6FFFFFFF
#define PT_LOPROC       0x70000000
#define PT_HIPROC       0x7FFFFFFF

/* Program header flags */
#define PF_X            (1 << 0)   /* Execute */
#define PF_W            (1 << 1)   /* Write */
#define PF_R            (1 << 2)   /* Read */

/* Section header types */
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_HASH        5
#define SHT_DYNAMIC     6
#define SHT_NOTE        7
#define SHT_NOBITS      8
#define SHT_REL         9
#define SHT_SHLIB       10
#define SHT_DYNSYM      11

/* Section header flags */
#define SHF_WRITE       (1 << 0)
#define SHF_ALLOC       (1 << 1)
#define SHF_EXECINSTR   (1 << 2)

/* ELF header structure (64-bit) */
typedef struct {
    UINT8  EIdent[EI_NIDENT];
    UINT16 EType;
    UINT16 EMachine;
    UINT32 EVersion;
    UINT64 EEntry;
    UINT64 EPhoff;
    UINT64 EShoff;
    UINT32 EFlags;
    UINT16 EEhsize;
    UINT16 EPhentsize;
    UINT16 EPhnum;
    UINT16 EShentsize;
    UINT16 EShnum;
    UINT16 EShstrndx;
} ATTRIBUTE(packed) Elf64Ehdr;

/* Program header structure (64-bit) */
typedef struct {
    UINT32 PType;
    UINT32 PFlags;
    UINT64 POffset;
    UINT64 PVaddr;
    UINT64 PPaddr;
    UINT64 PFilesz;
    UINT64 PMemsz;
    UINT64 PAlign;
} ATTRIBUTE(packed) Elf64Phdr;

/* Section header structure (64-bit) */
typedef struct {
    UINT32 ShName;
    UINT32 ShType;
    UINT64 ShFlags;
    UINT64 ShAddr;
    UINT64 ShOffset;
    UINT64 ShSize;
    UINT32 ShLink;
    UINT32 ShInfo;
    UINT64 ShAddralign;
    UINT64 ShEntsize;
} ATTRIBUTE(packed) Elf64Shdr;

/* Symbol table entry (64-bit) */
typedef struct {
    UINT32 StName;
    UINT8  StInfo;
    UINT8  StOther;
    UINT16 StShndx;
    UINT64 StValue;
    UINT64 StSize;
} ATTRIBUTE(packed) Elf64Sym;

/* Relocation entry (without addend) */
typedef struct {
    UINT64 ROffset;
    UINT64 RInfo;
} ATTRIBUTE(packed) Elf64Rel;

/* Relocation entry (with addend) */
typedef struct {
    UINT64 ROffset;
    UINT64 RInfo;
    INT64  RAddend;
} ATTRIBUTE(packed) Elf64Rela;

/* Helper macros */
#define ELF64_R_SYM(I)     ((I) >> 32)
#define ELF64_R_TYPE(I)    ((I) & 0xFFFFFFFF)
#define ELF64_R_INFO(S,T)  (((S) << 32) + ((T) & 0xFFFFFFFF))

/* x86_64 relocation types */
#define R_X86_64_NONE      0
#define R_X86_64_64        1
#define R_X86_64_PC32      2
#define R_X86_64_GOT32     3
#define R_X86_64_PLT32     4
#define R_X86_64_COPY      5
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8
#define R_X86_64_GOTPCREL  9
#define R_X86_64_32        10
#define R_X86_64_32S       11
#define R_X86_64_16        12
#define R_X86_64_PC16      13
#define R_X86_64_8         14
#define R_X86_64_PC8       15

/* ELF load result */
typedef enum {
    ELF_LOAD_SUCCESS = 0,
    ELF_LOAD_INVALID_MAGIC,
    ELF_LOAD_WRONG_CLASS,
    ELF_LOAD_NOT_EXEC,
    ELF_LOAD_NO_MEMORY,
    ELF_LOAD_INVALID_PHDR,
    ELF_LOAD_RELOCATION_ERROR
} ElfLoadResult;

/* Loaded ELF image */
typedef struct {
    UINT64 EntryPoint;
    UINT64 *PML4;           /* Page table for user space */
    UINT64 BaseAddr;        /* Load base address */
    UINT64 TotalSize;       /* Total size in memory */
    UINT8 *ProgramName;     /* Name of the program */
} ElfLoadedImage;

/* Function prototypes */
ElfLoadResult ElfLoad(const UINT8 *ElfData, USIZE ElfSize, ElfLoadedImage *Out);
ElfLoadResult ElfExecute(ElfLoadedImage *Image, INT Argc, CHAR **Argv);
NOPTR ElfUnload(ElfLoadedImage *Image);