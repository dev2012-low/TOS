#include <Kernel/KDriver.h>
#include <Kernel/Return.h>
#include <Lib/String.h>

static ListHead GKDriverList;
static UINT32 GKDriverCount = 0;

static UINT8 GKDriverPrivatePool[8192];
static UINT32 GKDriverPrivateOffset = 0;

NOPTR KDriverInit(NOPTR) {
    ListInit(&GKDriverList);
    GKDriverCount = 0;
}

INT KDriverRegister(KDriver *Driver) {
    if (Driver == NULLPTR) RETURN(NO_OBJECT);
    if (KDriverFindByName(Driver->Name)) {
	RETURN(ALREADY_EXISTS);
    }
    ListAddTail(&GKDriverList, &Driver->Node);
    GKDriverCount++;
    Driver->Initialized = FALSE;
    RETURN(SUCCESS);
}

INT KDriverUnregister(KDriver *Driver) {
    if (Driver == NULLPTR) RETURN(NO_OBJECT);
    if (Driver->Shutdown != NULLPTR) {
        Driver->Shutdown(Driver);
    }
    ListDel(&Driver->Node);
    GKDriverCount--;
    RETURN(SUCCESS);
}
KDriver* KDriverGenerateStruct(const CHAR *Name, UINT8 DCL, BOOL Initialized, NOPTR *Priv, KDriverCallback ShutdownCallback) {
    UINT8 StaticDCL = DCL;
    if (DCL > 2) StaticDCL = 1;
    
    // Выделяем память из пула или через MemoryAllocate
    KDriver *StaticDriver = (KDriver*)&GKDriverPrivatePool[GKDriverPrivateOffset];
    if (GKDriverPrivateOffset + sizeof(KDriver) > 8192) {
        return NULLPTR;  // Пул переполнен
    }
    GKDriverPrivateOffset += sizeof(KDriver);
    
    StrCpy(StaticDriver->Name, Name);
    StaticDriver->DCL = StaticDCL;
    StaticDriver->Initialized = Initialized;
    StaticDriver->Priv = Priv;
    StaticDriver->Shutdown = ShutdownCallback;
    
    return StaticDriver;
}

KDriver* KDriverFindByName(const CHAR *Name) {
    ListHead *Pos;
    ListForEach(Pos, &GKDriverList) {
        KDriver *Driver = ListEntry(Pos, KDriver, Node);
	if (StrCmp(Driver->Name, Name) == 0) {
	    return Driver;
	}
    }
    return NULLPTR;
}

UINT32 KDriverGetCount(NOPTR) {
    return GKDriverCount;
}

NOPTR* KDriverGetPrivate(KDriver *Driver, USIZE Size) {
    if (Driver == NULLPTR) return NULLPTR;
    
    // Если данные уже выделены, просто возвращаем их
    if (Driver->Priv != NULLPTR) {
        return Driver->Priv;
    }
    
    // Проверяем, хватит ли места в пуле
    if (GKDriverPrivateOffset + Size > 8192) {
        return NULLPTR; 
    }
    
    // Выделяем из пула
    Driver->Priv = (NOPTR*)&GKDriverPrivatePool[GKDriverPrivateOffset];
    GKDriverPrivateOffset += Size;
    
    // Обнуляем память
    MemSet(Driver->Priv, 0, Size);
    
    return Driver->Priv;
}
