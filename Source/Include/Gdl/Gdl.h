#pragma once

#include <Kernel/Types.h>
#include <Kernel/List.h>
#include <Kernel/Paging.h>
#include <Pci.h>
#include <FBDevice.h>
#include <Gdl/GdlDef.h>

/*
 * ============================================================================
 * Forward declarations
 * ============================================================================
 */
struct GdlDevice;
struct GdlCrtc;
struct GdlEncoder;
struct GdlConnector;
struct GdlFramebuffer;
struct GdlGemObject;
struct GdlMode;

/*
 * ============================================================================
 * Display Mode
 * ============================================================================
 */
typedef struct GdlMode {
    CHAR Name[32];
    UINT32 Width;
    UINT32 Height;
    UINT32 Refresh;
    UINT32 ClockKhz;
    
    UINT32 HDisplay;
    UINT32 HSyncStart;
    UINT32 HSyncEnd;
    UINT32 HTotal;
    UINT32 HSkew;
    
    UINT32 VDisplay;
    UINT32 VSyncStart;
    UINT32 VSyncEnd;
    UINT32 VTotal;
    UINT32 VScan;
    
    UINT32 Flags;
    UINT32 Type;
    
    struct ListHead Node;
} GdlMode;

typedef struct GdlPageFlip {
    struct GdlFramebuffer *Fb;
    NOPTR (*Callback)(void *Data);
    NOPTR *Data;
    int Pending;   // 1 = ожидает VBlank, 0 = выполнен
} GdlPageFlip;

/*
 * ============================================================================
 * CRTC (Cathode Ray Tube Controller)
 * ============================================================================
 */
typedef struct GdlCrtc {
    UINT32 Index;
    CHAR Name[32];
    struct GdlDevice *Dev;
    
    struct GdlFramebuffer *Fb;
    struct GdlMode *Mode;
    BOOL Enabled;
    
    UINT32 CursorX;
    UINT32 CursorY;
    UINT32 CursorHandle;
    BOOL CursorVisible;
    
    INT (*SetMode)(struct GdlCrtc *Crtc, struct GdlMode *Mode);
    INT (*SetFb)(struct GdlCrtc *Crtc, struct GdlFramebuffer *Fb);
    NOPTR (*Enable)(struct GdlCrtc *Crtc);
    NOPTR (*Disable)(struct GdlCrtc *Crtc);
    INT (*PageFlip)(struct GdlCrtc *Crtc, struct GdlFramebuffer *Fb);
    INT (*SetCursor)(struct GdlCrtc *Crtc, UINT32 X, UINT32 Y, UINT32 Handle);
    
    NOPTR *Priv;
    struct ListHead Node;

    GdlPageFlip *PendingFlip;
    UINT32 VBlankCount;
    NOPTR (*HandleVBlank)(struct GdlCrtc *Crtc);
} GdlCrtc;

/*
 * ============================================================================
 * Encoder
 * ============================================================================
 */
typedef struct GdlEncoder {
    UINT32 Index;
    CHAR Name[32];
    struct GdlDevice *Dev;
    UINT32 Type;
    UINT32 PossibleCrtcs;
    
    INT (*ModeSet)(struct GdlEncoder *Enc, struct GdlMode *Mode);
    
    NOPTR *Priv;
    struct ListHead Node;
} GdlEncoder;

/*
 * ============================================================================
 * Connector
 * ============================================================================
 */
typedef struct GdlConnector {
    UINT32 Index;
    CHAR Name[32];
    struct GdlDevice *Dev;
    UINT32 Type;
    struct GdlEncoder *Encoder;
    
    struct ListHead Modes;
    UINT8 *Edid;
    UINT32 EdidSize;
    bool Connected;
    bool Polled;
    
    INT (*Detect)(struct GdlConnector *Conn);
    
    NOPTR *Priv;
    struct ListHead Node;
} GdlConnector;

/*
 * ============================================================================
 * Framebuffer
 * ============================================================================
 */
typedef struct GdlFramebuffer {
    UINT32 Handle;
    UINT32 Width;
    UINT32 Height;
    UINT32 Pitch;
    UINT32 Format;
    UINT32 BitsPerPixel;
    UINT64 Size;
    
    UINT8 *VAddr;
    UINT64 PAddr;
    
    struct GdlGemObject *Gem;
    BOOL IsDirty;
    
    struct ListHead Node;
} GdlFramebuffer;

/*
 * ============================================================================
 * GEM Object (GPU memory)
 * ============================================================================
 */
typedef struct GdlGemObject {
    UINT32 Handle;
    UINT64 Size;
    UINT32 Flags;
    UINT32 Refcount;
    
    UINT64 CpuAddr;
    UINT64 PhysAddr;
    UINT32 PageCount;
    NOPTR *Pages;
    
    UINT32 DmaHandle;
    BOOL Contiguous;

    UINT64 GpuAddr;
    BOOL Bound;
    
    struct ListHead Node;
} GdlGem;

/*
 * ============================================================================
 * GPU Device
 * ============================================================================
 */

typedef struct GdlGtt {
    UINT64 *Entries;     // GPU-адреса для каждой страницы
    UINT32 EntryCount;
    UINT32 GttBase;      // физический адрес GTT в GPU
    BOOL Initialized;
} GdlGtt;

typedef struct GdlDevice {
    CHAR Name[32];
    UINT32 VendorId;
    UINT32 DeviceId;
    PciDevice *PciDev;
    UINT32 PciSlot;
    UINT32 PciBus;
    UINT32 Irq;
    
    volatile NOPTR *MmioBase;
    UINT32 MmioSize;
    volatile NOPTR *FbBase;
    UINT32 FbSize;
    
    NOPTR *Priv;
    
    struct ListHead Crtcs;
    struct ListHead Encoders;
    struct ListHead Connectors;
    struct ListHead Framebuffers;
    struct ListHead GemObjects;
    
    UINT32 NextGemHandle;
    UINT32 NextFbHandle;
    
    INT (*Init)(struct GdlDevice *Dev);
    NOPTR (*Fini)(struct GdlDevice *Dev);
    INT (*CreateGem)(struct GdlDevice *Dev, UINT64 Size, UINT32 *Handle, UINT32 Flags);
    NOPTR (*DestroyGem)(struct GdlDevice *Dev, UINT32 Handle);
    INT (*MapGem)(struct GdlDevice *Dev, UINT32 Handle, UINT64 *CpuAddr, UINT64 *PhysAddr);
    NOPTR (*UnmapGem)(struct GdlDevice *Dev, UINT32 Handle);
    INT (*Flush)(struct GdlDevice *Dev, struct GdlFramebuffer *Fb);
    GdlGtt Gtt;
    INT (*BindGem)(struct GdlDevice *Dev, GdlGem *Gem);
    INT (*UnbindGem)(struct GdlDevice *Dev, GdlGem *Gem);
    
    BOOL Initialized;
    struct GdlCrtc *PrimaryCrtc;
    
    struct ListHead Node;
} GdlDevice;

/*
 * ============================================================================
 * Core GDS API
 * ============================================================================
 */

// System init
NOPTR GdlInit(NOPTR);

// Device management
INT GdlDeviceRegister(GdlDevice *Dev);
INT GdlDeviceUnregister(GdlDevice *Dev);
GdlDevice *GdlDeviceFind(UINT32 VendorId, UINT32 DeviceId);
GdlDevice *GdlDeviceFindByPci(PciDevice *PciDev);
GdlDevice *GdlGetPrimaryDevice(NOPTR);

// CRTC ops
GdlCrtc *GdlCrtcCreate(GdlDevice *Dev, UINT32 Index);
INT GdlCrtcDestroy(GdlCrtc *Crtc);
INT GdlCrtcSetMode(GdlCrtc *Crtc, GdlMode *Mode);
INT GdlCrtcSetFramebuffer(GdlCrtc *Crtc, GdlFramebuffer *Fb);
INT GdlCrtcPageFlip(GdlCrtc *Crtc, GdlFramebuffer *Fb);
INT GdlCrtcSetCursor(GdlCrtc *Crtc, UINT32 X, UINT32 Y, UINT32 Handle, BOOL Show);
INT GdlCrtcEnable(GdlCrtc *Crtc);
INT GdlCrtcDisable(GdlCrtc *Crtc);
NOPTR GdlCrtcHandleVBlank(GdlCrtc *Crtc);

// Connector ops
GdlConnector *GdlConnectorCreate(GdlDevice *Dev, UINT32 Type, UINT32 Index);
INT GdlConnectorDestroy(GdlConnector *Conn);
INT GdlConnectorDetect(GdlConnector *Conn);
INT GdlConnectorAddMode(GdlConnector *Conn, GdlMode *Mode);
NOPTR GdlConnectorClearModes(GdlConnector *Conn);
INT GdsConnectorSetEdid(GdlConnector *Conn, UINT8 *Edid, UINT32 Size);
NOPTR GdsConnectorUpdateEdid(GdlConnector *Conn, UINT8 *Raw, UINT32 Size);

// Encoder ops
GdlEncoder *GdsEncoderCreate(GdlDevice *Dev, UINT32 Type, UINT32 Index);
INT GdlEncoderDestroy(GdlEncoder *Enc);
INT GdlEncoderSetMode(GdlEncoder *Enc, GdlMode *Mode);

// Framebuffer ops
GdlFramebuffer *GdlFramebufferCreate(UINT32 Width, UINT32 Height, UINT32 Format);
INT GdlFramebufferDestroy(GdlFramebuffer *Fb);
INT GdlFramebufferMap(GdlFramebuffer *Fb);
NOPTR GdlFramebufferUnmap(GdlFramebuffer *Fb);
INT GdlFramebufferFlush(GdlFramebuffer *Fb);

// GEM ops
INT GdlGemCreate(UINT64 Size, UINT32 *Handle);
INT GdlGemCreateContiguous(UINT64 Size, UINT32 *Handle);
INT GdlGemDestroy(UINT32 Handle);
INT GdlGemMap(UINT32 Handle, UINT64 *CpuAddr, UINT64 *PhysAddr);
NOPTR GdlGemUnmap(UINT32 Handle);
INT GdlGemGetSize(UINT32 Handle, UINT64 *Size);
NOPTR GdlGemInit(NOPTR);
INT GdlGemBind(GdlGem *Gem, GdlDevice *Dev);
INT GdlGemUnbind(GdlGem *Gem, GdlDevice *Dev);
UINT64 GdlGemGetGpuAddr(GdlGem *Gem, GdlDevice *Dev);

// Mode ops
GdlMode *GdlModeCreate(UINT32 Width, UINT32 Height, UINT32 Refresh);
GdlMode *GdlModeCreateTiming(UINT32 HDisplay, UINT32 HSyncStart, UINT32 HSyncEnd, UINT32 HTotal,
                                    UINT32 VDisplay, UINT32 VSyncStart, UINT32 VSyncEnd, UINT32 VTotal,
                                    UINT32 ClockKhz);
NOPTR GdlModeDestroy(GdlMode *Mode);
INT GdlModeValid(GdlCrtc *Crtc, GdlMode *Mode);
GdlMode *GdlModeFind(GdlConnector *Conn, UINT32 Width, UINT32 Height);