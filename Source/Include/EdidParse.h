#pragma once

#include <Kernel/Types.h>

#define EDID_BLOCK_SIZE         128
#define EDID_MAX_BLOCKS         4
#define EDID_MAX_MODES          64
#define EDID_MAX_NAME_LEN       14
#define EDID_MAX_SERIAL_LEN     14

// EDID structure (full)
typedef struct {
    UINT8 Header[8];
    UINT16 ManufacturerId;
    UINT16 ProductId;
    UINT32 SerialNumber;
    UINT8 Week;
    UINT8 Year;
    UINT8 Version;
    UINT8 Revision;
    UINT8 VideoInput;
    UINT8 MaxHSizeCm;
    UINT8 MaxVSizeCm;
    UINT8 Gamma;
    UINT8 Features;
    UINT8 ColorCoefficients[10];
    UINT8 EstablishedTimings[3];
    UINT8 StandardTimings[16];
    struct {
        UINT16 PixelClock;
        UINT8 Data[18];
    } DetailedTimings[4];
    UINT8 Extensions;
    UINT8 Checksum;
} ATTRIBUTE(packed) EdidBase;

// Detailed timing descriptor
typedef struct {
    UINT16 PixelClockKhz;
    UINT16 HActive;
    UINT16 HBlank;
    UINT16 HFrontPorch;
    UINT16 HSyncWidth;
    UINT16 VActive;
    UINT16 VBlank;
    UINT16 VFrontPorch;
    UINT16 VSyncWidth;
    BOOL HSyncPositive;
    BOOL VSyncPositive;
    BOOL Interlace;
    CHAR Name[EDID_MAX_NAME_LEN];
} EdidTiming;

// CEA-861 short video descriptor
typedef struct {
    UINT8 Vic;
    UINT16 Width;
    UINT16 Height;
    UINT16 Refresh;
    BOOL Interlaced;
    UINT16 PixelClockKhz;
    UINT8 AspectRatio;
} EdidVic;

// Monitor info
typedef struct {
    CHAR Manufacturer[4];
    CHAR ProductName[EDID_MAX_NAME_LEN];
    CHAR Serial[EDID_MAX_SERIAL_LEN];
    UINT16 ProductId;
    UINT32 SerialNumber;
    UINT8 Week;
    UINT8 Year;
    UINT8 Version;
    UINT8 Revision;
    UINT32 MaxHSizeMm;
    UINT32 MaxVSizeMm;
    BOOL Digital;
    UINT8 Gamma;
} EdidMonitor;

// Main EDID structure
typedef struct {
    UINT8 Raw[EDID_BLOCK_SIZE * EDID_MAX_BLOCKS];
    UINT32 Blocks;
    UINT32 ChecksumValid;
    
    EdidMonitor Monitor;
    EdidTiming PreferredTiming;
    EdidTiming DetailedTimings[4];
    UINT32 DetailedCount;
    
    EdidTiming EstablishedTimings[16];
    UINT32 RstablishedCount;
    
    EdidTiming StandardTimings[8];
    UINT32 StandardCount;
    
    EdidVic CeaModes[32];
    UINT32 CeaCount;
    
    // Flags
    BOOL CeaExtPresent;
    BOOL VtbExtPresent;
    BOOL DiExtPresent;
    BOOL LsExtPresent;
    BOOL MiExtPresent;
    
    // CEA data blocks
    BOOL Ycbcr420Supported;
    BOOL Ycbcr422Supported;
    BOOL Ycbcr444Supported;
    UINT32 MaxTmdsClockKhz;
    UINT32 AudioChannels;
    BOOL HdrSupported;
    BOOL VrrSupported;
    BOOL AllmSupported;
} Edid;

// ============================================================================
// API
// ============================================================================

// Parse raw EDID data
INT EdidParse(UINT8 *Raw, UINT32 Size, Edid *Edid);