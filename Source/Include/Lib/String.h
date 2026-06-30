#pragma once

#include <Kernel/Types.h>

NOPTR *MemCpy(NOPTR *Dst_, const NOPTR *Src_, USIZE N);
NOPTR *MemSet(NOPTR *S, INT C, USIZE N);
INT MemCmp(const NOPTR *Ptr1, const NOPTR *Ptr2, USIZE Num);
INT SecureMemCmp(const NOPTR *Ptr1, const NOPTR *Ptr2, USIZE Num);
NOPTR SecureMemZero(NOPTR *Ptr, USIZE Num);
NOPTR *MemMove(NOPTR *Dst0, const NOPTR *Src0, USIZE N);
NOPTR *MemMem(const NOPTR *HayStack, USIZE HayStackLen, const NOPTR *Needle, USIZE NeedleLen);

USIZE StrLen(const CHAR *S);
CHAR *StrCpy(CHAR *Dst, const CHAR *Src);
CHAR *StrnCpy(CHAR *Dst, const CHAR *Src, USIZE N);
CHAR *StrCat(CHAR *Dst, const CHAR *Src);
INT StrCmp(const CHAR *A, const CHAR *B);
INT StrCaseCmp(const CHAR *A, const CHAR *B);
INT StrnCmp(const CHAR *A, const CHAR *B, USIZE N);
CHAR *StrChr(const CHAR *S, INT C);
CHAR *StrrChr(const CHAR *S, INT C);
CHAR *StrnCat(CHAR *Dest, const CHAR *Src, USIZE N);
CHAR *StrTokR(CHAR *Str, const CHAR *Delim, CHAR **SavePtr);
INT NameEq(const CHAR *A, const CHAR *B, USIZE N);
CHAR *StrStr(const CHAR *HayStack, const CHAR *Needle);

INT AToI(const CHAR* str);
LONG AToL(const CHAR* Str);
CHAR* UToA(UINT32 Value, CHAR* Str, INT Base);
