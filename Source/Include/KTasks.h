#pragma once

#include <Kernel/Types.h>
#include <Pci.h>
#include <Kernel/Scheduler.h>
#include <Asm/Cpu.h>
#include <Drive/Drive.h>
#include <Hda.h>
#include <Smbios.h>
#include <Console.h>
#include <Time/Timer.h>
#include <Lib/String.h>
#include <Kernel/Return.h>
#include <Fs/Vfs.h>
#include <Fs/Exfat.h>
#include <Fs/Fat32.h>
#include <Ps2Keyboard.h>
#include <FBDevice.h>
#include <Usb/Usb.h>
#include <Usb/Xhci.h>
#include <Usb/Ehci.h>
#include <Usb/Ohci.h>
#include <Gpu/Nvidia.h>
#include <Serial.h>
#include <Drive/Virtio.h>
#include <Network/Network.h>
#include <Network/Dhcp.h>
#include <Network/IpV4.h>
#include <Network/Http.h>

EXTERN(NOPTR, Shell64Entry(NOPTR *Arg));

static NOPTR DhcpGlobalCallback(DhcpClient *Client, BOOL Success) {
    if (Success) {
        CHAR IpStr[16];
        IpV4NTop((IpV4Addr){ .Addr = Client->YIAddr }, IpStr, 16);
    }
}

static inline NOPTR MainWorkerEntry(NOPTR *Arg) {
    (NOPTR)Arg;
    for (;;) {
        Halt();
    }
}

static volatile BOOL GPciInitDone = FALSE;

static inline NOPTR PciInitTask(NOPTR *Arg) {
    (NOPTR)Arg;
    PciInit();
    __sync_synchronize();
    GPciInitDone = TRUE;
    return;
}

EXTERN(INT, Init64Ready);

static inline NOPTR InitTask(NOPTR *Arg) {
    (NOPTR)Arg;
    while (!GPciInitDone) {
        SchedulerYield();
    }
    DriveManagerInit();
    SerialInit();
    DriveInitializePata();
    DriveInitializeSata();
    DriveInitializeNvme();
    DriveInitializeVirtio();
    UsbInit();
    OhciProbeAll();
    XhciProbeAll();
    EhciProbeAll();
    NvidiaInitAll();
    VfsInit();
    Fat32Init();
    ExfatInit();

    NetworkInit();
    HttpInit();
    NetworkLateInit();
    DhcpStartOnAllInterfaces(DhcpGlobalCallback);

    HdaInit();
    Init64Ready = 1;

    for (;;) {
        Halt();
    }
}

EXTERN(NOPTR, FlushOutputBuffer());

static NOPTR HCBlinkTask(NOPTR *Arg) {
    (NOPTR)Arg;
    while (1) {
        TimerSleep(16);
        Ps2KeyboardService();
        ConsoleService();
	FlushOutputBuffer();
        if (ConsoleIsPromptActive()) {
            RenderCursor();
        }
        SchedulerYield();
    }
}

NOPTR KTasksCreateAll(NOPTR);
