#include <Apic.h>
#include <Asm/Mmio.h>
#include <Kernel/KDriver.h>
#include <Asm/Cpu.h>
#include <Acpi.h>
#include <Kernel/Return.h>
#include <Kernel/KDriver.h>

static struct {
    UINT32 Base;
    volatile NOPTR *Virt;
    BOOL Enabled;
    BOOL X2ApicMode;
} GApic = {0};

static BOOL ApicBaseIsEnabled(NOPTR) {
    return (ReadMSR(IA32_APIC_BASE_MSR) & APIC_BASE_ENABLE) != 0;
}

static BOOL ApicBaseIsX2Apic(NOPTR) {
    return (ReadMSR(IA32_APIC_BASE_MSR) & APIC_BASE_X2APIC) != 0;
}

static BOOL ApicTransitionToX2Apic(UINT32 BaseAddr) {
    UINT64 Base = ReadMSR(IA32_APIC_BASE_MSR);

    if (Base & APIC_BASE_X2APIC) {
        return TRUE;
    }

    /*
     * Intel SDM: x2APIC can only be entered from an enabled xAPIC state.
     * Never clear APIC_BASE_ENABLE before setting APIC_BASE_X2APIC.
     */
    if (!(Base & APIC_BASE_ENABLE)) {
        Base |= APIC_BASE_ENABLE;
        Base = (Base & ~APIC_BASE_ADDR_MASK) | ((UINT64)BaseAddr & APIC_BASE_ADDR_MASK);
        WriteMSR(IA32_APIC_BASE_MSR, Base);
    }

    Base = ReadMSR(IA32_APIC_BASE_MSR);
    Base |= APIC_BASE_X2APIC;      // Бит 10
    Base |= APIC_BASE_ENABLE;       // Бит 11
    Base &= ~0xFFFFFFFFFFFFFC00ULL;
    WriteMSR(IA32_APIC_BASE_MSR, Base);

    return (ReadMSR(IA32_APIC_BASE_MSR) & APIC_BASE_X2APIC) != 0;
}

static UINT32 ApicOffsetToMsr(UINT32 Reg) {
    return X2APIC_OFFSET_TO_MSR(Reg);
}

static BOOL ApicIsIcrReg(UINT32 Reg) {
    return Reg == LAPIC_ICR_LOW || Reg == LAPIC_ICR_HIGH;
}

static UINT64 ApicReadIcr(NOPTR) {
    return ReadMSR(X2APIC_MSR_ICR);
}

static NOPTR ApicWriteIcr(UINT64 Value) {
    WriteMSR(X2APIC_MSR_ICR, Value);
}

static BOOL ApicIcrIsPending(NOPTR) {
    if (GApic.X2ApicMode) {
        return (ApicReadIcr() & ICR_DELIVERY_PENDING) != 0;
    }
    return (MmioRead32(GApic.Virt + LAPIC_ICR_LOW) & ICR_DELIVERY_PENDING) != 0;
}

static NOPTR ApicWaitIcr(NOPTR) {
    INT Timeout = 100000;
    while (ApicIcrIsPending() && Timeout--) {
        CpuPause();
    }
}

BOOL ApicCpuSupportsX2Apic(NOPTR) {
    UINT32 Eax, Ebx, Ecx, Edx;
    Cpuid(1, &Eax, &Ebx, &Ecx, &Edx);
    return (Ecx & (1 << 21)) != 0;
}

BOOL ApicIsX2ApicMode(NOPTR) {
    return GApic.X2ApicMode;
}

UINT32 ApicFormatIoapicDestination(UINT32 ApicId) {
    if (GApic.X2ApicMode) {
        return ApicId;
    }
    return ApicId << 24;
}

INT ApicInit(UINT32 BaseAddr) {
    if (!BaseAddr) {
        RETURN(NO_OBJECT);
    }

    GApic.Base = BaseAddr;
    GApic.Virt = (volatile NOPTR*)(UINTPTR)BaseAddr;
    GApic.Enabled = FALSE;
    GApic.X2ApicMode = FALSE;

    BOOL supportsX2 = ApicCpuSupportsX2Apic();
    
    if (supportsX2) {
        BOOL isX2 = ApicBaseIsX2Apic();
        
        if (isX2) {
            GApic.X2ApicMode = TRUE;
        } else {
            if (ApicTransitionToX2Apic(BaseAddr)) {
                GApic.X2ApicMode = TRUE;
            }
        }
    }

    UINT32 Ver = ApicReadReg(LAPIC_VERSION);   
    if ((Ver & 0xFF) == 0) {
        RETURN(INCORRECT_VALUE);
    }

    RETURN(SUCCESS);
}

UINT32 ApicReadReg(UINT32 Reg) {
    if (GApic.X2ApicMode) {
        if (ApicIsIcrReg(Reg)) {
            UINT64 Icr = ApicReadIcr();
            if (Reg == LAPIC_ICR_LOW) {
                return (UINT32)Icr;
            }
            return (UINT32)(Icr >> ICR_DEST_FIELD_SHIFT);
        }
        if (Reg == LAPIC_ID) {
            return (UINT32)ReadMSR(X2APIC_MSR_ID);
        }
        return (UINT32)ReadMSR(ApicOffsetToMsr(Reg));
    }

    if (!GApic.Virt) return 0;
    return MmioRead32(GApic.Virt + Reg);
}

NOPTR ApicWriteReg(UINT32 Reg, UINT32 Val) {
    if (GApic.X2ApicMode) {
        if (Reg == LAPIC_ICR_LOW) {
            UINT64 Icr = ApicReadIcr();
            Icr = (Icr & (0xFFFFFFFFULL << ICR_DEST_FIELD_SHIFT)) | Val;
            ApicWriteIcr(Icr);
            return;
        }
        if (Reg == LAPIC_ICR_HIGH) {
            UINT64 Icr = ApicReadIcr();
            Icr = (Icr & 0xFFFFFFFFULL) | ((UINT64)Val << ICR_DEST_FIELD_SHIFT);
            ApicWriteIcr(Icr);
            return;
        }
        WriteMSR(ApicOffsetToMsr(Reg), Val);
        return;
    }

    if (!GApic.Virt) return;
    MmioWrite32(GApic.Virt + Reg, Val);
}

NOPTR ApicEnable(NOPTR) {
    if (!GApic.Base && !GApic.X2ApicMode) return;

    if (!GApic.X2ApicMode && !ApicBaseIsEnabled()) {
        UINT64 Base = ReadMSR(IA32_APIC_BASE_MSR);
        Base |= APIC_BASE_ENABLE;
        Base = (Base & ~APIC_BASE_ADDR_MASK) | (GApic.Base & APIC_BASE_ADDR_MASK);
        WriteMSR(IA32_APIC_BASE_MSR, Base);
    }

    UINT32 Svr = ApicReadReg(LAPIC_SVR);
    Svr |= (1 << 8);
    Svr &= ~0xFF;
    Svr |= 0xFF;
    ApicWriteReg(LAPIC_SVR, Svr);

    ApicWriteReg(LAPIC_LVT_LINT0, LVT_MASKED);
    ApicWriteReg(LAPIC_LVT_LINT1, LVT_MASKED);
    ApicWriteReg(LAPIC_TPR, 0);

    GApic.Enabled = TRUE;
}

NOPTR ApicDisable(NOPTR) {
    if (!GApic.Enabled) return;

    UINT32 Svr = ApicReadReg(LAPIC_SVR);
    Svr &= ~(1 << 8);
    ApicWriteReg(LAPIC_SVR, Svr);

    if (GApic.X2ApicMode) {
        UINT64 Base = ReadMSR(IA32_APIC_BASE_MSR);
        Base &= ~APIC_BASE_ENABLE;
        WriteMSR(IA32_APIC_BASE_MSR, Base);
    } else if (GApic.Base) {
        UINT64 Base = ReadMSR(IA32_APIC_BASE_MSR);
        Base &= ~APIC_BASE_ENABLE;
        WriteMSR(IA32_APIC_BASE_MSR, Base);
    }

    GApic.Enabled = FALSE;
}

NOPTR ApicEoi(NOPTR) {
    if (!GApic.Enabled) return;
    ApicWriteReg(LAPIC_EOI, 0);
}

UINT32 ApicGetId(NOPTR) {
    if (GApic.X2ApicMode) {
        return (UINT32)ReadMSR(X2APIC_MSR_ID);
    }
    UINT32 Id = ApicReadReg(LAPIC_ID);
    return (Id >> 24) & 0xFF;
}

UINT32 ApicGetVersion(NOPTR) {
    return ApicReadReg(LAPIC_VERSION) & 0xFF;
}

NOPTR ApicSendIpi(UINT32 ApicId, UINT32 Vector) {
    if (GApic.X2ApicMode) {
        ApicWaitIcr();
        ApicWriteIcr(((UINT64)ApicId << ICR_DEST_FIELD_SHIFT) |
                     (Vector & 0xFF) | DELIVERY_FIXED | ICR_DEST_PHYSICAL);
        return;
    }

    ApicWaitIcr();
    ApicWriteReg(LAPIC_ICR_HIGH, ApicId << 24);
    ApicWriteReg(LAPIC_ICR_LOW, Vector | DELIVERY_FIXED);
}

NOPTR ApicSendBroadcast(UINT32 Vector) {
    if (GApic.X2ApicMode) {
        ApicWaitIcr();
        ApicWriteIcr((Vector & 0xFF) | DELIVERY_FIXED | ICR_DEST_ALL);
        return;
    }

    ApicWaitIcr();
    ApicWriteReg(LAPIC_ICR_LOW, Vector | DELIVERY_FIXED | ICR_DEST_ALL);
}

NOPTR ApicSendInit(UINT32 ApicId) {
    if (GApic.X2ApicMode) {
        ApicWaitIcr();
        ApicWriteIcr(((UINT64)ApicId << ICR_DEST_FIELD_SHIFT) |
                     DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_DEST_PHYSICAL);
        ApicWaitIcr();
        ApicWriteIcr(((UINT64)ApicId << ICR_DEST_FIELD_SHIFT) |
                     DELIVERY_INIT | ICR_LEVEL_DEASSERT | ICR_DEST_PHYSICAL);
        ApicWaitIcr();
        return;
    }

    ApicWaitIcr();
    ApicWriteReg(LAPIC_ICR_HIGH, ApicId << 24);
    ApicWriteReg(LAPIC_ICR_LOW, DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_DEST_PHYSICAL);
    ApicWaitIcr();
    ApicWriteReg(LAPIC_ICR_HIGH, ApicId << 24);
    ApicWriteReg(LAPIC_ICR_LOW, DELIVERY_INIT | ICR_LEVEL_DEASSERT | ICR_DEST_PHYSICAL);
    ApicWaitIcr();
}

NOPTR ApicSendStartup(UINT32 ApicId, UINT32 Vector) {
    if (GApic.X2ApicMode) {
        ApicWaitIcr();
        ApicWriteIcr(((UINT64)ApicId << ICR_DEST_FIELD_SHIFT) |
                     ((Vector & 0xFF) | DELIVERY_STARTUP | ICR_DEST_PHYSICAL));
        ApicWaitIcr();
        return;
    }

    ApicWaitIcr();
    ApicWriteReg(LAPIC_ICR_HIGH, ApicId << 24);
    ApicWriteReg(LAPIC_ICR_LOW, (Vector & 0xFF) | DELIVERY_STARTUP | ICR_DEST_PHYSICAL);
    ApicWaitIcr();
}
