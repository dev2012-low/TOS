#include <Ps2Keyboard.h>
#include <Asm/Io.h>
#include <Asm/Cpu.h>
#include <Kernel/KDriver.h>
#include <Kernel/Return.h>
#include <Kernel/Idt.h>
#include <Kernel/SpinLock.h>
#include <Ioapic.h>
#include <Apic.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Console.h>

/*
 * =============================================================================
 * PS/2 Controller Registers
 * =============================================================================
 */
#define PS2_DATA_PORT           0x60
#define PS2_STATUS_PORT         0x64
#define PS2_COMMAND_PORT        0x64

#define PS2_STATUS_OUTPUT_FULL  (1 << 0)
#define PS2_STATUS_INPUT_FULL   (1 << 1)
#define PS2_STATUS_SYSTEM_FLAG  (1 << 2)
#define PS2_STATUS_COMMAND_DATA (1 << 3)
#define PS2_STATUS_TIMEOUT      (1 << 6)
#define PS2_STATUS_PARITY       (1 << 7)

#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_PORT2   0xA7
#define PS2_CMD_ENABLE_PORT2    0xA8
#define PS2_CMD_TEST_PORT2      0xA9
#define PS2_CMD_TEST_CTRL       0xAA
#define PS2_CMD_TEST_PORT1      0xAB
#define PS2_CMD_DISABLE_PORT1   0xAD
#define PS2_CMD_ENABLE_PORT1    0xAE

#define PS2_CONFIG_PORT1_INT    (1 << 0)
#define PS2_CONFIG_PORT2_INT    (1 << 1)
#define PS2_CONFIG_SYSTEM_FLAG  (1 << 2)
#define PS2_CONFIG_PORT1_CLK    (1 << 4)
#define PS2_CONFIG_PORT2_CLK    (1 << 5)
#define PS2_CONFIG_PORT1_TRANS  (1 << 6)
#define PS2_CONFIG_PORT2_TRANS  (1 << 7)

#define PS2_CMD_SET_LEDS        0xED
#define PS2_CMD_ECHO            0xEE
#define PS2_CMD_SCAN_CODE_SET   0xF0
#define PS2_CMD_IDENTIFY        0xF2
#define PS2_CMD_SET_TYPEMATIC   0xF3
#define PS2_CMD_ENABLE_SCAN     0xF4
#define PS2_CMD_DISABLE_SCAN    0xF5
#define PS2_CMD_SET_DEFAULTS    0xF6
#define PS2_CMD_RESEND          0xFE
#define PS2_CMD_RESET           0xFF

#define PS2_ACK                 0xFA
#define PS2_RESEND              0xFE
#define PS2_BAT_SUCCESS         0xAA
#define PS2_ECHO_RESPONSE       0xEE

/*
 * =============================================================================
 * US Keyboard Layout Tables (Set 1)
 * =============================================================================
 */
static const CHAR UsLower[128] = {
    0,    0,    '1',  '2',  '3',  '4',  '5',  '6',  // 00-07
    '7',  '8',  '9',  '0',  '-',  '=',  0,    0,    // 08-0F
    'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  // 10-17
    'o',  'p',  '[',  ']',  0,    0,    'a',  's',  // 18-1F
    'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';',  // 20-27
    '\'', '`',  0,    '\\', 'z',  'x',  'c',  'v',  // 28-2F
    'b',  'n',  'm',  ',',  '.',  '/',  0,    '*',  // 30-37
    0,    ' ',  0,    0,    0,    0,    0,    0,    // 38-3F
    0,    0,    0,    0,    0,    0,    0,    '7',  // 40-47
    '8',  '9',  '-',  '4',  '5',  '6',  '+',  '1',  // 48-4F
    '2',  '3',  '0',  '.',  0,    0,    0,    0,    // 50-57
    0,    0,    0,    0,    0,    0,    0,    0,    // 58-5F
};

static const CHAR UsUpper[128] = {
    0,    0,    '!',  '@',  '#',  '$',  '%',  '^',  // 00-07
    '&',  '*',  '(',  ')',  '_',  '+',  0,    0,    // 08-0F
    'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',  // 10-17
    'O',  'P',  '{',  '}',  0,    0,    'A',  'S',  // 18-1F
    'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',  // 20-27
    '"',  '~',  0,    '|',  'Z',  'X',  'C',  'V',  // 28-2F
    'B',  'N',  'M',  '<',  '>',  '?',  0,    '*',  // 30-37
    0,    ' ',  0,    0,    0,    0,    0,    0,    // 38-3F
};

/*
 * Set 2 make code -> internal Set 1 key code (OSDev PS/2 tables).
 */
static const UINT8 GSet2ToSet1[256] = {
    [0x01] = PS2_KEY_F9,
    [0x03] = PS2_KEY_F5,
    [0x04] = PS2_KEY_F3,
    [0x05] = PS2_KEY_F1,
    [0x06] = PS2_KEY_F2,
    [0x07] = PS2_KEY_F12,
    [0x09] = PS2_KEY_F10,
    [0x0A] = PS2_KEY_F8,
    [0x0B] = PS2_KEY_F6,
    [0x0C] = PS2_KEY_F4,
    [0x0D] = PS2_KEY_TAB,
    [0x0E] = PS2_KEY_GRAVE,
    [0x11] = PS2_KEY_LALT,
    [0x12] = PS2_KEY_LSHIFT,
    [0x14] = PS2_KEY_LCTRL,
    [0x15] = PS2_KEY_Q,
    [0x16] = PS2_KEY_1,
    [0x1A] = PS2_KEY_Z,
    [0x1B] = PS2_KEY_S,
    [0x1C] = PS2_KEY_A,
    [0x1D] = PS2_KEY_W,
    [0x1E] = PS2_KEY_2,
    [0x21] = PS2_KEY_C,
    [0x22] = PS2_KEY_X,
    [0x23] = PS2_KEY_D,
    [0x24] = PS2_KEY_E,
    [0x25] = PS2_KEY_4,
    [0x26] = PS2_KEY_3,
    [0x29] = PS2_KEY_SPACE,
    [0x2A] = PS2_KEY_V,
    [0x2B] = PS2_KEY_F,
    [0x2C] = PS2_KEY_T,
    [0x2D] = PS2_KEY_R,
    [0x2E] = PS2_KEY_5,
    [0x31] = PS2_KEY_N,
    [0x32] = PS2_KEY_B,
    [0x33] = PS2_KEY_H,
    [0x34] = PS2_KEY_G,
    [0x35] = PS2_KEY_Y,
    [0x36] = PS2_KEY_6,
    [0x3A] = PS2_KEY_M,
    [0x3B] = PS2_KEY_J,
    [0x3C] = PS2_KEY_U,
    [0x3D] = PS2_KEY_7,
    [0x3E] = PS2_KEY_8,
    [0x41] = PS2_KEY_COMMA,
    [0x42] = PS2_KEY_K,
    [0x43] = PS2_KEY_I,
    [0x44] = PS2_KEY_O,
    [0x45] = PS2_KEY_0,
    [0x46] = PS2_KEY_9,
    [0x49] = PS2_KEY_PERIOD,
    [0x4A] = PS2_KEY_SLASH,
    [0x4B] = PS2_KEY_L,
    [0x4C] = PS2_KEY_SEMICOLON,
    [0x4D] = PS2_KEY_P,
    [0x4E] = PS2_KEY_MINUS,
    [0x52] = PS2_KEY_APOSTROPHE,
    [0x54] = PS2_KEY_LBRACKET,
    [0x55] = PS2_KEY_EQUAL,
    [0x58] = PS2_KEY_CAPSLOCK,
    [0x59] = PS2_KEY_RSHIFT,
    [0x5A] = PS2_KEY_ENTER,
    [0x5B] = PS2_KEY_RBRACKET,
    [0x5D] = PS2_KEY_BACKSLASH,
    [0x66] = PS2_KEY_BACKSPACE,
    [0x69] = PS2_KEY_KP_1,
    [0x6B] = PS2_KEY_KP_4,
    [0x6C] = PS2_KEY_KP_7,
    [0x70] = PS2_KEY_KP_0,
    [0x71] = PS2_KEY_KP_PERIOD,
    [0x72] = PS2_KEY_KP_2,
    [0x73] = PS2_KEY_KP_5,
    [0x74] = PS2_KEY_KP_6,
    [0x75] = PS2_KEY_KP_8,
    [0x76] = PS2_KEY_ESC,
    [0x77] = PS2_KEY_NUMLOCK,
    [0x78] = PS2_KEY_F11,
    [0x79] = PS2_KEY_KP_PLUS,
    [0x7A] = PS2_KEY_KP_3,
    [0x7B] = PS2_KEY_KP_MINUS,
    [0x7C] = PS2_KEY_KP_ASTERISK,
    [0x7D] = PS2_KEY_KP_9,
    [0x7E] = PS2_KEY_SCROLLLOCK,
    [0x83] = PS2_KEY_F7,
};

/*
 * =============================================================================
 * Driver State
 * =============================================================================
 */
typedef struct {
    BOOL Initialized;
    BOOL Enabled;
    UINT8 ScanCodeSet;
    UINT8 Modifiers;
    BOOL KeyStates[128];
    BOOL ExtendedPrefix;
    BOOL BreakPrefix;
    BOOL PausePrefix;
    KeyboardLayout Layout;
    
    Ps2Subscriber Subscribers[PS2_MAX_SUBSCRIBERS];
    
    UINT64 EventCount;
    UINT64 ErrorCount;
    
    KDriver *Driver;
    UINT32 Gsi;
} Ps2KeyboardState;

static Ps2KeyboardState GPs2 = {0};

#define PS2_KEY_QUEUE_SIZE 64
#define PS2_DRAIN_MAX_BYTES 256

static struct {
    KeyEvent Events[PS2_KEY_QUEUE_SIZE];
    volatile UINT32 Head;
    volatile UINT32 Tail;
} GPs2KeyQueue = {0};

static SpinLock GPs2PortLock;

static BOOL Ps2KeyQueuePush(const KeyEvent *Event) {
    UINT32 Next = (GPs2KeyQueue.Head + 1) % PS2_KEY_QUEUE_SIZE;
    if (Next == GPs2KeyQueue.Tail) {
        GPs2.ErrorCount++;
        return FALSE;
    }
    GPs2KeyQueue.Events[GPs2KeyQueue.Head] = *Event;
    GPs2KeyQueue.Head = Next;
    return TRUE;
}

static BOOL Ps2KeyQueuePop(KeyEvent *Event) {
    if (GPs2KeyQueue.Head == GPs2KeyQueue.Tail) {
        return FALSE;
    }
    *Event = GPs2KeyQueue.Events[GPs2KeyQueue.Tail];
    GPs2KeyQueue.Tail = (GPs2KeyQueue.Tail + 1) % PS2_KEY_QUEUE_SIZE;
    return TRUE;
}

/*
 * =============================================================================
 * PS/2 Controller Low-Level
 * =============================================================================
 */
static BOOL Ps2WaitWrite(NOPTR) {
    for (INT I = 0; I < 1000; I++) {
        if (!(Inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) {
            return TRUE;
        }
        CpuPause();
    }
    return FALSE;
}

static BOOL Ps2WaitRead(NOPTR) {
    for (INT I = 0; I < 1000; I++) {
        if (Inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            return TRUE;
        }
        CpuPause();
    }
    return FALSE;
}

static UINT8 Ps2ReadData(NOPTR) {
    if (!Ps2WaitRead()) return 0;
    return Inb(PS2_DATA_PORT);
}

static BOOL Ps2WriteData(UINT8 Data) {
    if (!Ps2WaitWrite()) return FALSE;
    Outb(PS2_DATA_PORT, Data);
    return TRUE;
}

static BOOL Ps2WriteCommand(UINT8 Cmd) {
    if (!Ps2WaitWrite()) return FALSE;
    Outb(PS2_COMMAND_PORT, Cmd);
    return TRUE;
}

static UINT8 Ps2ReadConfig(NOPTR) {
    if (!Ps2WriteCommand(PS2_CMD_READ_CONFIG)) return 0;
    return Ps2ReadData();
}

static BOOL Ps2WriteConfig(UINT8 Config) {
    if (!Ps2WriteCommand(PS2_CMD_WRITE_CONFIG)) return FALSE;
    return Ps2WriteData(Config);
}

static NOPTR Ps2FlushOutput(NOPTR) {
    for (INT I = 0; I < 32; I++) {
        if (!(Inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) {
            return;
        }
        Inb(PS2_DATA_PORT);
        CpuPause();
    }
}

static BOOL Ps2SendCommand(UINT8 Cmd) {
    INT Retries = 3;
    
    while (Retries--) {
        if (!Ps2WriteData(Cmd)) continue;
        
        UINT8 Response = Ps2ReadData();
        if (Response == PS2_ACK) return TRUE;
        if (Response == PS2_RESEND) continue;
    }
    
    return FALSE;
}

/*
 * =============================================================================
 * Keyboard LED Control
 * =============================================================================
 */
NOPTR Ps2KeyboardSetLeds(BOOL NumLock, BOOL CapsLock, BOOL ScrollLock) {
    if (!GPs2.Initialized || !GPs2.Enabled) return;
    
    UINT8 Leds = 0;
    if (NumLock) Leds |= (1 << 0);
    if (CapsLock) Leds |= (1 << 1);
    if (ScrollLock) Leds |= (1 << 2);
    
    Ps2SendCommand(PS2_CMD_SET_LEDS);
    Ps2WriteData(Leds);
}

/*
 * =============================================================================
 * Keyboard Configuration
 * =============================================================================
 */
static BOOL Ps2KeyboardReset(NOPTR) {
    if (!Ps2SendCommand(PS2_CMD_RESET)) return FALSE;
    
    for (INT I = 0; I < 50; I++) {
        if (!Ps2WaitRead()) {
            TimerMdelay(1);
            continue;
        }
        UINT8 Response = Inb(PS2_DATA_PORT);
        if (Response == PS2_BAT_SUCCESS) {
            Ps2FlushOutput();
            TimerMdelay(50);
            return TRUE;
        }
        if (Response == PS2_ACK) continue;
        CpuPause();
    }
    
    return FALSE;
}

static BOOL Ps2KeyboardEnable(NOPTR) {
    return Ps2SendCommand(PS2_CMD_ENABLE_SCAN);
}

static BOOL Ps2KeyboardDisable(NOPTR) {
    return Ps2SendCommand(PS2_CMD_DISABLE_SCAN);
}

static UINT8 Ps2KeyboardQueryScanCodeSetRaw(NOPTR) {
    Ps2FlushOutput();
    if (!Ps2SendCommand(PS2_CMD_SCAN_CODE_SET)) {
        return 0;
    }
    if (!Ps2WriteData(0)) {
        return 0;
    }
    if (Ps2ReadData() != PS2_ACK) {
        return 0;
    }
    return Ps2ReadData();
}

static UINT8 Ps2NormalizeScanCodeSet(UINT8 RawId) {
    switch (RawId) {
        case PS2_SCANCODE_SET1_ID:
        case 0x01:
            return 1;
        case PS2_SCANCODE_SET2_ID:
        case 0x02:
            return 2;
        case PS2_SCANCODE_SET3_ID:
        case 0x03:
            return 3;
        default:
            return 0;
    }
}

static BOOL Ps2KeyboardSetScanCodeSet(UINT8 Set) {
    Ps2FlushOutput();
    if (!Ps2SendCommand(PS2_CMD_SCAN_CODE_SET)) {
        return FALSE;
    }
    if (!Ps2WriteData(Set)) {
        return FALSE;
    }
    return (Ps2ReadData() == PS2_ACK);
}

static UINT8 Ps2TranslateToSet1(UINT8 RawCode, UINT8 ScanCodeSet) {
    UINT8 Translated;

    if (ScanCodeSet == 1) {
        return RawCode;
    }
    if (ScanCodeSet == 3 && RawCode == 0x08) {
        return PS2_KEY_ESC;
    }

    if (ScanCodeSet == 2 || ScanCodeSet == 3) {
        Translated = GSet2ToSet1[RawCode];
    } else {
        return 0;
    }
    return Translated ? Translated : RawCode;
}

static BOOL Ps2KeyboardConfigureScanCodeSet(NOPTR) {
    UINT8 RawId = Ps2KeyboardQueryScanCodeSetRaw();
    UINT8 Set = Ps2NormalizeScanCodeSet(RawId);

    if (Set == 0) {
        Set = 2;
    }

    if (Set != 1) {
        if (Ps2KeyboardSetScanCodeSet(1)) {
            RawId = Ps2KeyboardQueryScanCodeSetRaw();
            Set = Ps2NormalizeScanCodeSet(RawId);
        }
    }

    if (Set == 3) {
        if (Ps2KeyboardSetScanCodeSet(2)) {
            Set = 2;
        }
    }

    if (Set == 0) {
        Set = 2;
    }

    GPs2.ScanCodeSet = Set;
    return TRUE;
}

/*
 * =============================================================================
 * Scan Code Translation
 * =============================================================================
 */
static CHAR ScanCodeToAscii(UINT8 ScanCode, BOOL Pressed, UINT8 Modifiers, KeyboardLayout Layout) {
    if (!Pressed) return 0;
    if (ScanCode >= 128) return 0;
    
    (NOPTR)Layout; // For future UK/RU layouts
    
    const CHAR *Table = (Modifiers & MOD_SHIFT) ? UsUpper : UsLower;
    CHAR Ascii = Table[ScanCode];
    
    if ((Modifiers & MOD_CAPSLOCK) && (Ascii >= 'a' && Ascii <= 'z')) {
        Ascii = Ascii - 'a' + 'A';
    } else if ((Modifiers & MOD_CAPSLOCK) && (Ascii >= 'A' && Ascii <= 'Z')) {
        Ascii = Ascii - 'A' + 'a';
    }
    
    if ((Modifiers & MOD_CTRL) && (Ascii >= 'a' && Ascii <= 'z')) {
        Ascii = Ascii - 'a' + 1;
    } else if ((Modifiers & MOD_CTRL) && (Ascii >= 'A' && Ascii <= 'Z')) {
        Ascii = Ascii - 'A' + 1;
    }
    
    return Ascii;
}

static UINT8 MapExtendedKey(UINT8 RawCode) {
    switch (RawCode) {
        case PS2_KEY_KP_ENTER:  return PS2_KEY_ENTER;
        case PS2_KEY_RCTRL:     return PS2_KEY_LCTRL;
        case PS2_KEY_KP_SLASH:  return PS2_KEY_SLASH;
        case PS2_KEY_RALT:      return PS2_KEY_LALT;
        case PS2_KEY_HOME:      return PS2_KEY_KP_7;
        case PS2_KEY_UP:        return PS2_KEY_KP_8;
        case PS2_KEY_PGUP:      return PS2_KEY_KP_9;
        case PS2_KEY_LEFT:      return PS2_KEY_KP_4;
        case PS2_KEY_RIGHT:     return PS2_KEY_KP_6;
        case PS2_KEY_END:       return PS2_KEY_KP_1;
        case PS2_KEY_DOWN:      return PS2_KEY_KP_2;
        case PS2_KEY_PGDN:      return PS2_KEY_KP_3;
        case PS2_KEY_INSERT:    return PS2_KEY_KP_0;
        case PS2_KEY_DELETE:    return PS2_KEY_KP_PERIOD;
        default:                return RawCode;
    }
}

/*
 * =============================================================================
 * Modifier Handling
 * =============================================================================
 */
static NOPTR UpdateModifiers(UINT8 RawCode, BOOL Pressed, BOOL Extended) {
    if (!Extended) {
        switch (RawCode) {
            case PS2_KEY_LSHIFT:
            case PS2_KEY_RSHIFT:
                if (Pressed) GPs2.Modifiers |= MOD_SHIFT;
                else GPs2.Modifiers &= ~MOD_SHIFT;
                break;
                
            case PS2_KEY_LCTRL:
                if (Pressed) GPs2.Modifiers |= MOD_CTRL;
                else GPs2.Modifiers &= ~MOD_CTRL;
                break;
                
            case PS2_KEY_LALT:
                if (Pressed) GPs2.Modifiers |= MOD_ALT;
                else GPs2.Modifiers &= ~MOD_ALT;
                break;
                
            case PS2_KEY_CAPSLOCK:
                if (Pressed && !GPs2.KeyStates[RawCode]) {
                    GPs2.Modifiers ^= MOD_CAPSLOCK;
                    Ps2KeyboardSetLeds(
                        (GPs2.Modifiers & MOD_NUMLOCK) ? TRUE : FALSE,
                        (GPs2.Modifiers & MOD_CAPSLOCK) ? TRUE : FALSE,
                        (GPs2.Modifiers & MOD_SCROLLLOCK) ? TRUE : FALSE
                    );
                }
                break;
                
            case PS2_KEY_NUMLOCK:
                if (Pressed && !GPs2.KeyStates[RawCode]) {
                    GPs2.Modifiers ^= MOD_NUMLOCK;
                    Ps2KeyboardSetLeds(
                        (GPs2.Modifiers & MOD_NUMLOCK) ? TRUE : FALSE,
                        (GPs2.Modifiers & MOD_CAPSLOCK) ? TRUE : FALSE,
                        (GPs2.Modifiers & MOD_SCROLLLOCK) ? TRUE : FALSE
                    );
                }
                break;
                
            case PS2_KEY_SCROLLLOCK:
                if (Pressed && !GPs2.KeyStates[RawCode]) {
                    GPs2.Modifiers ^= MOD_SCROLLLOCK;
                    Ps2KeyboardSetLeds(
                        (GPs2.Modifiers & MOD_NUMLOCK) ? TRUE : FALSE,
                        (GPs2.Modifiers & MOD_CAPSLOCK) ? TRUE : FALSE,
                        (GPs2.Modifiers & MOD_SCROLLLOCK) ? TRUE : FALSE
                    );
                }
                break;
        }
    } else {
        switch (RawCode) {
            case PS2_KEY_RCTRL:
                if (Pressed) GPs2.Modifiers |= MOD_CTRL;
                else GPs2.Modifiers &= ~MOD_CTRL;
                break;
                
            case PS2_KEY_RALT:
                if (Pressed) GPs2.Modifiers |= MOD_ALT;
                else GPs2.Modifiers &= ~MOD_ALT;
                break;
        }
    }
}

/*
 * =============================================================================
 * Key Event Processing
 * =============================================================================
 */
static NOPTR ProcessKeyEvent(UINT8 ScanCode, BOOL Extended) {
    BOOL Pressed = !(ScanCode & 0x80);
    UINT8 RawCode = ScanCode & 0x7F;
    UINT8 KeyCode = Extended ? MapExtendedKey(RawCode) : RawCode;
    CHAR Ascii = 0;
    
    GPs2.KeyStates[KeyCode] = Pressed;
    
    BOOL IsKeypad = (RawCode >= PS2_KEY_KP_7 && RawCode <= PS2_KEY_KP_PERIOD) ||
                    RawCode == PS2_KEY_KP_MINUS || RawCode == PS2_KEY_KP_PLUS ||
                    RawCode == PS2_KEY_KP_ASTERISK;
    
    if (IsKeypad && (GPs2.Modifiers & MOD_NUMLOCK)) {
        switch (RawCode) {
            case PS2_KEY_KP_0:       Ascii = '0'; break;
            case PS2_KEY_KP_1:       Ascii = '1'; break;
            case PS2_KEY_KP_2:       Ascii = '2'; break;
            case PS2_KEY_KP_3:       Ascii = '3'; break;
            case PS2_KEY_KP_4:       Ascii = '4'; break;
            case PS2_KEY_KP_5:       Ascii = '5'; break;
            case PS2_KEY_KP_6:       Ascii = '6'; break;
            case PS2_KEY_KP_7:       Ascii = '7'; break;
            case PS2_KEY_KP_8:       Ascii = '8'; break;
            case PS2_KEY_KP_9:       Ascii = '9'; break;
            case PS2_KEY_KP_PERIOD:  Ascii = '.'; break;
            case PS2_KEY_KP_PLUS:    Ascii = '+'; break;
            case PS2_KEY_KP_MINUS:   Ascii = '-'; break;
            case PS2_KEY_KP_ASTERISK: Ascii = '*'; break;
            case PS2_KEY_KP_SLASH:   Ascii = '/'; break;
            default: break;
        }
    } else if (!IsKeypad) {
        Ascii = ScanCodeToAscii(RawCode, Pressed, GPs2.Modifiers, GPs2.Layout);
    }
    
    if (Ascii == 0 && Pressed) {
        switch (KeyCode) {
            case PS2_KEY_TAB:        Ascii = '\t'; break;
            case PS2_KEY_ENTER:      Ascii = '\n'; break;
            case PS2_KEY_BACKSPACE:  Ascii = '\b'; break;
            case PS2_KEY_ESC:        Ascii = 27; break;
        }
    }
    
    KeyEvent Event;
    Event.ScanCode = KeyCode;
    Event.Ascii = Ascii;
    Event.RawScanCode = RawCode;
    Event.Extended = Extended;
    Event.State = Pressed ? KEY_STATE_PRESSED : KEY_STATE_RELEASED;
    Event.Modifiers = GPs2.Modifiers;
    
    GPs2.EventCount++;
    
    Ps2KeyQueuePush(&Event);
}

NOPTR Ps2HandleScanByte(UINT8 Byte) {
    if (Byte == PS2_KEY_EXT_PREFIX) {
        GPs2.ExtendedPrefix = TRUE;
        return;
    }

    if (Byte == PS2_KEY_PAUSE_PREFIX) {
        GPs2.PausePrefix = TRUE;
        return;
    }

    if (GPs2.PausePrefix) {
        GPs2.PausePrefix = FALSE;
        return;
    }

    if (GPs2.ScanCodeSet >= 2) {
        if (Byte == PS2_KEY_BREAK_PREFIX) {
            GPs2.BreakPrefix = TRUE;
            return;
        }

        BOOL Extended = GPs2.ExtendedPrefix;
        GPs2.ExtendedPrefix = FALSE;
        BOOL Pressed = !GPs2.BreakPrefix;
        GPs2.BreakPrefix = FALSE;

        UINT8 RawCode = Ps2TranslateToSet1(Byte, GPs2.ScanCodeSet);
        if (RawCode == 0) {
            return;
        }

        UINT8 ScanCode = Pressed ? RawCode : (UINT8)(RawCode | 0x80);
        UpdateModifiers(RawCode, Pressed, Extended);
        ProcessKeyEvent(ScanCode, Extended);
        return;
    }

    if (Byte == PS2_KEY_BREAK_PREFIX) {
        return;
    }

    BOOL Extended = GPs2.ExtendedPrefix;
    GPs2.ExtendedPrefix = FALSE;

    UpdateModifiers(Byte & 0x7F, !(Byte & 0x80), Extended);
    ProcessKeyEvent(Byte, Extended);
}

static NOPTR Ps2PortDrainOutputLocked(NOPTR) {
    INT I;

    for (I = 0; I < PS2_DRAIN_MAX_BYTES; I++) {
        if (!(Inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) {
            return;
        }
        Ps2HandleScanByte(Inb(PS2_DATA_PORT));
    }

    /* Защита от зависания при залипшем буфере PS/2 (мышь, ошибка контроллера). */
    Ps2FlushOutput();
}

NOPTR Ps2PortDrainOutput(NOPTR) {
    SpinLockAcquire(&GPs2PortLock);
    Ps2PortDrainOutputLocked();
    SpinLockRelease(&GPs2PortLock);
}

NOPTR Ps2PortService(NOPTR) {
    Ps2PortDrainOutput();
    Ps2KeyboardDispatchEvents();
}

/*
 * =============================================================================
 * IRQ Handler
 * =============================================================================
 */
NOPTR Ps2KeyboardIrqHandler(NOPTR) {
    // IRQ уже атомарный — спинлок НЕ НУЖЕН!
    Ps2PortDrainOutputLocked();
    ApicEoi();
}

NOPTR Ps2KeyboardService(NOPTR) {
    if (!GPs2.Initialized) {
        return;
    }
    Ps2PortService();
    Ps2KeyboardDispatchEvents();
}

/*
 * =============================================================================
 * Driver Shutdown
 * =============================================================================
 */
static NOPTR Ps2KeyboardShutdown(KDriver *Driver) {
    (NOPTR)Driver;
    
    if (GPs2.Enabled) {
        Ps2KeyboardDisable();
        GPs2.Enabled = FALSE;
    }
    
    if (GPs2.Gsi) {
        IoapicMaskIrq(GPs2.Gsi);
    }
    
    GPs2.Initialized = FALSE;
}

/*
 * =============================================================================
 * Public API
 * =============================================================================
 */
BOOL Ps2KeyboardPollEvent(KeyEvent *Event) {
    if (!Event) {
        return FALSE;
    }
    if (!(Inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) {
        return FALSE;
    }

    Ps2HandleScanByte(Inb(PS2_DATA_PORT));
    return Ps2KeyQueuePop(Event);
}

BOOL Ps2KeyboardIsPressed(UINT8 ScanCode) {
    if (ScanCode >= 128) return FALSE;
    return GPs2.KeyStates[ScanCode];
}

UINT8 Ps2KeyboardGetModifiers(NOPTR) {
    return GPs2.Modifiers;
}

UINT8 Ps2KeyboardGetScanCodeSet(NOPTR) {
    return GPs2.ScanCodeSet;
}

INT Ps2KeyboardSetLayout(KeyboardLayout Layout) {
    if (Layout > KB_LAYOUT_RU) RETURN(INCORRECT_VALUE);
    GPs2.Layout = Layout;
    RETURN(SUCCESS);
}

KeyboardLayout Ps2KeyboardGetLayout(NOPTR) {
    return GPs2.Layout;
}

INT Ps2KeyboardSubscribe(KeyboardCallback cb, NOPTR *ud) {
    if (!cb) RETURN(NO_OBJECT);

    for (UINT32 i = 0; i < PS2_MAX_SUBSCRIBERS; ++i) {
        if (!GPs2.Subscribers[i].Active) {
            GPs2.Subscribers[i].Callback = cb;
            GPs2.Subscribers[i].UserData = ud;
            GPs2.Subscribers[i].Active = TRUE;
            RETURN(SUCCESS);
        }
    }

    RETURN(NO_MEMORY); // Нет места
}

INT Ps2KeyboardUnsubscribe(KeyboardCallback cb) {
    if (!cb) RETURN(NO_OBJECT);

    BOOL found = FALSE;
    for (UINT32 i = 0; i < PS2_MAX_SUBSCRIBERS; ++i) {
        if (GPs2.Subscribers[i].Active && GPs2.Subscribers[i].Callback == cb) {
            GPs2.Subscribers[i].Callback = NULLPTR;
            GPs2.Subscribers[i].UserData = NULLPTR;
            GPs2.Subscribers[i].Active = FALSE;
            found = TRUE;
        }
    }

    return found ? SUCCESS : NOT_FOUND;
}


INT Ps2KeyboardDispatchEvents(NOPTR) {
    KeyEvent Event;
    INT Count = 0;

    while (Ps2KeyQueuePop(&Event)) {
        for (UINT32 i = 0; i < PS2_MAX_SUBSCRIBERS; ++i) {
            if (GPs2.Subscribers[i].Active && GPs2.Subscribers[i].Callback) {
                GPs2.Subscribers[i].Callback(&Event, GPs2.Subscribers[i].UserData);
            }
        }
        Count++;
    }

    RETURN(Count);
}


/*
 * =============================================================================
 * Initialization
 * =============================================================================
 */

EXTERN(NOPTR, Ps2KeyboardIrq());

INT Ps2KeyboardInit(NOPTR) {
    UINT8 Config;
    UINT32 Gsi, Flags;
    INT Result;
    
    if (GPs2.Initialized) {
        RETURN(SUCCESS);
    }
    
    MemSet(&GPs2, 0, sizeof(Ps2KeyboardState));

    for (UINT32 i = 0; i < PS2_MAX_SUBSCRIBERS; ++i) {
        GPs2.Subscribers[i].Callback = NULLPTR;
        GPs2.Subscribers[i].UserData = NULLPTR;
        GPs2.Subscribers[i].Active = FALSE;
    }

    MemSet(&GPs2KeyQueue, 0, sizeof(GPs2KeyQueue));
    SpinLockInit(&GPs2PortLock);
    GPs2.Layout = KB_LAYOUT_US;

    // Disable both ports temporarily
    Ps2WriteCommand(PS2_CMD_DISABLE_PORT1);
    Ps2WriteCommand(PS2_CMD_DISABLE_PORT2);
    Ps2FlushOutput();
    
    // Reset config
    Config = Ps2ReadConfig();
    Config &= ~PS2_CONFIG_PORT1_INT;
    Config &= ~PS2_CONFIG_PORT2_INT;
    Config &= ~PS2_CONFIG_PORT1_TRANS;
    Config &= ~PS2_CONFIG_PORT2_CLK;
    Ps2WriteConfig(Config);
    
    // Test controller
    Ps2WriteCommand(PS2_CMD_TEST_CTRL);
    if (Ps2ReadData() != 0x55) {
        RETURN(INCORRECT_VALUE);
    }
    
    
    // Включаем порт 1 (клавиатура)
    Ps2WriteCommand(PS2_CMD_ENABLE_PORT1);
    
    
    // Тестируем порт 1
    Ps2WriteCommand(PS2_CMD_TEST_PORT1);
    if (Ps2ReadData() != 0x00) {
        ConsolePrint("[PS2] port1 test failed\n");
        // Но продолжаем — может, мышь спасёт ситуацию
    }

    // Инициализируем клавиатуру
    if (!Ps2KeyboardReset()) {
        RETURN(NO_OBJECT);
    }

    if (!Ps2KeyboardConfigureScanCodeSet()) {
        RETURN(NO_OBJECT);
    }
    
    if (!Ps2KeyboardEnable()) {
        RETURN(NO_OBJECT);
    }
    
    Ps2FlushOutput();
    
    GPs2.Enabled = TRUE;
    GPs2.Initialized = TRUE;
    
    GPs2.Driver = KDriverGenerateStruct("PS2Keyboard", DCL0, TRUE, &GPs2, Ps2KeyboardShutdown);
    if (GPs2.Driver) {
        KDriverRegister(GPs2.Driver);
    }
    
    // Настройка IRQ для клавиатуры
    Result = IoapicGetOverride(1, &Gsi, &Flags);
    if (IsError(Result).IsError) {
        Gsi = 1;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }
    
    GPs2.Gsi = Gsi;
    
    IdtSetGate(KEYBOARD, Ps2KeyboardIrq, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    
    Result = IoapicRedirectIrq(Gsi, KEYBOARD, ApicGetId(), Flags);
    if (IsError(Result).IsError) {
        RETURN(INCORRECT_VALUE);
    }
    
    Ps2KeyboardSetLeds(FALSE, FALSE, FALSE);
    Ps2FlushOutput();
    
    // ========== НАСТРАИВАЕМ КОНФИГУРАЦИЮ ДЛЯ ОБОИХ ПОРТОВ ==========
    Config = Ps2ReadConfig();
    Config |= PS2_CONFIG_PORT1_INT;   // IRQ для клавиатуры
    Config &= ~PS2_CONFIG_PORT2_INT;
    Config &= ~PS2_CONFIG_PORT2_CLK;
    Config &= ~PS2_CONFIG_PORT2_TRANS;
    Ps2WriteConfig(Config);
    
    Ps2WriteCommand(PS2_CMD_ENABLE_PORT1);
   
    // Включаем IRQ для клавиатуры
    IoapicUnmaskIrq(Gsi);

    
    RETURN(SUCCESS);
}
