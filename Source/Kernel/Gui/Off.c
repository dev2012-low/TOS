#include <Gui/Off.h>
#include <Console.h>
#include <FBDevice.h>
#include <Kernel/Scheduler.h>
#include <Rgb.h>
#include <Lib/String.h>
#include <Time/Timer.h>

static NOPTR GuiShutdownTask(NOPTR *Arg) {
    (NOPTR)Arg;
    INT32 CenterX = (FBDeviceGetWidth() / 2);
    INT32 CenterY = (FBDeviceGetHeight() / 2);
    const CHAR* Text1 = "Shutting Down";
    const CHAR* Text2 = "Shutting Down.";
    const CHAR* Text3 = "Shutting Down..";
    const CHAR* Text4 = "Shutting Down...";
    USIZE Text1Len = StrLen(Text1);
    USIZE Text2Len = Text1Len++;
    USIZE Text3Len = Text2Len++;
    USIZE Text4Len = Text3Len++;
    INT32 Text1X = (CenterX - ((INT32)Text1Len / 2));
    INT32 Text2X = (CenterX - ((INT32)Text2Len / 2));
    INT32 Text3X = (CenterX - ((INT32)Text3Len / 2));
    INT32 Text4X = (CenterX - ((INT32)Text4Len / 2));
    TimerSdelay(2);
    for (;;) {
	FBDeviceClear(RGB_BLACK);

	FBDeviceDrawString(Text1, Text1X, CenterY, RGB_BLUE, RGB_BLACK);
        FBDeviceClear(RGB_BLACK);
        TimerMdelay(8);
	
	FBDeviceDrawString(Text2, Text2X, CenterY, RGB_BLUE, RGB_BLACK);
        FBDeviceClear(RGB_BLACK);
        TimerMdelay(8);

	FBDeviceDrawString(Text3, Text3X, CenterY, RGB_BLUE, RGB_BLACK);
        FBDeviceClear(RGB_BLACK);
        TimerMdelay(8);

	FBDeviceDrawString(Text4, Text4X,CenterY, RGB_BLUE, RGB_BLACK);
        FBDeviceClear(RGB_BLACK);
        TimerMdelay(8);
    }
}

NOPTR GuiShutdown(NOPTR) {
    ConsoleSwitch(FALSE);
    FBDeviceClear(RGB_BLACK);
    SchedulerCreateTask("SHUTDOWN", GuiShutdownTask, NULLPTR,
                          SCHED_PRIORITY_REALTIME, TASK_DEFAULT_QUANTUM);
}

static NOPTR GuiRebootTask(NOPTR *Arg) {
    (NOPTR)Arg;
    INT32 CenterX = (FBDeviceGetWidth() / 2);
    INT32 CenterY = (FBDeviceGetHeight() / 2);
    const CHAR* Text1 = "Rebooting";
    const CHAR* Text2 = "Rebooting.";
    const CHAR* Text3 = "Rebooting..";
    const CHAR* Text4 = "Rebooting...";
    USIZE Text1Len = StrLen(Text1);
    USIZE Text2Len = Text1Len++;
    USIZE Text3Len = Text2Len++;
    USIZE Text4Len = Text3Len++;
    INT32 Text1X = (CenterX - ((INT32)Text1Len / 2));
    INT32 Text2X = (CenterX - ((INT32)Text2Len / 2));
    INT32 Text3X = (CenterX - ((INT32)Text3Len / 2));
    INT32 Text4X = (CenterX - ((INT32)Text4Len / 2));
    TimerSdelay(2);
    for (;;) {
	FBDeviceClear(RGB_BLACK);

	FBDeviceDrawString(Text1, Text1X, CenterY, RGB_BLUE, RGB_BLACK);
        FBDeviceClear(RGB_BLACK);
        TimerMdelay(8);
	
	FBDeviceDrawString(Text2, Text2X, CenterY, RGB_BLUE, RGB_BLACK);
        FBDeviceClear(RGB_BLACK);
        TimerMdelay(8);

	FBDeviceDrawString(Text3, Text3X, CenterY, RGB_BLUE, RGB_BLACK);
        FBDeviceClear(RGB_BLACK);
        TimerMdelay(8);

	FBDeviceDrawString(Text4, Text4X,CenterY, RGB_BLUE, RGB_BLACK);
        FBDeviceClear(RGB_BLACK);
        TimerMdelay(8);
    }
}

NOPTR GuiReboot(NOPTR) {
    ConsoleSwitch(FALSE);
    FBDeviceClear(RGB_BLACK);
    SchedulerCreateTask("REBOOT", GuiRebootTask, NULLPTR,
                          SCHED_PRIORITY_REALTIME, TASK_DEFAULT_QUANTUM);
}