#include <Hda.h>
#include <HdaRegs.h>
#include <Pci.h>
#include <Kernel/KDriver.h>
#include <Kernel/Idt.h>
#include <Kernel/Return.h>
#include <Apic.h>
#include <Ioapic.h>
#include <Asm/Mmio.h>
#include <Asm/Cpu.h>
#include <Console.h>
#include <Lib/String.h>
#include <Time/Timer.h>

#define HDA_POLL_US           100
#define HDA_RESET_MS          100
static HdaController GHda;
static KDriver *GHdaDriver;

static UINT32 __attribute__((aligned(128))) GCorb[HDA_CORB_SIZE];
static UINT64 __attribute__((aligned(128))) GRirb[HDA_RIRB_SIZE];
static HdaBdlEntry __attribute__((aligned(16))) GBdl[HDA_BDL_ENTRIES];
static INT16 __attribute__((aligned(4096))) GPcmBuffer[HDA_PCM_BUF_SIZE / 2];

static volatile UINT32 GCorbWp;
static volatile UINT32 GRirbWpLast;
static volatile BOOL GRirbResponseReady;
static volatile UINT64 GRirbResponse;

static inline volatile UINT32 *HdaReg32(UINT32 Off) {
    return (volatile UINT32 *)(GHda.Mmio + Off);
}

static inline volatile UINT16 *HdaReg16(UINT32 Off) {
    return (volatile UINT16 *)(GHda.Mmio + Off);
}

static inline volatile UINT8 *HdaReg8(UINT32 Off) {
    return (volatile UINT8 *)(GHda.Mmio + Off);
}

static UINT32 HdaMakeVerb(UINT8 Codec, UINT8 Node, UINT16 Verb, UINT16 Param) {
    return ((UINT32)Codec << 28) | ((UINT32)Node << 20) |
           ((UINT32)Verb << 8) | (Param & 0xFF);
}

static BOOL HdaPollMs(UINT32 Ms, BOOL (*Ready)(NOPTR)) {
    for (UINT32 I = 0; I < Ms * 10; I++) {
        if (Ready()) {
            return TRUE;
        }
        TimerUdelay(HDA_POLL_US);
    }
    return FALSE;
}

static BOOL HdaResetDone(NOPTR) {
    return (MmioRead32(HdaReg32(HDA_REG_GCTL)) & HDA_GCTL_CRST) == 0;
}

static INT HdaControllerReset(NOPTR) {
    UINT32 Gctl = MmioRead32(HdaReg32(HDA_REG_GCTL));
    Gctl &= ~HDA_GCTL_CRST;
    MmioWrite32(HdaReg32(HDA_REG_GCTL), Gctl);
    Gctl |= HDA_GCTL_CRST;
    MmioWrite32(HdaReg32(HDA_REG_GCTL), Gctl);

    if (!HdaPollMs(HDA_RESET_MS, HdaResetDone)) {
        return TIMEOUT;
    }

    Gctl &= ~HDA_GCTL_CRST;
    MmioWrite32(HdaReg32(HDA_REG_GCTL), Gctl);
    if (!HdaPollMs(HDA_RESET_MS, HdaResetDone)) {
        return TIMEOUT;
    }
    return SUCCESS;
}

static INT HdaInitCorbRirb(NOPTR) {
    UINT64 CorbPhys = (UINT64)(UINTPTR)GCorb;
    UINT64 RirbPhys = (UINT64)(UINTPTR)GRirb;

    MmioWrite32(HdaReg32(HDA_REG_CORBCTL), 0);
    MmioWrite32(HdaReg32(HDA_REG_RIRBCTL), 0);

    MmioWrite32(HdaReg32(HDA_REG_CORBLBASE), (UINT32)CorbPhys);
    MmioWrite32(HdaReg32(HDA_REG_CORBUBASE), (UINT32)(CorbPhys >> 32));
    MmioWrite16(HdaReg16(HDA_REG_CORBRP), 0);
    MmioWrite16(HdaReg16(HDA_REG_CORBWP), 0);
    GCorbWp = 0;

    MmioWrite32(HdaReg32(HDA_REG_RIRBLBASE), (UINT32)RirbPhys);
    MmioWrite32(HdaReg32(HDA_REG_RIRBUBASE), (UINT32)(RirbPhys >> 32));
    MmioWrite16(HdaReg16(HDA_REG_RIRBRP), 0);
    MmioWrite16(HdaReg16(HDA_REG_RIRBWP), 0);
    GRirbWpLast = 0;

    MmioWrite16(HdaReg16(HDA_REG_RINTCNT), 1);
    MmioWrite8(HdaReg8(HDA_REG_RIRBCTL), HDA_RIRBCTL_IRQ_EN);
    MmioWrite8(HdaReg8(HDA_REG_RIRBCTL), HDA_RIRBCTL_IRQ_EN | HDA_RIRBCTL_RUN);

    MmioWrite8(HdaReg8(HDA_REG_CORBCTL), HDA_CORBCTL_CMEIE | HDA_CORBCTL_RUN);

    MmioWrite32(HdaReg32(HDA_REG_INTCTL), 0xC0000000u);
    return SUCCESS;
}

static NOPTR HdaDrainRirb(NOPTR) {
    UINT16 RirbWp = MmioRead16(HdaReg16(HDA_REG_RIRBWP));
    while (GRirbWpLast != RirbWp) {
        GRirbResponse = GRirb[GRirbWpLast];
        GRirbWpLast = (GRirbWpLast + 1) % HDA_RIRB_SIZE;
        GRirbResponseReady = TRUE;
    }
}

static INT HdaSendVerb(UINT32 Verb, UINT64 *Response) {
    UINT16 CorbWp = MmioRead16(HdaReg16(HDA_REG_CORBWP));
    UINT32 Next = (CorbWp + 1) % HDA_CORB_SIZE;

    GRirbResponseReady = FALSE;
    GCorb[CorbWp] = Verb;
    __sync_synchronize();
    MmioWrite16(HdaReg16(HDA_REG_CORBWP), (UINT16)Next);

    for (UINT32 I = 0; I < 5000; I++) {
        HdaDrainRirb();
        if (GRirbResponseReady) {
            break;
        }
        TimerUdelay(HDA_POLL_US);
    }

    if (!GRirbResponseReady) {
        return TIMEOUT;
    }

    if (Response) {
        *Response = GRirbResponse;
    }
    GRirbResponseReady = FALSE;
    return SUCCESS;
}

static INT HdaGetParam(UINT8 Codec, UINT8 Node, UINT16 Param, UINT32 *Out) {
    UINT32 Verb = HdaMakeVerb(Codec, Node, HDA_VERB_GET_PARAM, Param);
    UINT64 Resp;
    INT Ret = HdaSendVerb(Verb, &Resp);
    if (Ret != SUCCESS) {
        return Ret;
    }
    if (Out) {
        *Out = (UINT32)Resp;
    }
    return SUCCESS;
}

static UINT8 HdaWidgetType(UINT32 Cap) {
    return (UINT8)((Cap >> 20) & 0x0F);
}

static INT HdaCodecDiscover(HdaCodecInfo *Codec) {
    UINT32 Vendor = 0;
    UINT32 Subsys = 0;
    UINT32 NodeCount = 0;

    if (HdaGetParam(Codec->Addr, 0, HDA_PARAM_VENDOR, &Vendor) != SUCCESS) {
        return NOT_FOUND;
    }
    if (Vendor == 0 || Vendor == 0xFFFFFFFF) {
        return NOT_FOUND;
    }

    Codec->VendorId = Vendor;
    Codec->RootNode = 0;
    HdaGetParam(Codec->Addr, 0, HDA_PARAM_SUBSYS, &Subsys);
    Codec->SubsystemId = Subsys;

    for (UINT8 Node = 1; Node < HDA_MAX_WIDGETS; Node++) {
        UINT32 Cap = 0;
        if (HdaGetParam(Codec->Addr, Node, HDA_PARAM_WIDGET_CAP, &Cap) != SUCCESS) {
            continue;
        }
        if (HdaWidgetType(Cap) == 0x0B) {
            Codec->AfgNode = Node;
            HdaGetParam(Codec->Addr, Node, HDA_PARAM_NODE_COUNT, &NodeCount);
            NodeCount = (NodeCount & 0xFF) + 1;
            break;
        }
    }

    if (!Codec->AfgNode) {
        Codec->AfgNode = 1;
        NodeCount = 32;
    }

    UINT32 PcmCaps;
    HdaGetParam(Codec->Addr, Codec->AfgNode, HDA_PARAM_PCM, &PcmCaps);
    Codec->SupportedPcmRates = (PcmCaps >> 16) & 0xFFFF;
    Codec->SupportedPcmSizes = (PcmCaps >> 0) & 0xFFFF;

    for (UINT8 Node = Codec->AfgNode + 1;
         Node < Codec->AfgNode + 1 + (UINT8)NodeCount && Node < HDA_MAX_WIDGETS;
         Node++) {
        UINT32 Cap = 0;
        UINT32 PinCaps = 0;
        if (HdaGetParam(Codec->Addr, Node, HDA_PARAM_WIDGET_CAP, &Cap) != SUCCESS) {
            continue;
        }
        UINT8 Type = HdaWidgetType(Cap);
        if (Type == 0x04) {
            HdaGetParam(Codec->Addr, Node, HDA_PARAM_PIN_CAPS, &PinCaps);
            if ((PinCaps & 0x10) == 0) {
                continue;
            }
            Codec->OutPinNode = Node;
        } else if (Type == 0x00 && !Codec->OutConverterNode) {
    	    Codec->OutConverterNode = Node;
    	    // Проверяем, есть ли у этого нода усилитель
    	    if (Cap & (1 << 7)) {  // бит 7 = Amplifier Present
       		Codec->OutAmpNode = Node;
        	// Читаем усилитель
        	UINT32 AmpCap;
        	HdaGetParam(Codec->Addr, Node, 0x12, &AmpCap); // GET_AMP_CAPS
        	// Парсим Capabilities
        	Codec->OutAmpCapabilities = AmpCap;
        	Codec->OutAmpMin = (AmpCap >> 0) & 0x7F;
        	Codec->OutAmpMax = (AmpCap >> 8) & 0x7F;
        	Codec->OutAmpStep = (AmpCap >> 16) & 0x7F;
    	    }
        }
    }

    if (!Codec->OutPinNode && Codec->OutConverterNode) {
        Codec->OutPinNode = Codec->OutConverterNode;
    }
    if (!Codec->OutPinNode) {
        Codec->OutPinNode = 4;
        Codec->OutConverterNode = 3;
    }

    Codec->Present = TRUE;
    return SUCCESS;
}

static INT HdaCodecPowerUp(HdaCodecInfo *Codec) {
    UINT32 Verb;
    UINT64 Resp;

    Verb = HdaMakeVerb(Codec->Addr, Codec->AfgNode, HDA_VERB_SET_POWER,
                       (HDA_POWER_D0 << 8) | HDA_POWER_D0);
    if (HdaSendVerb(Verb, &Resp) != SUCCESS) {
        return DEVICE_ERROR;
    }

    Verb = HdaMakeVerb(Codec->Addr, Codec->OutPinNode, HDA_VERB_SET_POWER,
                       (HDA_POWER_D0 << 8) | HDA_POWER_D0);
    HdaSendVerb(Verb, NULLPTR);

    if (Codec->OutConverterNode && Codec->OutConverterNode != Codec->OutPinNode) {
        Verb = HdaMakeVerb(Codec->Addr, Codec->OutConverterNode, HDA_VERB_SET_POWER,
                           (HDA_POWER_D0 << 8) | HDA_POWER_D0);
        HdaSendVerb(Verb, NULLPTR);
    }

    Verb = HdaMakeVerb(Codec->Addr, Codec->OutPinNode, HDA_VERB_SET_PIN, 0xC0);
    HdaSendVerb(Verb, NULLPTR);

    UINT8 Stream = GHda.OutputStream;
    Verb = HdaMakeVerb(Codec->Addr, Codec->OutConverterNode, HDA_VERB_SET_STREAM,
                       ((UINT16)Stream << 4) | 0);
    HdaSendVerb(Verb, NULLPTR);

    Verb = HdaMakeVerb(Codec->Addr, Codec->OutConverterNode, HDA_VERB_SET_AMP_GAIN,
                       0xB000);
    HdaSendVerb(Verb, NULLPTR);

    Verb = HdaMakeVerb(Codec->Addr, Codec->OutPinNode, HDA_VERB_SET_AMP_GAIN, 0xE000);
    HdaSendVerb(Verb, NULLPTR);

    return SUCCESS;
}

static INT HdaSetupPlayback(NOPTR) {
    UINT8 Sd = GHda.OutputStream;
    UINT32 SdBase = HDA_SD_BASE(Sd);
    UINT64 PcmPhys = (UINT64)(UINTPTR)GPcmBuffer;
    UINT32 BytesPerPeriod = HDA_PCM_BUF_SIZE / HDA_BDL_ENTRIES;

    for (INT I = 0; I < HDA_BDL_ENTRIES; I++) {
        GBdl[I].Addr = (UINT32)(PcmPhys + (UINT64)I * BytesPerPeriod);
        GBdl[I].AddrU = (UINT32)((PcmPhys + (UINT64)I * BytesPerPeriod) >> 32);
        GBdl[I].Length = BytesPerPeriod | (I == HDA_BDL_ENTRIES - 1 ? HDA_BDL_IOC : 0);
        GBdl[I].Flags = 0;
    }

    volatile UINT32 *SdRegs = (volatile UINT32 *)(GHda.Mmio + SdBase);
    volatile UINT16 *SdRegs16 = (volatile UINT16 *)(GHda.Mmio + SdBase);

    MmioWrite32(&SdRegs[HDA_SD_CTL_OFF / 4], HDA_SD_CTL_RESET);
    TimerUdelay(1000);
    MmioWrite32(&SdRegs[HDA_SD_CTL_OFF / 4], 0);

    MmioWrite32(&SdRegs[HDA_SD_CBL_OFF / 4], HDA_PCM_BUF_SIZE - 1);
    MmioWrite16(&SdRegs16[HDA_SD_LVI_OFF / 2], HDA_BDL_ENTRIES - 1);
    MmioWrite16(&SdRegs16[HDA_SD_FMT_OFF / 2], (UINT16)HDA_FMT_48K_16_STEREO);

    UINT64 BdlPhys = (UINT64)(UINTPTR)GBdl;
    MmioWrite32(&SdRegs[HDA_SD_BDPL_OFF / 4], (UINT32)BdlPhys);
    MmioWrite32(&SdRegs[HDA_SD_BDPU_OFF / 4], (UINT32)(BdlPhys >> 32));

    MmioWrite32(&SdRegs[HDA_SD_CTL_OFF / 4], HDA_SD_CTL_RUN);
    return SUCCESS;
}

static NOPTR HdaFillTestTone(NOPTR) {
    const UINT32 SampleRate = 48000;
    const UINT32 Freq = 440;
    const INT16 Amplitude = 6000;
    const UINT32 Period = SampleRate / Freq;
    const UINT32 TotalSamples = HDA_PCM_BUF_SIZE / 4;

    for (UINT32 I = 0; I < TotalSamples; I++) {
        INT16 Sample = (INT16)(((I % Period) < (Period / 2)) ? Amplitude : -Amplitude);
        GPcmBuffer[I * 2] = Sample;
        GPcmBuffer[I * 2 + 1] = Sample;
    }
}

static INT HdaDetectOutputStream(NOPTR) {
    UINT32 Gcap = MmioRead32(HdaReg32(HDA_REG_GCAP));
    UINT8 Bis = (UINT8)(Gcap & 0x0F);
    UINT8 Iss = (UINT8)((Gcap >> 4) & 0x0F);
    UINT8 Oss = (UINT8)((Gcap >> 8) & 0x0F);
    if (Oss == 0) {
        return NOT_FOUND;
    }
    GHda.OutputStream = Bis + Iss + 1;
    return SUCCESS;
}

static INT HdaSetupIrq(NOPTR) {
    IdtSetGate(HDA_IRQ, HdaIrqHandler, KERNEL_CODE_SEL, IDT_GATE_INT, 0);

    if (PciEnableMsi(GHda.PciDev, HDA_IRQ, ApicGetId()) == SUCCESS) {
        GHda.MsiEnabled = TRUE;
        GHda.IrqGsi = 0;
        return SUCCESS;
    }

    UINT8 Irq = GHda.PciDev->InterruptLine;
    if (Irq == 0xFF) {
        return NOT_SUPPORTED;
    }

    UINT32 Flags;
    if (IoapicGetOverride(Irq, &GHda.IrqGsi, &Flags) != 0) {
        GHda.IrqGsi = Irq;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }

    if (IoapicRedirectIrq(GHda.IrqGsi, HDA_IRQ, ApicGetId(), Flags) != 0) {
        return IO_ERROR;
    }
    IoapicUnmaskIrq(GHda.IrqGsi);
    GHda.MsiEnabled = FALSE;
    return SUCCESS;
}

NOPTR HdaIrqHandler(NOPTR) {
    UINT32 IntSts = MmioRead32(HdaReg32(HDA_REG_INTSTS));
    if (IntSts & (1u << 31)) {
        UINT16 RirbWp = MmioRead16(HdaReg16(HDA_REG_RIRBWP));
        while (GRirbWpLast != RirbWp) {
            GRirbResponse = GRirb[GRirbWpLast];
            GRirbWpLast = (GRirbWpLast + 1) % HDA_RIRB_SIZE;
            GRirbResponseReady = TRUE;
        }
        MmioWrite8(HdaReg8(HDA_REG_RIRBSTS), HDA_RIRBSTS_IRQ);
    }

    UINT8 Sd = GHda.OutputStream;
    if (Sd < 8 && (IntSts & (1u << Sd))) {
        volatile UINT8 *Sts = HdaReg8(HDA_SD_BASE(Sd) + HDA_SD_STS_OFF);
        if (*Sts & HDA_SD_STS_BCIS) {
            *Sts = HDA_SD_STS_BCIS;
        }
    }

    ApicEoi();
    if (!GHda.MsiEnabled && GHda.IrqGsi) {
        IoapicEoi(GHda.IrqGsi);
    }
}

BOOL HdaIsReady(NOPTR) {
    return GHda.Initialized;
}

HdaController *HdaGetController(NOPTR) {
    return GHda.Initialized ? &GHda : NULLPTR;
}

NOPTR HdaPrintInfo(NOPTR) {
    if (!GHda.Initialized) {
        ConsolePrint("[HDA] not initialized\n");
        return;
    }

    ConsolePrint("[HDA] streams: out SD%u | codecs: %u\n",
                 GHda.OutputStream, GHda.CodecCount);
    for (UINT8 I = 0; I < GHda.CodecCount; I++) {
        HdaCodecInfo *C = &GHda.Codecs[I];
        ConsolePrint("[HDA]  codec %u: VID=0x%08X pin=%u conv=%u\n",
                     C->Addr, C->VendorId, C->OutPinNode, C->OutConverterNode);
    }
}

INT HdaPlayTestTone(NOPTR) {
    if (!GHda.PlaybackReady) {
        return DEVICE_ERROR;
    }
    HdaFillTestTone();
    return SUCCESS;
}

INT HdaInit(NOPTR) {
    INT Ret;

    MemSet(&GHda, 0, sizeof(GHda));
    MemSet(GCorb, 0, sizeof(GCorb));
    MemSet(GRirb, 0, sizeof(GRirb));

    GHda.PciDev = PciFindClass(0x04, 0x03);
    if (!GHda.PciDev) {
        RETURN(NOT_FOUND);
    }

    if (GHda.PciDev->Bars[0] == 0) {
        RETURN(NO_OBJECT);
    }

    PciEnable(GHda.PciDev);
    PciEnableBusmaster(GHda.PciDev);

    GHda.Mmio = (volatile UINT8 *)(UINTPTR)(GHda.PciDev->Bars[0] & ~0xFULL);

    Ret = HdaControllerReset();
    if (Ret != SUCCESS) {
        RETURN(Ret);
    }

    Ret = HdaDetectOutputStream();
    if (Ret != SUCCESS) {
        RETURN(Ret);
    }

    Ret = HdaInitCorbRirb();
    if (Ret != SUCCESS) {
        RETURN(Ret);
    }

    HdaSetupIrq();

    UINT16 StateSts = MmioRead16(HdaReg16(HDA_REG_STATESTS));
    for (UINT8 C = 0; C < HDA_MAX_CODECS; C++) {
        if (!(StateSts & (1u << C))) {
            continue;
        }
        HdaCodecInfo *Codec = &GHda.Codecs[GHda.CodecCount];
        Codec->Addr = C;
        if (HdaCodecDiscover(Codec) == SUCCESS) {
            GHda.CodecCount++;
        }
    }

    if (GHda.CodecCount == 0) {
        RETURN(NOT_FOUND);
    }

    Ret = HdaCodecPowerUp(&GHda.Codecs[0]);
    if (Ret != SUCCESS) {
        RETURN(Ret);
    }

    Ret = HdaSetupPlayback();
    if (Ret != SUCCESS) {
        RETURN(Ret);
    }

    GHda.PlaybackReady = TRUE;
    GHda.Initialized = TRUE;

    if (!GHdaDriver) {
        GHdaDriver = KDriverGenerateStruct("HDA", DCL1, TRUE, NULLPTR, NULLPTR);
        if (GHdaDriver) {
            KDriverRegister(GHdaDriver);
        }
    }

    RETURN(SUCCESS);
}

INT HdaSetVolume(HdaController *Ctl, INT CodecIdx, INT Percent) {
    // Percent: 0..100
    HdaCodecInfo *Codec = &Ctl->Codecs[CodecIdx];
    if (!Codec->OutAmpNode) return -1;
    
    INT Gain = Codec->OutAmpMin + (Percent * (Codec->OutAmpMax - Codec->OutAmpMin)) / 100;
    // В спецификации HDA: AMP = (Gain << 0) | (Gain << 8) | (Channel << 12) | (1 << 15)
    UINT32 Verb = HdaMakeVerb(Codec->Addr, Codec->OutAmpNode, HDA_VERB_SET_AMP_GAIN,
                              (Gain << 0) | (Gain << 8) | 0x8000); // Оба канала
    return HdaSendVerb(Verb, NULLPTR);
}