// Cardputer keyboard driver using M5Cardputer.Keyboard API.
// Fills keymap[256] with PS/2 scancodes for the emulator.

#include "cardputer_kbd.h"
#include "cardputer_display.h"
#include "cardputer_input.h"
#include "hardware.h"
#include "gbConfig.h"
#include "gbGlobals.h"
#include "keys.h"
#include "PS2KeyCode.h"
#include <M5Cardputer.h>
#include <Arduino.h>

static uint8_t osd_cache[256];
static bool osd_cache_inited = false;

void cardputer_kbd_init(void) {
    // Keyboard already initialized by M5Cardputer.begin(true)
    memset((void *)keymap, 1, sizeof(keymap));
    memset((void *)oldKeymap, 1, sizeof(oldKeymap));
    memset(osd_cache, 1, sizeof(osd_cache));
    osd_cache_inited = true;
}

void cardputer_kbd_scan(void) { /* no-op */ }

// char → PS/2 scancode
static uint8_t char_to_sc(char c) {
    // accept both upper and lower case
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    switch (c) {
        case 'a': return PS2_KC_A;
        case 'b': return PS2_KC_B;
        case 'c': return PS2_KC_C;
        case 'd': return PS2_KC_D;
        case 'e': return PS2_KC_E;
        case 'f': return PS2_KC_F;
        case 'g': return PS2_KC_G;
        case 'h': return PS2_KC_H;
        case 'i': return PS2_KC_I;
        case 'j': return PS2_KC_J;
        case 'k': return PS2_KC_K;
        case 'l': return PS2_KC_L;
        case 'm': return PS2_KC_M;
        case 'n': return PS2_KC_N;
        case 'o': return PS2_KC_O;
        case 'p': return PS2_KC_P;
        case 'q': return PS2_KC_Q;
        case 'r': return PS2_KC_R;
        case 's': return PS2_KC_S;
        case 't': return PS2_KC_T;
        case 'u': return PS2_KC_U;
        case 'v': return PS2_KC_V;
        case 'w': return PS2_KC_W;
        case 'x': return PS2_KC_X;
        case 'y': return PS2_KC_Y;
        case 'z': return PS2_KC_Z;
        case '0': return PS2_KC_0;
        case '1': return PS2_KC_1;
        case '2': return PS2_KC_2;
        case '3': return PS2_KC_3;
        case '4': return PS2_KC_4;
        case '5': return PS2_KC_5;
        case '6': return PS2_KC_6;
        case '7': return PS2_KC_7;
        case '8': return PS2_KC_8;
        case '9': return PS2_KC_9;
        /* Shifted half of the number row. M5Cardputer's Keyboard::getKey()
           returns the *shifted* legend while Aa is held, so keysState().word
           contains '!' instead of '1'. Without these cases char_to_sc() fell
           through to "return 0" and only the L_SHIFT make code reached the
           guest, which is why !@#$%^&*() could never be typed. */
        case '!': return PS2_KC_1;
        case '@': return PS2_KC_2;
        case '#': return PS2_KC_3;
        case '$': return PS2_KC_4;
        case '%': return PS2_KC_5;
        case '^': return PS2_KC_6;
        case '&': return PS2_KC_7;
        case '*': return PS2_KC_8;
        case '(': return PS2_KC_9;
        case ')': return PS2_KC_0;
        case '-': case '_': return PS2_KC_MINUS;
        case '=': case '+': return PS2_KC_EQUAL;
        case '[': case '{': return PS2_KC_OPEN_SQ;
        case ']': case '}': return PS2_KC_CLOSE_SQ;
        case '\\':case '|': return PS2_KC_BACK;
        case ';': case ':': return PS2_KC_SEMI;
        case '\'':case '"': return PS2_KC_APOS;
        case ',': case '<': return PS2_KC_COMMA;
        case '.': case '>': return PS2_KC_DOT;
        case '/': case '?': return PS2_KC_DIV;
        case '`': case '~': return PS2_KC_SINGLE;
        default: return 0;
    }
}

void cardputer_kbd_fill_keymap(void) {
    memset((void *)keymap, 1, sizeof(keymap));

    if (!M5Cardputer.Keyboard.isPressed()) return;

    Keyboard_Class::KeysState s = M5Cardputer.Keyboard.keysState();
    const bool fn_layer = s.fn;
    const bool shift_layer = s.shift;

    // Modifiers
    // The physical Aa key is exposed as "shift" by M5Cardputer.
    if (s.shift) keymap[PS2_KC_L_SHIFT] = 0;
    if (s.ctrl)  keymap[PS2_KC_CTRL]    = 0;
    if (s.alt)   keymap[PS2_KC_ALT]     = 0;

    // Special keys
    if (s.enter) keymap[PS2_KC_ENTER]   = 0;
    if (s.del)   keymap[PS2_KC_BS]      = 0;
    if (s.tab)   keymap[PS2_KC_TAB]     = 0;
    if (s.space) keymap[PS2_KC_SPACE]   = 0;

    // Printable chars from state.word
    for (char c : s.word) {
        uint8_t sc = char_to_sc(c);
        if (sc) keymap[sc] = 0;
    }

    // Opt combinations are CardPuter86 display controls. They should not leak
    // as normal DOS keystrokes; Fn combinations remain emulator keys.
    if (s.opt) {
        if (s.space) keymap[PS2_KC_SPACE] = 1;
        static const char display_keys[] = ";,./'";
        static const uint8_t display_scancodes[] = {
            PS2_KC_SEMI, PS2_KC_COMMA, PS2_KC_DOT, PS2_KC_DIV,
            PS2_KC_APOS
        };
        for (uint8_t i = 0; i < 5; i++) {
            if (M5Cardputer.Keyboard.isKeyPressed(display_keys[i])) {
                keymap[display_scancodes[i]] = 1;
            }
        }
    }

    // CardPuter86 function layer: Fn+1..0,-,= maps to F1..F12,
    // Fn+` maps to Esc, and Fn+Backspace maps to Delete.
    if (fn_layer) {
        const uint8_t function_index = cardputer_input_function_index();
        static const uint8_t base_scancodes[] = {
            PS2_KC_1, PS2_KC_2, PS2_KC_3, PS2_KC_4, PS2_KC_5,
            PS2_KC_6, PS2_KC_7, PS2_KC_8, PS2_KC_9, PS2_KC_0,
            PS2_KC_MINUS, PS2_KC_EQUAL
        };
        static const uint8_t function_scancodes[] = {
            PS2_KC_F1, PS2_KC_F2, PS2_KC_F3, PS2_KC_F4, PS2_KC_F5,
            PS2_KC_F6, PS2_KC_F7, PS2_KC_F8, PS2_KC_F9, PS2_KC_F10,
            PS2_KC_F11, PS2_KC_F12
        };
        if (function_index < 12) {
            keymap[base_scancodes[function_index]] = 1;
            keymap[function_scancodes[function_index]] = 0;
        }

        if (cardputer_input_pressed(CARDPUTER_VK_ESC)) {
            keymap[PS2_KC_SINGLE] = 1;
            keymap[PS2_KC_ESC] = 0;
        }
        if (cardputer_input_pressed(CARDPUTER_VK_DEL)) {
            keymap[PS2_KC_BS] = 1;
            keymap[PS2_KC_DELETE] = 0;
        }

        static const uint8_t nav_base[] = {
            PS2_KC_SEMI, PS2_KC_COMMA, PS2_KC_DOT, PS2_KC_DIV
        };
        static const uint8_t nav_sc[] = {
            KEY_CURSOR_UP, KEY_CURSOR_LEFT, KEY_CURSOR_DOWN, KEY_CURSOR_RIGHT
        };
        static const CardputerVirtualKey nav_vk[] = {
            CARDPUTER_VK_UP, CARDPUTER_VK_LEFT, CARDPUTER_VK_DOWN, CARDPUTER_VK_RIGHT
        };
        for (uint8_t i = 0; i < 4; i++) {
            if (cardputer_input_pressed(nav_vk[i])) {
                keymap[nav_base[i]] = 1;
                keymap[nav_sc[i]] = 0;
            }
        }

        if (cardputer_input_pressed(CARDPUTER_VK_AUTO)) keymap[PS2_KC_SPACE] = 1;
    }

    if (shift_layer) {
        const uint8_t symbol_index = cardputer_input_shift_symbol_index();
        static const uint8_t symbol_base_scancodes[] = {
            PS2_KC_SINGLE, PS2_KC_1, PS2_KC_2, PS2_KC_3, PS2_KC_4,
            PS2_KC_5, PS2_KC_6, PS2_KC_7, PS2_KC_8, PS2_KC_9,
            PS2_KC_0, PS2_KC_MINUS, PS2_KC_EQUAL
        };
        if (symbol_index < 13) {
            keymap[symbol_base_scancodes[symbol_index]] = 0;
            keymap[PS2_KC_L_SHIFT] = 0;
        }
    }
}

boolean checkAndCleanKey(uint8_t scancode) {
    if (!osd_cache_inited) { memset(osd_cache, 1, 256); osd_cache_inited = true; }
    bool cur  = (keymap[scancode] == 0);
    bool prev = (osd_cache[scancode] == 0);
    osd_cache[scancode] = keymap[scancode];
    return (cur && !prev);
}

boolean checkKey(uint8_t scancode) {
    return (keymap[scancode] == 0);
}

#ifdef use_lib_log_serial
/* Debug/telnet-style remote keyboard: feed bytes arriving on the USB serial
   port into the guest as PS/2 scancodes.  Only compiled into the *-dbg /
   *-dbglean environments; the shipping lean build never sees it. */
#include <PS2KeyCode.h>

static uint8_t serial_keys[64];
static uint8_t serial_key_shift[64];   // 1 = this key needs L_SHIFT held with it
static bool serial_shift_held = false; // guest currently believes L_SHIFT is down
static uint8_t serial_idle_polls = 0;  // empty polls owed before the next key
static uint8_t serial_key_count = 0;
static uint8_t serial_key_head = 0;
static bool serial_cmd_armed = false;
static int serial_cmd_pending = 0;

/* Characters that live on the shifted half of a US keyboard.  Without this the
   guest BIOS sees only the unshifted character (':' arrived as ';', which broke
   every "drive:" argument), because the older injection path dropped the shift
   state entirely.  The physical keyboard uses the same trick in the symbol
   layer above: keymap[key] = 0 together with keymap[PS2_KC_L_SHIFT] = 0. */
static uint8_t serial_scancode_for(int c, bool *shift) {
    *shift = false;
    if (c >= 'A' && c <= 'Z') {
        *shift = true;
        return char_to_sc((char)(c - 'A' + 'a'));
    }
    switch (c) {
        case ':': *shift = true; return PS2_KC_SEMI;
        case '+': *shift = true; return PS2_KC_EQUAL;
        case '_': *shift = true; return PS2_KC_MINUS;
        case '{': *shift = true; return PS2_KC_OPEN_SQ;
        case '}': *shift = true; return PS2_KC_CLOSE_SQ;
        case '|': *shift = true; return PS2_KC_BACK;
        case '"': *shift = true; return PS2_KC_APOS;
        case '<': *shift = true; return PS2_KC_COMMA;
        case '>': *shift = true; return PS2_KC_DOT;
        case '?': *shift = true; return PS2_KC_DIV;
        case '~': *shift = true; return PS2_KC_SINGLE;
        case '!': *shift = true; return PS2_KC_1;
        case '@': *shift = true; return PS2_KC_2;
        case '#': *shift = true; return PS2_KC_3;
        case '$': *shift = true; return PS2_KC_4;
        case '%': *shift = true; return PS2_KC_5;
        case '^': *shift = true; return PS2_KC_6;
        case '&': *shift = true; return PS2_KC_7;
        case '*': *shift = true; return PS2_KC_8;
        case '(': *shift = true; return PS2_KC_9;
        case ')': *shift = true; return PS2_KC_0;
        default: return char_to_sc((char)c);
    }
}

int cardputer_kbd_serial_take_command(void) {
    const int cmd = serial_cmd_pending;
    serial_cmd_pending = 0;
    return cmd;
}

/* Raw-line mode: while active, incoming bytes are collected verbatim instead of
   being translated into PS/2 scan codes. This exists because the scan-code path
   has to squeeze make/break pairs for shift through a one-key-per-poll guest
   handshake (see cardputer_kbd_serial_apply), which is fine for typing but a bad
   way to hand the firmware a password. */
#define SERIAL_LINE_MAX 96
static char serial_line[SERIAL_LINE_MAX];
static uint8_t serial_line_len = 0;
static bool serial_line_mode = false;
static bool serial_line_ready = false;

void cardputer_kbd_serial_begin_line(void) {
    serial_line_mode = true;
    serial_line_len = 0;
    serial_line_ready = false;
}

const char *cardputer_kbd_serial_take_line(void) {
    if (!serial_line_ready) return NULL;
    serial_line_ready = false;
    return serial_line;
}

/* queue bytes that arrived on the serial port (called early in loop()) */
void cardputer_kbd_serial_inject(void) {
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (serial_line_mode) {
            if (c == '\r' || c == '\n') {
                serial_line[serial_line_len] = '\0';
                serial_line_ready = true;
                serial_line_mode = false;
            } else if ((c == 8 || c == 127) && serial_line_len > 0) {
                serial_line_len--;
            } else if (c >= 32 && c < 127 &&
                       serial_line_len < (SERIAL_LINE_MAX - 1)) {
                serial_line[serial_line_len++] = (char)c;
            }
            continue;
        }
        // 0x01 arms debug command mode so a command byte never turns into a key.
        if (serial_cmd_armed) {
            serial_cmd_armed = false;
            serial_cmd_pending = c;
            continue;
        }
        if (c == 1) {
            serial_cmd_armed = true;
            continue;
        }
        uint8_t sc = 0;
        bool need_shift = false;
        if (c == '\r' || c == '\n') sc = PS2_KC_ENTER;
        else if (c == 8 || c == 127) sc = PS2_KC_BS;
        else if (c == 27) sc = PS2_KC_ESC;
        else if (c == ' ') sc = PS2_KC_SPACE;
        else if (c > 32 && c < 127) sc = serial_scancode_for(c, &need_shift);
        if (sc == 0) continue;
        if (serial_key_count < sizeof(serial_keys)) {
            serial_key_shift[serial_key_count] = need_shift ? 1 : 0;
            serial_keys[serial_key_count++] = sc;
            Serial.printf("[rx] %02X -> %02X sh=%d\n", (unsigned char)c, sc,
                          need_shift ? 1 : 0);
        } else {
            Serial.printf("[rx] %02X DROPPED (queue full)\n", (unsigned char)c);
        }
    }
}

/* called right AFTER cardputer_kbd_fill_keymap(): that function resets the
   whole keymap to "released" on every scan, so an injected key has to be
   applied on top of it.  One key per scan cycle, so a release is seen on the
   next cycle and the PS/2 scanner emits a proper make/break pair. */
void cardputer_kbd_serial_apply(void) {
    if (serial_key_head >= serial_key_count) {
        serial_key_head = 0;
        serial_key_count = 0;
        serial_idle_polls = 0;
        // NOTE: serial_shift_held is deliberately NOT cleared here. Keys arrive
        // one at a time, so this branch runs between every two characters — i.e.
        // exactly while the guest may still be holding L_SHIFT from the previous
        // (shifted) character. Clearing it here made the "release shift first"
        // branch below unreachable, which is why "AT+WIFI=" kept arriving as
        // "AT+WIFI+" and "JY2504" as "JY@504". Worst case it now costs two extra
        // idle polls before the next unshifted key, which is free.
        return;
    }
    // handleinput() in CardPuter86.ino delivers exactly ONE changed key per poll
    // (it returns right after the first difference), and it only advances that
    // one key's shadow state. Two ordering rules therefore apply:
    //   * L_SHIFT make must be delivered in a poll of its own *before* the key it
    //     modifies (so the key make happens while the guest already sees shift);
    //   * L_SHIFT break must be delivered *before* the next unshifted key's make.
    // The old code only did the first half, so after any shifted character the
    // guest still believed shift was down and the following unshifted key came out
    // shifted ("AT+WIFI=" arrived as "AT+WIFI+", JY2504 as JY@504). Those keys are
    // one poll apart at most, hence the explicit idle polls below.
    if (serial_idle_polls > 0) {
        serial_idle_polls--;
        return;                        // keymap stays all-released: nothing to send
    }
    const bool need_shift = serial_key_shift[serial_key_head] != 0;
    if (need_shift && !serial_shift_held) {
        serial_shift_held = true;
        keymap[PS2_KC_L_SHIFT] = 0;
        return;
    }
    if (!need_shift && serial_shift_held) {
        // This poll leaves the keymap empty so handleinput() can push the pending
        // key release; the +1 idle poll then carries the L_SHIFT break. Only after
        // both are out does the next key get injected.
        serial_shift_held = false;
        serial_idle_polls = 1;
        return;
    }
    keymap[serial_keys[serial_key_head]] = 0;
    if (need_shift) keymap[PS2_KC_L_SHIFT] = 0;
    serial_key_head++;
}
#endif