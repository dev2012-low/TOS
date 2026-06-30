#pragma once

#include <Kernel/Types.h>
#include <Pci.h>

#define HDA_MAX_CODECS        4
#define HDA_MAX_WIDGETS       64

typedef struct {
    UINT8 Addr;
    UINT32 VendorId;
    UINT32 SubsystemId;
    UINT8 RootNode;
    UINT8 AfgNode;
    UINT8 OutPinNode;
    UINT8 OutConverterNode;
    BOOL Present;
    UINT8 OutAmpNode;          // какой нод отвечает за усиление (обычно OutConverterNode или OutPinNode)
    UINT8 OutAmpChannel;       // левый/правый/оба (0 = оба)
    UINT32 OutAmpCapabilities; // что умеет (с шагом, с шагом в 0.25 dB и т.д.)
    INT OutAmpLeftGain;        // текущий gain левого канала
    INT OutAmpRightGain;       // текущий gain правого канала
    INT OutAmpMin, OutAmpMax;  // минимальный и максимальный gain (в 0.25 dB)
    INT OutAmpStep;
    UINT32 SupportedPcmRates;   // битовая маска: 1 << (kHz-48)
    UINT32 SupportedPcmSizes;   // 1<<0 = 8bit, 1<<1 = 16bit, 1<<2 = 20bit, 1<<3 = 24bit, 1<<4 = 32bit
    UINT32 SupportedPcmChannels; // маска поддерживаемых каналов
} HdaCodecInfo;

typedef struct {
    volatile UINT8 *Mmio;
    PciDevice *PciDev;
    UINT8 OutputStream;
    UINT8 CodecCount;
    HdaCodecInfo Codecs[HDA_MAX_CODECS];
    UINT32 IrqGsi;
    BOOL MsiEnabled;
    BOOL Initialized;
    BOOL PlaybackReady;
} HdaController;

INT HdaInit(NOPTR);
BOOL HdaIsReady(NOPTR);
HdaController *HdaGetController(NOPTR);
NOPTR HdaPrintInfo(NOPTR);
INT HdaPlayTestTone(NOPTR);
NOPTR HdaIrqHandler(NOPTR);
INT HdaSetVolume(HdaController *Ctl, INT CodecIdx, INT Percent);
