#include <EdidParse.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Kernel/Return.h>

/*
 * ============================================================================
 * Checksum
 * ============================================================================
 */
static UINT8 EdidChecksum(UINT8 *Data, UINT32 Len) {
    UINT8 Sum = 0;
    for (UINT32 I = 0; I < Len; I++) {
        Sum += Data[I];
    }
    return Sum;
}

static BOOL EdidVerifyChecksum(UINT8 *Block) {
    return EdidChecksum(Block, EDID_BLOCK_SIZE) == 0;
}

/*
 * ============================================================================
 * Manufacturer parsing
 * ============================================================================
 */
static NOPTR EdidParseManufacturer(UINT16 MfgId, CHAR *Buf) {
    Buf[0] = ((MfgId >> 10) & 0x1F) + 'A' - 1;
    Buf[1] = ((MfgId >> 5) & 0x1F) + 'A' - 1;
    Buf[2] = (MfgId & 0x1F) + 'A' - 1;
    Buf[3] = '\0';
}

/*
 * ============================================================================
 * Detailed Timing Descriptor parsing
 * ============================================================================
 */
static BOOL IsDtd(UINT8 *Desc) {
    return Desc[0] != 0x00 || Desc[1] != 0x00;
}

static BOOL IsMonitorName(UINT8 *Desc) {
    return Desc[0] == 0x00 && Desc[1] == 0x00 && Desc[2] == 0xFC;
}

static BOOL IsSerialNumber(UINT8 *Desc) {
    return Desc[0] == 0x00 && Desc[1] == 0x00 && Desc[2] == 0xFF;
}

static NOPTR EdidParseDtd(UINT8 *Desc, EdidTiming *Timing) {
    Timing->PixelClockKhz = (Desc[0] | (Desc[1] << 8)) * 10;
    
    Timing->HActive = Desc[2] | ((Desc[4] & 0xF0) << 4);
    Timing->HBlank = Desc[3] | ((Desc[4] & 0x0F) << 8);
    
    Timing->VActive = Desc[5] | ((Desc[7] & 0xF0) << 4);
    Timing->VBlank = Desc[6] | ((Desc[7] & 0x0F) << 8);
    
    Timing->HFrontPorch = Desc[8] | ((Desc[11] & 0xC0) << 2);
    Timing->HSyncWidth = Desc[9] | ((Desc[11] & 0x30) << 4);
    
    Timing->VFrontPorch = (Desc[10] >> 4) & 0x0F;
    Timing->VSyncWidth = Desc[10] & 0x0F;
    Timing->VFrontPorch |= ((Desc[11] & 0x0C) << 2);
    Timing->VSyncWidth |= ((Desc[11] & 0x03) << 4);
    
    Timing->HSyncPositive = (Desc[17] & 0x02) != 0;
    Timing->VSyncPositive = (Desc[17] & 0x04) != 0;
    Timing->Interlace = (Desc[17] & 0x20) != 0;
    
    SnPrintf(Timing->Name, sizeof(Timing->Name), "%dx%d",
             Timing->HActive, Timing->VActive);
}

static NOPTR EdidParseDescriptor(UINT8 *Desc, Edid *Edid) {
    if (IsDtd(Desc)) {
        if (Edid->DetailedCount < 4) {
            EdidParseDtd(Desc, &Edid->DetailedTimings[Edid->DetailedCount]);
            Edid->DetailedCount++;
        }
    } else if (IsMonitorName(Desc)) {
        // Copy monitor name (13 bytes, null-terminated)
        for (INT I = 0; I < 13 && Desc[5 + I] != 0x0A; I++) {
            if (Desc[5 + I] >= 0x20 && Desc[5 + I] < 0x7F) {
                Edid->Monitor.ProductName[I] = Desc[5 + I];
            }
        }
    } else if (IsSerialNumber(Desc)) {
        for (INT I = 0; I < 13 && Desc[5 + I] != 0x0A; I++) {
            if (Desc[5 + I] >= 0x20 && Desc[5 + I] < 0x7F) {
                Edid->Monitor.Serial[I] = Desc[5 + I];
            }
        }
    }
}

/*
 * ============================================================================
 * Established timings
 * ============================================================================
 */
static const struct {
    UINT16 Width, Height, Refresh;
    UINT16 BitMask;
} EstablishedModes[] = {
    {720, 400, 70, 0x0001},
    {720, 400, 88, 0x0002},
    {640, 480, 60, 0x0004},
    {640, 480, 67, 0x0008},
    {640, 480, 72, 0x0010},
    {640, 480, 75, 0x0020},
    {800, 600, 56, 0x0040},
    {800, 600, 60, 0x0080},
    {800, 600, 72, 0x0100},
    {800, 600, 75, 0x0200},
    {832, 624, 75, 0x0400},
    {1024, 768, 60, 0x0800},
    {1024, 768, 70, 0x1000},
    {1024, 768, 75, 0x2000},
    {1280, 1024, 75, 0x4000},
    {1152, 864, 75, 0x8000},
    {1280, 960, 60, 0x0001},
    {0, 0, 0, 0}
};

static NOPTR EdidParseEstablished(Edid *Edid) {
    UINT8 Timings[3];
    Timings[0] = Edid->Raw[0x23];
    Timings[1] = Edid->Raw[0x24];
    Timings[2] = Edid->Raw[0x25];
    
    for (INT I = 0; EstablishedModes[I].Width; I++) {
        UINT16 Mask = EstablishedModes[I].BitMask;
        UINT8 Byte = Timings[Mask >> 8];
        if (Byte & (1 << (Mask & 7))) {
            EdidTiming *T = &Edid->EstablishedTimings[Edid->RstablishedCount];
            T->HActive = EstablishedModes[I].Width;
            T->VActive = EstablishedModes[I].Height;
            T->PixelClockKhz = T->HActive * T->VActive * EstablishedModes[I].Refresh / 1000;
            SnPrintf(T->Name, sizeof(T->Name), "%dx%d@%d", T->HActive, T->VActive, EstablishedModes[I].Refresh);
            Edid->RstablishedCount++;
        }
    }
}

/*
 * ============================================================================
 * Standard timings
 * ============================================================================
 */
static NOPTR EdidParseStandard(Edid *Edid) {
    UINT8 *Std = &Edid->Raw[0x26];
    
    for (INT I = 0; I < 8; I++) {
        UINT8 H = Std[I * 2];
        UINT8 V = Std[I * 2 + 1];
        if (H == 0x01 && V == 0x01) continue;  // Skip
        if (H == 0x00 && V == 0x00) continue;  // Skip
        
        UINT16 Width = (H + 31) * 8;
        UINT8 Aspect = (V >> 6) & 0x03;
        UINT16 Height;
        
        switch (Aspect) {
            case 0: Height = Width * 10 / 16; break;  // 16:10
            case 1: Height = Width * 4 / 5; break;    // 4:3
            case 2: Height = Width * 3 / 4; break;    // 5:4
            case 3: Height = Width * 9 / 16; break;   // 16:9
            default: Height = Width * 3 / 4;
        }
        
        UINT8 Refresh = (V & 0x3F) + 60;
        
        EdidTiming *T = &Edid->StandardTimings[Edid->StandardCount];
        T->HActive = Width;
        T->VActive = Height;
        T->PixelClockKhz = Width * Height * Refresh / 1000;
        SnPrintf(T->Name, sizeof(T->Name), "%dx%d@%d", Width, Height, Refresh);
        Edid->StandardCount++;
    }
}

/*
 * ============================================================================
 * CEA-861 extension parsing
 * ============================================================================
 */
static const struct {
    UINT8 Vic;
    UINT16 Width;
    UINT16 Height;
    UINT16 Refresh;
    UINT32 ClockKhz;  // ← u32 вместо u16
    BOOL Interlaced;
} VicTable[] = {
    {1, 640, 480, 60, 25200, FALSE},
    {2, 720, 480, 60, 27000, FALSE},
    {3, 720, 480, 60, 27000, TRUE},
    {4, 1280, 720, 60, 74250, FALSE},
    {5, 1920, 1080, 60, 148500, FALSE},
    {6, 720, 480, 60, 27000, TRUE},
    {7, 720, 480, 60, 27000, FALSE},
    {16, 1920, 1080, 60, 148500, FALSE},
    {18, 720, 576, 50, 27000, FALSE},
    {19, 1280, 720, 50, 74250, FALSE},
    {20, 1920, 1080, 50, 148500, FALSE},
    {31, 1920, 1080, 50, 148500, TRUE},
    {32, 1920, 1080, 24, 74250, FALSE},
    {33, 1920, 1080, 25, 74250, FALSE},
    {34, 1920, 1080, 30, 74250, FALSE},
    {60, 1280, 720, 24, 59400, FALSE},
    {61, 1280, 720, 25, 74250, FALSE},
    {62, 1280, 720, 30, 74250, FALSE},
    {63, 1920, 1080, 120, 297000, FALSE},
    {64, 1920, 1080, 100, 297000, FALSE},
    {65, 3840, 2160, 60, 594000, FALSE},
    {66, 3840, 2160, 50, 594000, FALSE},
    {67, 3840, 2160, 30, 297000, FALSE},
    {68, 3840, 2160, 25, 297000, FALSE},
    {69, 3840, 2160, 24, 297000, FALSE},
    {70, 4096, 2160, 60, 594000, FALSE},
    {71, 4096, 2160, 50, 594000, FALSE},
    {72, 4096, 2160, 30, 297000, FALSE},
    {73, 4096, 2160, 25, 297000, FALSE},
    {74, 4096, 2160, 24, 297000, FALSE},
    {0, 0, 0, 0, 0, false}
};

static NOPTR EdidParseCeaModes(Edid *Edid, UINT8 *Data, UINT32 Len) {
    UINT8 DtdStart = Data[2];
    UINT8 DtdCount = Data[3];
    
    // Parse short video descriptors
    UINT8 *Ptr = Data + 4;
    while (Ptr < Data + Len - DtdStart * 18) {
        UINT8 Tag = (*Ptr >> 5) & 0x07;
        UINT8 BlockLen = *Ptr & 0x1F;
        Ptr++;
        
        if (Tag == 3) {  // Short Video Descriptor
            for (INT I = 0; I < BlockLen; I++) {
                UINT8 Vic = Ptr[I] & 0x7F;
                for (INT J = 0; VicTable[J].Vic; J++) {
                    if (VicTable[J].Vic == Vic) {
                        EdidVic *V = &Edid->CeaModes[Edid->CeaCount];
                        V->Vic = Vic;
                        V->Width = VicTable[J].Width;
                        V->Height = VicTable[J].Height;
                        V->Refresh = VicTable[J].Refresh;
                        V->Interlaced = VicTable[J].Interlaced;
                        V->PixelClockKhz = VicTable[J].ClockKhz;
                        Edid->CeaCount++;
                        break;
                    }
                }
            }
        } else if (Tag == 7) {  // HDMI VSDB
            UINT8 Ieee[3] = {Ptr[0], Ptr[1], Ptr[2]};
            if (Ieee[0] == 0x03 && Ieee[1] == 0x0C && Ieee[2] == 0x00) {  // HDMI
                if (BlockLen >= 5) {
                    Edid->MaxTmdsClockKhz = (Ptr[5] & 0x3F) * 5000;
                    Edid->HdrSupported = (BlockLen >= 8 && (Ptr[7] & 0x04));
                }
            }
        } else if (Tag == 4) {  // VRR
            Edid->VrrSupported = TRUE;
        }
        Ptr += BlockLen;
    }
}

static NOPTR EdidParseCeaExtension(Edid *Edid) {
    UINT8 *Ext = Edid->Raw + EDID_BLOCK_SIZE;
    UINT8 Revision = Ext[1];
    UINT8 DtdStart = Ext[2];
    
    if (Revision >= 3) {
        EdidParseCeaModes(Edid, Ext, EDID_BLOCK_SIZE);
    }
}

/*
 * ============================================================================
 * Main parse function
 * ============================================================================
 */
INT EdidParse(UINT8 *Raw, UINT32 Size, Edid *Edid) {
    if (!Raw || !Edid || Size < EDID_BLOCK_SIZE) RETURN(NO_OBJECT);
    
    MemSet(Edid, 0, sizeof(Edid));
    MemCpy(Edid->Raw, Raw, (Size > EDID_BLOCK_SIZE * EDID_MAX_BLOCKS) ? 
           EDID_BLOCK_SIZE * EDID_MAX_BLOCKS : Size);
    
    // Check header
    if (Raw[0] != 0x00 || Raw[1] != 0xFF || Raw[2] != 0xFF || Raw[3] != 0xFF ||
        Raw[4] != 0xFF || Raw[5] != 0xFF || Raw[6] != 0xFF || Raw[7] != 0x00) {
        RETURN(INCORRECT_VALUE);
    }
    
    Edid->Blocks = 1 + Raw[0x7E];
    if (Edid->Blocks > 4) Edid->Blocks = 4;
    
    // Verify checksums
    for (UINT32 I = 0; I < Edid->Blocks; I++) {
        if (!EdidVerifyChecksum(Edid->Raw + I * EDID_BLOCK_SIZE)) {
            RETURN(GENERAL_ERROR);
        }
    }
    
    EdidBase *Base = (EdidBase*)Raw;
    
    // Basic info
    EdidParseManufacturer(Base->ManufacturerId, Edid->Monitor.Manufacturer);
    Edid->Monitor.ProductId = Base->ProductId;
    Edid->Monitor.SerialNumber = Base->SerialNumber;
    Edid->Monitor.Week = Base->Week;
    Edid->Monitor.Year = 1990 + Base->Year;
    Edid->Monitor.Digital = (Base->VideoInput & 0x80) != 0;
    Edid->Monitor.Gamma = Base->Gamma ? Base->Gamma + 100 : 0;  // * 0.01
    Edid->Monitor.MaxHSizeMm = Base->MaxHSizeCm * 10;
    Edid->Monitor.MaxVSizeMm = Base->MaxVSizeCm * 10;
    
    // Parse detailed timings and descriptors
    for (INT I = 0; I < 4; I++) {
        EdidParseDescriptor((UINT8*)&Base->DetailedTimings[I], Edid);
    }
    
    // Parse established timings
    EdidParseEstablished(Edid);
    
    // Parse standard timings
    EdidParseStandard(Edid);
    
    // Parse CEA extension if present
    if (Edid->Blocks > 1 && (Raw[EDID_BLOCK_SIZE] == 0x02)) {  // CEA-861
        Edid->CeaExtPresent = TRUE;
        EdidParseCeaExtension(Edid);
    }
    
    // Set preferred timing
    if (Edid->DetailedCount > 0) {
        Edid->PreferredTiming = Edid->DetailedTimings[0];
    }
    
    RETURN(SUCCESS);
}