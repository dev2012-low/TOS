#pragma once

#include <Kernel/Types.h>

// Scan code set 1
#define PS2_KEY_ERR_OVERRUN     0x00
#define PS2_KEY_ESC             0x01
#define PS2_KEY_1               0x02
#define PS2_KEY_2               0x03
#define PS2_KEY_3               0x04
#define PS2_KEY_4               0x05
#define PS2_KEY_5               0x06
#define PS2_KEY_6               0x07
#define PS2_KEY_7               0x08
#define PS2_KEY_8               0x09
#define PS2_KEY_9               0x0A
#define PS2_KEY_0               0x0B
#define PS2_KEY_MINUS           0x0C
#define PS2_KEY_EQUAL           0x0D
#define PS2_KEY_BACKSPACE       0x0E
#define PS2_KEY_TAB             0x0F
#define PS2_KEY_Q               0x10
#define PS2_KEY_W               0x11
#define PS2_KEY_E               0x12
#define PS2_KEY_R               0x13
#define PS2_KEY_T               0x14
#define PS2_KEY_Y               0x15
#define PS2_KEY_U               0x16
#define PS2_KEY_I               0x17
#define PS2_KEY_O               0x18
#define PS2_KEY_P               0x19
#define PS2_KEY_LBRACKET        0x1A
#define PS2_KEY_RBRACKET        0x1B
#define PS2_KEY_ENTER           0x1C
#define PS2_KEY_LCTRL           0x1D
#define PS2_KEY_A               0x1E
#define PS2_KEY_S               0x1F
#define PS2_KEY_D               0x20
#define PS2_KEY_F               0x21
#define PS2_KEY_G               0x22
#define PS2_KEY_H               0x23
#define PS2_KEY_J               0x24
#define PS2_KEY_K               0x25
#define PS2_KEY_L               0x26
#define PS2_KEY_SEMICOLON       0x27
#define PS2_KEY_APOSTROPHE      0x28
#define PS2_KEY_GRAVE           0x29
#define PS2_KEY_LSHIFT          0x2A
#define PS2_KEY_BACKSLASH       0x2B
#define PS2_KEY_Z               0x2C
#define PS2_KEY_X               0x2D
#define PS2_KEY_C               0x2E
#define PS2_KEY_V               0x2F
#define PS2_KEY_B               0x30
#define PS2_KEY_N               0x31
#define PS2_KEY_M               0x32
#define PS2_KEY_COMMA           0x33
#define PS2_KEY_PERIOD          0x34
#define PS2_KEY_SLASH           0x35
#define PS2_KEY_RSHIFT          0x36
#define PS2_KEY_KP_ASTERISK     0x37
#define PS2_KEY_LALT            0x38
#define PS2_KEY_SPACE           0x39
#define PS2_KEY_CAPSLOCK        0x3A
#define PS2_KEY_F1              0x3B
#define PS2_KEY_F2              0x3C
#define PS2_KEY_F3              0x3D
#define PS2_KEY_F4              0x3E
#define PS2_KEY_F5              0x3F
#define PS2_KEY_F6              0x40
#define PS2_KEY_F7              0x41
#define PS2_KEY_F8              0x42
#define PS2_KEY_F9              0x43
#define PS2_KEY_F10             0x44
#define PS2_KEY_NUMLOCK         0x45
#define PS2_KEY_SCROLLLOCK      0x46
#define PS2_KEY_KP_7            0x47
#define PS2_KEY_KP_8            0x48
#define PS2_KEY_KP_9            0x49
#define PS2_KEY_KP_MINUS        0x4A
#define PS2_KEY_KP_4            0x4B
#define PS2_KEY_KP_5            0x4C
#define PS2_KEY_KP_6            0x4D
#define PS2_KEY_KP_PLUS         0x4E
#define PS2_KEY_KP_1            0x4F
#define PS2_KEY_KP_2            0x50
#define PS2_KEY_KP_3            0x51
#define PS2_KEY_KP_0            0x52
#define PS2_KEY_KP_PERIOD       0x53
#define PS2_KEY_SYSREQ          0x54
#define PS2_KEY_F11             0x57
#define PS2_KEY_F12             0x58

// Scan-code set identifiers (response to F0 00)
#define PS2_SCANCODE_SET1_ID    0x43
#define PS2_SCANCODE_SET2_ID    0x41
#define PS2_SCANCODE_SET3_ID    0x3F

// Set 2/3 break prefix (Set 1 uses bit 7 instead)
#define PS2_KEY_BREAK_PREFIX    0xF0

// Extended scan codes (prefixed with 0xE0)
#define PS2_KEY_EXT_PREFIX      0xE0
#define PS2_KEY_KP_ENTER        0x1C
#define PS2_KEY_RCTRL           0x1D
#define PS2_KEY_KP_SLASH        0x35
#define PS2_KEY_RALT            0x38
#define PS2_KEY_HOME            0x47
#define PS2_KEY_UP              0x48
#define PS2_KEY_PGUP            0x49
#define PS2_KEY_LEFT            0x4B
#define PS2_KEY_RIGHT           0x4D
#define PS2_KEY_END             0x4F
#define PS2_KEY_DOWN            0x50
#define PS2_KEY_PGDN            0x51
#define PS2_KEY_INSERT          0x52
#define PS2_KEY_DELETE          0x53

// Pause/Break sequence
#define PS2_KEY_PAUSE_PREFIX    0xE1

// Key state
typedef enum {
    KEY_STATE_RELEASED = 0,
    KEY_STATE_PRESSED = 1
} KeyState;

// Modifier flags
#define MOD_SHIFT       0x01
#define MOD_CTRL        0x02
#define MOD_ALT         0x04
#define MOD_CAPSLOCK    0x08
#define MOD_NUMLOCK     0x10
#define MOD_SCROLLLOCK  0x20

#define PS2_MAX_SUBSCRIBERS 128

// Key event structure
typedef struct {
    UINT8 ScanCode;
    CHAR Ascii;
    UINT8 RawScanCode;
    BOOL Extended;
    KeyState State;
    UINT8 Modifiers;
} KeyEvent;

// Keyboard layout
typedef enum {
    KB_LAYOUT_US,
    KB_LAYOUT_UK,
    KB_LAYOUT_RU
} KeyboardLayout;

// API functions
INT Ps2KeyboardInit(NOPTR);
INT Ps2KeyboardSetLayout(KeyboardLayout Layout);
KeyboardLayout Ps2KeyboardGetLayout(NOPTR);
BOOL Ps2KeyboardPollEvent(KeyEvent *Event);
BOOL Ps2KeyboardIsPressed(UINT8 ScanCode);
UINT8 Ps2KeyboardGetModifiers(NOPTR);
UINT8 Ps2KeyboardGetScanCodeSet(NOPTR);
NOPTR Ps2KeyboardSetLeds(BOOL NumLock, BOOL CapsLock, BOOL ScrollLock);

// Callback support
typedef NOPTR (*KeyboardCallback)(KeyEvent *Event, NOPTR *UserData);

typedef struct {
    KeyboardCallback Callback;
    NOPTR            *UserData;
    BOOL              Active;
} Ps2Subscriber;

INT Ps2KeyboardSubscribe(KeyboardCallback Cb, NOPTR *Ud);
INT Ps2KeyboardUnsubscribe(KeyboardCallback Cb);
INT Ps2KeyboardDispatchEvents(NOPTR);
NOPTR Ps2KeyboardService(NOPTR);
NOPTR Ps2PortDrainOutput(NOPTR);
NOPTR Ps2PortService(NOPTR);
