#pragma once

#include <Kernel/Types.h>
#include <Fs/Vfs.h>

#define TOSASM_MAX_OUTPUT   (512 * 1024)
#define TOSASM_MAX_LABELS   256
#define TOSASM_MAX_SYMBOLS  256
#define TOSASM_LOAD_BASE    0x400000ULL

INT TosAsmAssembleSource(const CHAR *Source, USIZE SourceLen,
                         UINT8 **OutElf, USIZE *OutSize);
INT TosAsmAssembleFile(VfsInode *BaseDir, const CHAR *SrcPath, const CHAR *OutPath);
