#include <Kernel/TosAsm.h>
#include <Kernel/Return.h>
#include <Memory/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Fs/Vfs.h>
#include <Elf.h>
#include <Console.h>

typedef enum {
    SEC_TEXT,
    SEC_DATA,
    SEC_BSS
} AsmSection;

typedef enum {
    REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP,
    REG_RSI, REG_RDI, REG_R8, REG_R9, REG_R10, REG_R11,
    REG_R12, REG_R13, REG_R14, REG_R15, REG_NONE
} AsmReg;

typedef struct {
    CHAR Name[64];
    UINT64 Offset;
    AsmSection Section;
} AsmLabel;

typedef struct {
    UINT8 *Text;
    USIZE TextCap;
    USIZE TextLen;
    UINT8 *Data;
    USIZE DataCap;
    USIZE DataLen;
    USIZE BssLen;
    USIZE TextSize;
    USIZE DataSize;
    AsmSection CurSec;
    AsmLabel Labels[TOSASM_MAX_LABELS];
    INT LabelCount;
    BOOL HasEntry;
    CHAR EntryName[64];
} AsmState;

static INT AsmEmitText(AsmState *S, UINT8 Byte) {
    if (S->TextLen >= S->TextCap) {
        return NO_MEMORY;
    }
    S->Text[S->TextLen++] = Byte;
    return SUCCESS;
}

static INT AsmEmitData(AsmState *S, UINT8 Byte) {
    if (S->DataLen >= S->DataCap) {
        return NO_MEMORY;
    }
    S->Data[S->DataLen++] = Byte;
    return SUCCESS;
}

static INT AsmEmitDataU64(AsmState *S, UINT64 Val) {
    INT I;
    for (I = 0; I < 8; I++) {
        if (AsmEmitData(S, (UINT8)(Val >> (I * 8))) != SUCCESS) {
            return NO_MEMORY;
        }
    }
    return SUCCESS;
}

static INT AsmParseNumber(const CHAR *Str, UINT64 *Out) {
    UINT64 Val = 0;
    INT Base = 10;
    USIZE I = 0;

    if (Str[0] == '0' && (Str[1] == 'x' || Str[1] == 'X')) {
        Base = 16;
        I = 2;
    }

    for (; Str[I]; I++) {
        INT Digit = -1;
        if (Str[I] >= '0' && Str[I] <= '9') {
            Digit = Str[I] - '0';
        } else if (Base == 16 && Str[I] >= 'a' && Str[I] <= 'f') {
            Digit = Str[I] - 'a' + 10;
        } else if (Base == 16 && Str[I] >= 'A' && Str[I] <= 'F') {
            Digit = Str[I] - 'A' + 10;
        } else {
            break;
        }
        Val = Val * (UINT64)Base + (UINT64)Digit;
    }

    if (I == 0 || (Base == 16 && I == 2)) {
        return INCORRECT_VALUE;
    }
    *Out = Val;
    return SUCCESS;
}

static AsmReg AsmParseReg(const CHAR *Name) {
    struct { const CHAR *N; AsmReg R; } Map[] = {
        {"rax", REG_RAX}, {"rcx", REG_RCX}, {"rdx", REG_RDX}, {"rbx", REG_RBX},
        {"rsp", REG_RSP}, {"rbp", REG_RBP}, {"rsi", REG_RSI}, {"rdi", REG_RDI},
        {"r8", REG_R8}, {"r9", REG_R9}, {"r10", REG_R10}, {"r11", REG_R11},
        {"r12", REG_R12}, {"r13", REG_R13}, {"r14", REG_R14}, {"r15", REG_R15},
        {NULLPTR, REG_NONE}
    };
    INT I;
    for (I = 0; Map[I].N; I++) {
        if (StrCmp(Name, Map[I].N) == 0) {
            return Map[I].R;
        }
    }
    return REG_NONE;
}

static UINT8 AsmRegId(AsmReg R) {
    static const UINT8 Ids[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    };
    return Ids[R];
}

static USIZE AsmCurrentOffset(AsmState *S) {
    if (S->CurSec == SEC_TEXT) {
        return S->TextLen;
    }
    if (S->CurSec == SEC_DATA) {
        return S->DataLen;
    }
    return S->BssLen;
}

static INT AsmAddLabel(AsmState *S, const CHAR *Name, UINT64 Offset, AsmSection Sec) {
    INT I;
    for (I = 0; I < S->LabelCount; I++) {
        if (StrCmp(S->Labels[I].Name, Name) == 0) {
            S->Labels[I].Offset = Offset;
            S->Labels[I].Section = Sec;
            return SUCCESS;
        }
    }
    if (S->LabelCount >= TOSASM_MAX_LABELS) {
        return NO_MEMORY;
    }
    StrnCpy(S->Labels[S->LabelCount].Name, Name, 63);
    S->Labels[S->LabelCount].Offset = Offset;
    S->Labels[S->LabelCount].Section = Sec;
    S->LabelCount++;
    return SUCCESS;
}

static INT AsmResolveLabel(AsmState *S, const CHAR *Name, UINT64 *Out) {
    INT I;
    for (I = 0; I < S->LabelCount; I++) {
        if (StrCmp(S->Labels[I].Name, Name) == 0) {
            if (S->Labels[I].Section == SEC_TEXT) {
                *Out = TOSASM_LOAD_BASE + S->Labels[I].Offset;
            } else if (S->Labels[I].Section == SEC_DATA) {
                *Out = TOSASM_LOAD_BASE + S->TextSize + S->Labels[I].Offset;
            } else {
                *Out = TOSASM_LOAD_BASE + S->TextSize + S->DataSize + S->Labels[I].Offset;
            }
            return SUCCESS;
        }
    }
    return NOT_FOUND;
}

static INT AsmResolveExpr(AsmState *S, const CHAR *Expr, UINT64 *Out) {
    CHAR Buf[128];
    UINT64 Val;

    StrnCpy(Buf, Expr, sizeof(Buf) - 1);
    {
        CHAR *P = Buf;
        while (*P == ' ' || *P == '\t') {
            P++;
        }
        if (*P == '\0') {
            return INCORRECT_VALUE;
        }
        if (AsmParseNumber(P, &Val) == SUCCESS) {
            *Out = Val;
            return SUCCESS;
        }
        return AsmResolveLabel(S, P, Out);
    }
}

static INT AsmEmitRex(AsmState *S, AsmReg Dst, AsmReg Src, BOOL W) {
    UINT8 Rex = 0x40;
    if (W) {
        Rex |= 0x08;
    }
    if (Dst >= REG_R8) {
        Rex |= 0x04;
    }
    if (Src >= REG_R8) {
        Rex |= 0x01;
    }
    if (Rex != 0x40) {
        return AsmEmitText(S, Rex);
    }
    return SUCCESS;
}

static INT AsmEmitMovRegImm(AsmState *S, AsmReg Dst, UINT64 Imm) {
    INT I;
    if (AsmEmitRex(S, Dst, REG_NONE, TRUE) != SUCCESS) {
        return NO_MEMORY;
    }
    if (AsmEmitText(S, (UINT8)(0xB8 + AsmRegId(Dst))) != SUCCESS) {
        return NO_MEMORY;
    }
    for (I = 0; I < 8; I++) {
        if (AsmEmitText(S, (UINT8)(Imm >> (I * 8))) != SUCCESS) {
            return NO_MEMORY;
        }
    }
    return SUCCESS;
}

static INT AsmEmitMovRegReg(AsmState *S, AsmReg Dst, AsmReg Src) {
    if (AsmEmitRex(S, Dst, Src, TRUE) != SUCCESS) {
        return NO_MEMORY;
    }
    if (AsmEmitText(S, 0x89) != SUCCESS) {
        return NO_MEMORY;
    }
    return AsmEmitText(S, (UINT8)(0xC0 | (AsmRegId(Src) << 3) | AsmRegId(Dst)));
}

static INT AsmEmitPushPop(AsmState *S, AsmReg R, BOOL Push) {
    if (R >= REG_R8) {
        if (AsmEmitText(S, 0x41) != SUCCESS) {
            return NO_MEMORY;
        }
    }
    return AsmEmitText(S, (UINT8)((Push ? 0x50 : 0x58) + AsmRegId(R)));
}

static INT AsmEmitAluRegImm(AsmState *S, UINT8 OpExt, AsmReg Dst, UINT32 Imm) {
    INT I;
    if (AsmEmitRex(S, Dst, REG_NONE, TRUE) != SUCCESS) {
        return NO_MEMORY;
    }
    if (AsmEmitText(S, 0x81) != SUCCESS) {
        return NO_MEMORY;
    }
    if (AsmEmitText(S, (UINT8)(0xC0 | (OpExt << 3) | AsmRegId(Dst))) != SUCCESS) {
        return NO_MEMORY;
    }
    for (I = 0; I < 4; I++) {
        if (AsmEmitText(S, (UINT8)(Imm >> (I * 8))) != SUCCESS) {
            return NO_MEMORY;
        }
    }
    return SUCCESS;
}

static INT AsmEmitJmpRel(AsmState *S, INT32 Rel32) {
    INT I;
    if (AsmEmitText(S, 0xE9) != SUCCESS) {
        return NO_MEMORY;
    }
    for (I = 0; I < 4; I++) {
        if (AsmEmitText(S, (UINT8)((UINT32)Rel32 >> (I * 8))) != SUCCESS) {
            return NO_MEMORY;
        }
    }
    return SUCCESS;
}

static INT AsmEmitLeaRip(AsmState *S, AsmReg Dst, UINT64 Target) {
    INT32 Rel;
    INT I;
    if (AsmEmitRex(S, Dst, REG_NONE, TRUE) != SUCCESS) {
        return NO_MEMORY;
    }
    if (AsmEmitText(S, 0x8D) != SUCCESS) {
        return NO_MEMORY;
    }
    if (AsmEmitText(S, (UINT8)(0x05 | (AsmRegId(Dst) << 3))) != SUCCESS) {
        return NO_MEMORY;
    }
    Rel = (INT32)(Target - (TOSASM_LOAD_BASE + S->TextLen + 4));
    for (I = 0; I < 4; I++) {
        if (AsmEmitText(S, (UINT8)((UINT32)Rel >> (I * 8))) != SUCCESS) {
            return NO_MEMORY;
        }
    }
    return SUCCESS;
}

static INT AsmProcessLine(AsmState *S, CHAR *Line, BOOL Emit) {
    CHAR Label[64];
    CHAR Op[32];
    CHAR Arg1[128];
    CHAR Arg2[128];
    CHAR *Colon;
    CHAR *Save;
    CHAR *Tok;

    {
        CHAR *Semi = StrChr(Line, ';');
        if (Semi) {
            *Semi = '\0';
        }
    }

    Label[0] = '\0';
    Colon = StrChr(Line, ':');
    if (Colon) {
        USIZE Len = (USIZE)(Colon - Line);
        if (Len < sizeof(Label)) {
            MemCpy(Label, Line, Len);
            Label[Len] = '\0';
            if (!Emit) {
                AsmAddLabel(S, Label, AsmCurrentOffset(S), S->CurSec);
            }
        }
        MemMove(Line, Colon + 1, StrLen(Colon + 1) + 1);
    }

    Tok = StrTokR(Line, " \t,", &Save);
    if (!Tok || Tok[0] == '\0') {
        return SUCCESS;
    }
    StrCpy(Op, Tok);
    Arg1[0] = '\0';
    Arg2[0] = '\0';
    Tok = StrTokR(NULLPTR, " \t,", &Save);
    if (Tok) {
        StrCpy(Arg1, Tok);
        Tok = StrTokR(NULLPTR, " \t,", &Save);
        if (Tok) {
            StrCpy(Arg2, Tok);
        }
    }

    if (StrCmp(Op, "section") == 0) {
        if (StrStr(Arg1, ".text")) {
            S->CurSec = SEC_TEXT;
        } else if (StrStr(Arg1, ".data")) {
            S->CurSec = SEC_DATA;
        } else if (StrStr(Arg1, ".bss")) {
            S->CurSec = SEC_BSS;
        }
        return SUCCESS;
    }
    if (StrCmp(Op, "bits") == 0 || StrCmp(Op, "global") == 0 || StrCmp(Op, "extern") == 0) {
        if (StrCmp(Op, "global") == 0) {
            StrnCpy(S->EntryName, Arg1, 63);
            S->HasEntry = TRUE;
        }
        return SUCCESS;
    }

    if (!Emit) {
        if (StrCmp(Op, "db") == 0) {
            if (Arg1[0] == '"') {
                USIZE J = 1;
                while (Arg1[J] && Arg1[J] != '"') {
                    if (S->CurSec == SEC_TEXT) {
                        S->TextLen++;
                    } else {
                        S->DataLen++;
                    }
                    J++;
                }
            } else if (S->CurSec == SEC_TEXT) {
                S->TextLen++;
            } else {
                S->DataLen++;
            }
            return SUCCESS;
        }
        if (StrCmp(Op, "dq") == 0) {
            S->DataLen += 8;
            return SUCCESS;
        }
        if (StrCmp(Op, "resb") == 0) {
            UINT64 N;
            if (AsmParseNumber(Arg1, &N) == SUCCESS) {
                S->BssLen += (USIZE)N;
            }
            return SUCCESS;
        }
        if (S->CurSec == SEC_TEXT) {
            S->TextLen += 16;
        }
        return SUCCESS;
    }

    if (StrCmp(Op, "db") == 0) {
        if (Arg1[0] == '"') {
            USIZE J = 1;
            while (Arg1[J] && Arg1[J] != '"') {
                if (S->CurSec == SEC_TEXT) {
                    AsmEmitText(S, (UINT8)Arg1[J]);
                } else {
                    AsmEmitData(S, (UINT8)Arg1[J]);
                }
                J++;
            }
        } else {
            UINT64 Val;
            if (AsmResolveExpr(S, Arg1, &Val) != SUCCESS) {
                return NOT_FOUND;
            }
            if (S->CurSec == SEC_TEXT) {
                AsmEmitText(S, (UINT8)Val);
            } else {
                AsmEmitData(S, (UINT8)Val);
            }
        }
        return SUCCESS;
    }

    if (StrCmp(Op, "dq") == 0) {
        UINT64 Val;
        if (AsmResolveExpr(S, Arg1, &Val) != SUCCESS) {
            return NOT_FOUND;
        }
        return AsmEmitDataU64(S, Val);
    }

    if (S->CurSec != SEC_TEXT) {
        return SUCCESS;
    }

    if (StrCmp(Op, "mov") == 0) {
        AsmReg Rd = AsmParseReg(Arg1);
        AsmReg Rs = AsmParseReg(Arg2);
        UINT64 Imm;
        if (Rd != REG_NONE && Rs != REG_NONE) {
            return AsmEmitMovRegReg(S, Rd, Rs);
        }
        if (Rd != REG_NONE && AsmResolveExpr(S, Arg2, &Imm) == SUCCESS) {
            return AsmEmitMovRegImm(S, Rd, Imm);
        }
        return INCORRECT_VALUE;
    }
    if (StrCmp(Op, "lea") == 0) {
        AsmReg Rd = AsmParseReg(Arg1);
        CHAR Inner[128];
        UINT64 Target;
        if (Rd == REG_NONE || Arg2[0] != '[') {
            return INCORRECT_VALUE;
        }
        StrnCpy(Inner, Arg2 + 1, sizeof(Inner) - 1);
        {
            CHAR *End = StrChr(Inner, ']');
            if (End) {
                *End = '\0';
            }
        }
        if (AsmResolveExpr(S, Inner, &Target) != SUCCESS) {
            return NOT_FOUND;
        }
        return AsmEmitLeaRip(S, Rd, Target);
    }
    if (StrCmp(Op, "push") == 0) {
        return AsmEmitPushPop(S, AsmParseReg(Arg1), TRUE);
    }
    if (StrCmp(Op, "pop") == 0) {
        return AsmEmitPushPop(S, AsmParseReg(Arg1), FALSE);
    }
    if (StrCmp(Op, "xor") == 0) {
        AsmReg Rd = AsmParseReg(Arg1);
        AsmReg Rs = AsmParseReg(Arg2);
        if (Rd != REG_NONE && Rs != REG_NONE) {
            if (AsmEmitRex(S, Rd, Rs, TRUE) != SUCCESS) {
                return NO_MEMORY;
            }
            AsmEmitText(S, 0x31);
            return AsmEmitText(S, (UINT8)(0xC0 | (AsmRegId(Rs) << 3) | AsmRegId(Rd)));
        }
        return AsmEmitAluRegImm(S, 6, Rd, 0);
    }
    if (StrCmp(Op, "add") == 0) {
        UINT64 Imm;
        if (AsmResolveExpr(S, Arg2, &Imm) == SUCCESS) {
            return AsmEmitAluRegImm(S, 0, AsmParseReg(Arg1), (UINT32)Imm);
        }
        return INCORRECT_VALUE;
    }
    if (StrCmp(Op, "sub") == 0) {
        UINT64 Imm;
        if (AsmResolveExpr(S, Arg2, &Imm) == SUCCESS) {
            return AsmEmitAluRegImm(S, 5, AsmParseReg(Arg1), (UINT32)Imm);
        }
        return INCORRECT_VALUE;
    }
    if (StrCmp(Op, "syscall") == 0) {
        AsmEmitText(S, 0x0F);
        return AsmEmitText(S, 0x05);
    }
    if (StrCmp(Op, "ret") == 0) {
        return AsmEmitText(S, 0xC3);
    }
    if (StrCmp(Op, "nop") == 0) {
        return AsmEmitText(S, 0x90);
    }
    if (StrCmp(Op, "jmp") == 0) {
        UINT64 Target;
        INT32 Rel;
        if (AsmResolveExpr(S, Arg1, &Target) != SUCCESS) {
            return NOT_FOUND;
        }
        Rel = (INT32)(Target - (TOSASM_LOAD_BASE + S->TextLen + 5));
        return AsmEmitJmpRel(S, Rel);
    }

    return NOT_IMPLEMENTED;
}

static INT AsmBuildElf(AsmState *S, UINT8 **OutElf, USIZE *OutSize) {
    USIZE TextSz = S->TextLen;
    USIZE DataSz = S->DataLen;
    
    // Высчитываем размер итогового файла с учетом ДВУХ программных заголовков (Elf64Phdr * 2)
    USIZE FileSize = sizeof(Elf64Ehdr) + (sizeof(Elf64Phdr) * 2) + TextSz + DataSz;
    UINT8 *Elf;
    Elf64Ehdr *Eh;
    Elf64Phdr *PhText;
    Elf64Phdr *PhData;
    UINT64 Entry = TOSASM_LOAD_BASE;
    INT I;

    // Ищем точку входа по Labels (Твой отличный оригинальный код)
    if (S->HasEntry) {
        for (I = 0; I < S->LabelCount; I++) {
            if (StrCmp(S->Labels[I].Name, S->EntryName) == 0 &&
                S->Labels[I].Section == SEC_TEXT) {
                Entry = TOSASM_LOAD_BASE + S->Labels[I].Offset;
                break;
            }
        }
    }

    Elf = (UINT8*)MemoryAllocate(FileSize);
    if (!Elf) {
        return NO_MEMORY;
    }
    MemSet(Elf, 0, FileSize);

    // Заполняем основной заголовок ELF64
    Eh = (Elf64Ehdr*)Elf;
    Eh->EIdent[EI_MAG0] = ELFMAG0;
    Eh->EIdent[EI_MAG1] = ELFMAG1;
    Eh->EIdent[EI_MAG2] = ELFMAG2;
    Eh->EIdent[EI_MAG3] = ELFMAG3;
    Eh->EIdent[EI_CLASS] = ELFCLASS64;
    Eh->EIdent[EI_DATA] = ELFDATA2LSB;
    Eh->EIdent[EI_VERSION] = EV_CURRENT;
    Eh->EType = ET_EXEC;
    Eh->EMachine = EM_X86_64;
    Eh->EVersion = EV_CURRENT;
    Eh->EEntry = Entry;
    Eh->EPhoff = sizeof(Elf64Ehdr);
    Eh->EPhentsize = sizeof(Elf64Phdr);
    Eh->EPhnum = 2; // Указываем ядру, что в файле ровно 2 сегмента LOAD

    // ========================================================================
    // СЕГМЕНТ 1: КОД (.text) — Исполняемый, но защищен от записи!
    // ========================================================================
    PhText = (Elf64Phdr*)(Elf + Eh->EPhoff);
    PhText->PType = PT_LOAD;
    PhText->PFlags = PF_R | PF_X; // Только чтение и выполнение (Без флага PF_W!)
    PhText->POffset = sizeof(Elf64Ehdr) + (sizeof(Elf64Phdr) * 2);
    PhText->PVaddr = TOSASM_LOAD_BASE;
    PhText->PPaddr = TOSASM_LOAD_BASE;
    PhText->PFilesz = TextSz;
    PhText->PMemsz = TextSz;
    PhText->PAlign = 0x1000; // Выравнивание по границе страницы 4КБ

    // ========================================================================
    // СЕГМЕНТ 2: ДАННЫЕ (.data + .bss) — Запись разрешена, но исполнение ЗАПРЕЩЕНО!
    // ========================================================================
    PhData = PhText + 1; // Переходим к следующей структуре заголовка в памяти
    PhData->PType = PT_LOAD;
    PhData->PFlags = PF_R | PF_W; // Только чтение и запись (Без флага PF_X, включит NX!)
    
    // Смещение в файле для данных начинается строго после байт секции кода
    PhData->POffset = PhText->POffset + TextSz;
    
    // Виртуальный адрес в памяти выравниваем по границе страницы (4КБ) строго после кода
    UINT64 DataVaddr = (TOSASM_LOAD_BASE + TextSz + 0xFFF) & ~0xFFFULL;
    PhData->PVaddr = DataVaddr;
    PhData->PPaddr = DataVaddr;
    PhData->PFilesz = DataSz;
    PhData->PMemsz = DataSz + S->BssLen; // Выделяем дополнительное место под .bss на лету
    PhData->PAlign = 0x1000;

    // Копируем бинарный код и данные по их вычисленным смещениям в файл
    MemCpy(Elf + PhText->POffset, S->Text, TextSz);
    if (DataSz > 0) {
        MemCpy(Elf + PhData->POffset, S->Data, DataSz);
    }

    *OutElf = Elf;
    *OutSize = FileSize;
    return SUCCESS;
}


INT TosAsmAssembleSource(const CHAR *Source, USIZE SourceLen,
                         UINT8 **OutElf, USIZE *OutSize) {
    AsmState S;
    CHAR Line[512];
    USIZE Pos = 0;
    USIZE LineStart;
    INT R;

    (void)SourceLen;
    if (!Source || !OutElf || !OutSize) {
        return INCORRECT_VALUE;
    }

    MemSet(&S, 0, sizeof(S));
    S.TextCap = TOSASM_MAX_OUTPUT;
    S.DataCap = TOSASM_MAX_OUTPUT;
    S.Text = (UINT8*)MemoryAllocate(S.TextCap);
    S.Data = (UINT8*)MemoryAllocate(S.DataCap);
    if (!S.Text || !S.Data) {
        MemoryFree(S.Text);
        MemoryFree(S.Data);
        return NO_MEMORY;
    }
    S.CurSec = SEC_TEXT;

    while (Source[Pos]) {
        LineStart = Pos;
        while (Source[Pos] && Source[Pos] != '\n' && Source[Pos] != '\r') {
            Pos++;
        }
        {
            USIZE Len = Pos - LineStart;
            if (Len >= sizeof(Line)) {
                Len = sizeof(Line) - 1;
            }
            MemCpy(Line, Source + LineStart, Len);
            Line[Len] = '\0';
        }
        R = AsmProcessLine(&S, Line, FALSE);
        if (R != SUCCESS && R != NOT_IMPLEMENTED) {
            MemoryFree(S.Text);
            MemoryFree(S.Data);
            return R;
        }
        if (Source[Pos] == '\r') {
            Pos++;
        }
        if (Source[Pos] == '\n') {
            Pos++;
        }
    }

    S.TextSize = S.TextLen;
    S.DataSize = S.DataLen;
    S.TextLen = 0;
    S.DataLen = 0;
    Pos = 0;
    while (Source[Pos]) {
        LineStart = Pos;
        while (Source[Pos] && Source[Pos] != '\n' && Source[Pos] != '\r') {
            Pos++;
        }
        {
            USIZE Len = Pos - LineStart;
            if (Len >= sizeof(Line)) {
                Len = sizeof(Line) - 1;
            }
            MemCpy(Line, Source + LineStart, Len);
            Line[Len] = '\0';
        }
        R = AsmProcessLine(&S, Line, TRUE);
        if (R != SUCCESS && R != NOT_IMPLEMENTED) {
            MemoryFree(S.Text);
            MemoryFree(S.Data);
            return R;
        }
        if (Source[Pos] == '\r') {
            Pos++;
        }
        if (Source[Pos] == '\n') {
            Pos++;
        }
    }

    R = AsmBuildElf(&S, OutElf, OutSize);
    MemoryFree(S.Text);
    MemoryFree(S.Data);
    return R;
}

INT TosAsmAssembleFile(VfsInode *BaseDir, const CHAR *SrcPath, const CHAR *OutPath) {
    VfsFile *InFile;
    VfsFile *OutFile;
    UINT8 *SrcData;
    UINT8 *ElfData;
    USIZE ElfSize;
    UINT64 FileSize;
    UINT32 Read, Written;
    INT Result;

    if (VfsOpen(BaseDir, SrcPath, O_READ, &InFile) != SUCCESS) {
        return NOT_FOUND;
    }

    FileSize = InFile->FInode->ISize;
    if (FileSize == 0 || FileSize > 256 * 1024) {
        VfsClose(InFile);
        return INCORRECT_VALUE;
    }

    SrcData = (UINT8*)MemoryAllocate((USIZE)FileSize + 1);
    if (!SrcData) {
        VfsClose(InFile);
        return NO_MEMORY;
    }

    if (VfsRead(InFile, SrcData, (UINT32)FileSize, &Read) != SUCCESS) {
        MemoryFree(SrcData);
        VfsClose(InFile);
        return IO_ERROR;
    }
    SrcData[Read] = '\0';
    VfsClose(InFile);

    Result = TosAsmAssembleSource((const CHAR*)SrcData, Read, &ElfData, &ElfSize);
    MemoryFree(SrcData);
    if (Result != SUCCESS) {
        ConsolePrint("Assemble failed (code %d)\n", Result);
        return Result;
    }

    if (VfsOpen(BaseDir, OutPath, O_WRITE | O_CREAT | O_TRUNC, &OutFile) != SUCCESS) {
        MemoryFree(ElfData);
        return IO_ERROR;
    }

    if (VfsWrite(OutFile, ElfData, (UINT32)ElfSize, &Written) != SUCCESS ||
        Written != ElfSize) {
        VfsClose(OutFile);
        MemoryFree(ElfData);
        return IO_ERROR;
    }

    VfsClose(OutFile);
    MemoryFree(ElfData);
    ConsolePrint("Assembled: %s -> %s (%u bytes ELF)\n", SrcPath, OutPath, (UINT32)ElfSize);
    return SUCCESS;
}
