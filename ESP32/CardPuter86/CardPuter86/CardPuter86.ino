// CardPuter86 - 8086 PC emulator for M5Stack Cardputer
//Original: ESP32TinyFake86 by ackerman (TTGO VGA32)
//Cardputer port: 8086 PC emulator on ESP32-S3 with ST7789 TFT

#include <Arduino.h>
#include <esp_timer.h>
#include <esp_log.h>
#include "gbConfig.h"
#include "gbGlobals.h"
#include "fake86.h"
#include "hardware.h"
#include "driver/timer.h"
#include "soc/timer_group_struct.h"
#include "input.h"
#include "i8253.h"
#include "i8259.h"
#include "i8237.h"
#include "cpu.h"
#include "render.h"
#include "video.h"
#include "timing.h"
#include "disk.h"
#include "PS2KeyCode.h"
#include "keys.h"
#include "dataFlash/com/comdigger.h"
#include "dataFlash/gbcom.h"
#include "gb_sdl_font8x8.h"
#include "osd.h"

// Cardputer drivers
#include "cardputer_display.h"
#include "cardputer_input.h"
#include "cardputer_kbd.h"
#include "cardputer_speaker.h"
#include "cardputer_storage.h"
#include "cardputer_cpu.h"
#include "cardputer_power.h"
#include "cardputer_rtc.h"
#include "cardputer_settings.h"
#include "cardputer_modem.h"
#include "cardputer_btkbd.h"
#include "guest_memory.h"

// ===============================================
// Global variables (defined here, extern in gbGlobals.h)
// ===============================================
unsigned char gb_invert_color = 0;
unsigned char gb_silence = 0;

unsigned char gb_delay_tick_cpu_milis = use_lib_delay_tick_cpu_milis;
unsigned char gb_auto_delay_cpu = use_lib_delay_tick_cpu_auto;
unsigned char gb_vga_poll_milis = use_lib_vga_poll_milis;
unsigned char gb_keyboard_poll_milis = use_lib_keyboard_poll_milis;
unsigned char gb_timers_poll_milis = use_lib_timers_poll_milis;

unsigned char gb_frec_speaker_low = 0;
unsigned char gb_frec_speaker_high = 0;
unsigned char gb_cont_frec_speaker = 0;
volatile int gb_frecuencia01 = 0;
volatile int gb_volumen01 = 0;

unsigned char gb_use_remap_cartdridge = 0;
unsigned char gb_use_snarare_madmix = 0;

unsigned char gb_font_8x8 = 1;

unsigned int lasttick;
unsigned char gb_reset = 0;
unsigned char gb_id_cur_com = 0;

unsigned char tiempo_vga = 0;
unsigned int jj_ini_cpu, jj_end_cpu, jj_ini_vga, jj_end_vga;
unsigned int gb_cpu_timer_before, gb_cpu_timer_cur;
unsigned int gb_max_cpu_ticks, gb_min_cpu_ticks, gb_cur_cpu_ticks;
unsigned int gb_max_vga_ticks, gb_min_vga_ticks, gb_cur_vga_ticks;
unsigned long tickgap = 0;
unsigned char port3da = 0;

unsigned char gb_key_cur[80];
unsigned char gb_key_before[80];

// CGA palette (raw 6-bit VGA color values)
const unsigned char palettecga[16] = {
    0x00, 0xAA, 0x00, 0xAA,
    0x00, 0xAA, 0x00, 0xAA,
    0x55, 0xFF, 0x55, 0xFF,
    0x55, 0xFF, 0x55, 0xFF
};

unsigned char gbKeepAlive = 0;

#define uint8_t unsigned char

unsigned char vidmode = 5;
unsigned char gb_force_set_cga = 0;
unsigned char gb_force_load_com = 0;
unsigned char gb_force_boot = 0;

unsigned char gb_portramTiny[51];
void *gb_portTiny_write_callback[5];
void *gb_portTiny_read_callback[5];
unsigned char cf;
unsigned char hdcount = 0;
unsigned char running = 0;

// CGA video memory
unsigned char gb_video_cga[16384];

unsigned char slowsystem = 0;
unsigned short int constantw = 0, constanth = 0;
unsigned char bootdrive = 0;
unsigned char speakerenabled = 0;
unsigned char renderbenchmark = 0;
unsigned int gb_keyboard_time_before, gb_keyboard_time_cur;
unsigned int gb_ini_vga;
unsigned int gb_cur_vga;
unsigned char scrmodechange = 0;

unsigned int x, y;
unsigned short int segregs[4];

// Keymap for Cardputer keyboard (same format as PS/2 keymap)
volatile unsigned char keymap[256];
volatile unsigned char oldKeymap[256];

// Forward declarations
extern void VideoThreadPoll(void);
extern void draw(void);
extern void doscrmodechange();

// totalexec and totalframes defined in cpu.cpp and render.cpp
extern uint64_t totalexec, totalframes;
uint64_t starttick, endtick;
uint32_t speed = 0;

// ===============================================
// Load COM file from Flash array into emulated RAM
// ===============================================
void LoadCOMFlash(const unsigned char *ptr, int auxSize, int seg_load) {
    int dir_load = seg_load * 16;
    for (int i = 0; i < auxSize; i++) {
        write86((dir_load + 0x100 + i), ptr[i]);
    }
    SetRegCS(seg_load);
    SetRegDS(seg_load);
    SetRegES(seg_load);
    SetRegSS(seg_load);
    SetRegIP(0x100);
    SetRegSP(0);
    SetRegBP(0);
    SetRegSI(0);
    SetRegDI(0);
    SetCF(0);
}

// ===============================================
// Initialize emulated hardware chipset
// ===============================================
void inithardware() {
#ifdef use_lib_log_serial
    Serial.printf("Initializing emulated hardware:\n");
#endif
#ifndef use_lib_not_use_callback_port
    memset(gb_portramTiny, 0, sizeof(gb_portramTiny));
    memset(gb_portTiny_write_callback, 0, sizeof(gb_portTiny_write_callback));
    memset(gb_portTiny_read_callback, 0, sizeof(gb_portTiny_read_callback));
#endif
#ifdef use_lib_log_serial
    Serial.printf("  - Intel 8253 timer: ");
#endif
    init8253();
#ifdef use_lib_log_serial
    Serial.printf("OK\n");
    Serial.printf("  - Intel 8259 interrupt controller: ");
#endif
    init8259();
#ifdef use_lib_log_serial
    Serial.printf("OK\n");
    Serial.printf("  - Intel 8237 DMA controller: ");
#endif
    init8237();
#ifdef use_lib_log_serial
    Serial.printf("OK\n");
    Serial.printf("  - COM1 Hayes WiFi modem: (deferred, see setup)\n");
#endif
    // cardputer_modem_init() 不在这里调用：WiFi 一旦 begin 会吃掉 20-40KB 堆，
    // 必须排在 guest 页缓存（CreateRAM）之后，否则页缓存拿不到连续块，
    // guest 直接起不来（实测 modem 构建 "RAM allocation failed"）。
    // 顺带修掉「guest 复位时重复初始化 WiFi」的老问题。
    initVideoPorts();
    inittiming();
    initscreen();
}

// ===============================================
// Process pending actions (reset, load COM, etc.)
// ===============================================
void ProcesarAcciones() {
    if (gb_reset == 1) {
        gb_reset = 0;
        ClearRAM();
        memset(gb_video_cga, 0, 16384);
        memset(gb_key_cur, 1, sizeof(gb_key_cur));
        memset(gb_key_before, 1, sizeof(gb_key_before));
        running = 1;
        reset86();
        inithardware();
        return;
    }
    if (gb_force_load_com == 1) {
        gb_force_load_com = 0;
        int auxOffs = 0;
        if (gb_list_seg_load[gb_id_cur_com] == 0)
            auxOffs = 0x07C0;
        else
            auxOffs = 0x0051;
        LoadCOMFlash(gb_list_com_data[gb_id_cur_com], gb_list_com_size[gb_id_cur_com], auxOffs);
        return;
    }
}

// ===============================================
// Clear all emulated RAM
// ===============================================
void ClearRAM() {
    guest_memory_clear();
}

// ===============================================
// Allocate RAM banks for emulator
// ===============================================
bool CreateRAM() {
    return guest_memory_init();
}

// ===============================================
// Keyboard scancode translation
// ===============================================
unsigned char TraduceTecla(unsigned char aux) {
    unsigned char aRet = 0;
    switch (aux) {
        case PS2_KC_A: aRet = 30; break;
        case PS2_KC_B: aRet = 48; break;
        case PS2_KC_C: aRet = 46; break;
        case PS2_KC_D: aRet = 32; break;
        case PS2_KC_E: aRet = 18; break;
        case PS2_KC_F: aRet = 33; break;
        case PS2_KC_G: aRet = 34; break;
        case PS2_KC_H: aRet = 35; break;
        case PS2_KC_I: aRet = 23; break;
        case PS2_KC_J: aRet = 36; break;
        case PS2_KC_K: aRet = 37; break;
        case PS2_KC_L: aRet = 38; break;
        case PS2_KC_M: aRet = 50; break;
        case PS2_KC_N: aRet = 49; break;
        case PS2_KC_O: aRet = 24; break;
        case PS2_KC_P: aRet = 25; break;
        case PS2_KC_Q: aRet = 16; break;
        case PS2_KC_R: aRet = 19; break;
        case PS2_KC_S: aRet = 31; break;
        case PS2_KC_T: aRet = 20; break;
        case PS2_KC_U: aRet = 22; break;
        case PS2_KC_V: aRet = 47; break;
        case PS2_KC_W: aRet = 17; break;
        case PS2_KC_X: aRet = 45; break;
        case PS2_KC_Y: aRet = 21; break;
        case PS2_KC_Z: aRet = 44; break;
        case PS2_KC_0: aRet = 11; break;
        case PS2_KC_1: aRet = 2; break;
        case PS2_KC_2: aRet = 3; break;
        case PS2_KC_3: aRet = 4; break;
        case PS2_KC_4: aRet = 5; break;
        case PS2_KC_5: aRet = 6; break;
        case PS2_KC_6: aRet = 7; break;
        case PS2_KC_7: aRet = 8; break;
        case PS2_KC_8: aRet = 9; break;
        case PS2_KC_9: aRet = 10; break;
        case PS2_KC_ENTER: aRet = 28; break;
        case PS2_KC_SPACE: aRet = 57; break;
        case PS2_KC_BS: aRet = 14; break;
        case PS2_KC_TAB: aRet = 15; break;
        case PS2_KC_ESC: aRet = 1; break;
        case PS2_KC_L_SHIFT: aRet = 42; break;
        case PS2_KC_R_SHIFT: aRet = 54; break;
        case PS2_KC_CTRL: aRet = 29; break;
        case PS2_KC_ALT: aRet = 56; break;
        case PS2_KC_MINUS: aRet = 12; break;
        case PS2_KC_EQUAL: aRet = 13; break;
        case PS2_KC_OPEN_SQ: aRet = 26; break;
        case PS2_KC_CLOSE_SQ: aRet = 27; break;
        case PS2_KC_SEMI: aRet = 39; break;
        case PS2_KC_APOS: aRet = 40; break;
        case PS2_KC_SINGLE: aRet = 41; break;
        case PS2_KC_BACK: aRet = 43; break;
        case PS2_KC_COMMA: aRet = 51; break;
        case PS2_KC_DOT: aRet = 52; break;
        case PS2_KC_DIV: aRet = 53; break;
        case PS2_KC_DELETE: aRet = 83; break;
        case KEY_CURSOR_LEFT: aRet = 75; break;
        case KEY_CURSOR_RIGHT: aRet = 77; break;
        case KEY_CURSOR_UP: aRet = 72; break;
        case KEY_CURSOR_DOWN: aRet = 80; break;
        case PS2_KC_F1: aRet = 59; break;
        case PS2_KC_F2: aRet = 60; break;
        case PS2_KC_F3: aRet = 61; break;
        case PS2_KC_F4: aRet = 62; break;
        case PS2_KC_F5: aRet = 63; break;
        case PS2_KC_F6: aRet = 64; break;
        case PS2_KC_F7: aRet = 65; break;
        case PS2_KC_F8: aRet = 66; break;
        case PS2_KC_F9: aRet = 67; break;
        case PS2_KC_F10: aRet = 68; break;
        case PS2_KC_F11: aRet = 87; break;
        case PS2_KC_F12: aRet = 88; break;
        default: aRet = 0; break;
    }
    return aRet;
}

// ===============================================
// Send key event to emulated PC
// ===============================================
void CheckTeclaSDL(unsigned char codigo, unsigned char pulsado) {
    if (codigo == 0) return;
#ifdef use_lib_log_serial
    // Proof of what actually reached the emulated 8042 (physical keys included).
    Serial.printf("[xt] %02X %s\n", codigo, pulsado ? "up" : "down");
#endif

    if (pulsado == 0) {
        gb_portramTiny[fast_tiny_port_0x60] = codigo;
        gb_portramTiny[fast_tiny_port_0x64] |= 2;
        doirq(1);
    } else {
        gb_portramTiny[fast_tiny_port_0x60] = codigo | 0x80;
        gb_portramTiny[fast_tiny_port_0x64] |= 2;
        doirq(1);
    }
}

// ===============================================
// Translate key array index to BIOS scancode
// ===============================================
unsigned char TraduceCodigoTecla(int aux) {
    unsigned char aRet = 0;
    switch (aux) {
        case 0: aRet = 30; break;   // a
        case 1: aRet = 48; break;   // b
        case 2: aRet = 46; break;   // c
        case 3: aRet = 32; break;   // d
        case 4: aRet = 18; break;   // e
        case 5: aRet = 33; break;   // f
        case 6: aRet = 34; break;   // g
        case 7: aRet = 35; break;   // h
        case 8: aRet = 23; break;   // i
        case 9: aRet = 36; break;   // j
        case 10: aRet = 37; break;  // k
        case 11: aRet = 38; break;  // l
        case 12: aRet = 50; break;  // m
        case 13: aRet = 49; break;  // n
        case 14: aRet = 24; break;  // o
        case 15: aRet = 25; break;  // p
        case 16: aRet = 16; break;  // q
        case 17: aRet = 19; break;  // r
        case 18: aRet = 31; break;  // s
        case 19: aRet = 20; break;  // t
        case 20: aRet = 22; break;  // u
        case 21: aRet = 47; break;  // v
        case 22: aRet = 17; break;  // w
        case 23: aRet = 45; break;  // x
        case 24: aRet = 21; break;  // y
        case 25: aRet = 44; break;  // z
        case 26: aRet = 11; break;  // 0
        case 27: aRet = 2; break;   // 1
        case 28: aRet = 3; break;   // 2
        case 29: aRet = 4; break;   // 3
        case 30: aRet = 5; break;   // 4
        case 31: aRet = 6; break;   // 5
        case 32: aRet = 7; break;   // 6
        case 33: aRet = 8; break;   // 7
        case 34: aRet = 9; break;   // 8
        case 35: aRet = 10; break;  // 9
        case 36: aRet = 28; break;  // ENTER
        case 37: aRet = 57; break;  // Space
        case 38: aRet = 14; break;  // Backspace
        case 39: aRet = 1; break;   // ESC
        case 40: aRet = 75; break;  // Left
        case 41: aRet = 77; break;  // Right
        case 42: aRet = 72; break;  // Up
        case 43: aRet = 80; break;  // Down
        case 44: aRet = 59; break;  // F1
        default: aRet = 0; break;
    }
    return aRet;
}

// ===============================================
// Handle keyboard input (Cardputer → emulated PC)
// ===============================================
void handleinput() {
    static const uint8_t monitored_keys[] = {
        PS2_KC_A, PS2_KC_B, PS2_KC_C, PS2_KC_D, PS2_KC_E, PS2_KC_F,
        PS2_KC_G, PS2_KC_H, PS2_KC_I, PS2_KC_J, PS2_KC_K, PS2_KC_L,
        PS2_KC_M, PS2_KC_N, PS2_KC_O, PS2_KC_P, PS2_KC_Q, PS2_KC_R,
        PS2_KC_S, PS2_KC_T, PS2_KC_U, PS2_KC_V, PS2_KC_W, PS2_KC_X,
        PS2_KC_Y, PS2_KC_Z,
        PS2_KC_0, PS2_KC_1, PS2_KC_2, PS2_KC_3, PS2_KC_4,
        PS2_KC_5, PS2_KC_6, PS2_KC_7, PS2_KC_8, PS2_KC_9,
        PS2_KC_ENTER, PS2_KC_SPACE, PS2_KC_BS, PS2_KC_TAB, PS2_KC_ESC,
        PS2_KC_L_SHIFT, PS2_KC_CTRL, PS2_KC_ALT,
        PS2_KC_MINUS, PS2_KC_EQUAL, PS2_KC_OPEN_SQ, PS2_KC_CLOSE_SQ,
        PS2_KC_SEMI, PS2_KC_APOS, PS2_KC_SINGLE, PS2_KC_BACK,
        PS2_KC_COMMA, PS2_KC_DOT, PS2_KC_DIV, PS2_KC_DELETE,
        KEY_CURSOR_LEFT, KEY_CURSOR_RIGHT, KEY_CURSOR_UP, KEY_CURSOR_DOWN,
        PS2_KC_F1, PS2_KC_F2, PS2_KC_F3, PS2_KC_F4, PS2_KC_F5,
        PS2_KC_F6, PS2_KC_F7, PS2_KC_F8, PS2_KC_F9, PS2_KC_F10,
        PS2_KC_F11, PS2_KC_F12
    };
    const size_t key_count = sizeof(monitored_keys) / sizeof(monitored_keys[0]);

    for (size_t i = 0; i < key_count; i++) {
        gb_key_cur[i] = keymap[monitored_keys[i]];
    }

    // Only one scan code can be pushed per poll: the emulated 8042 has a single
    // data register and the guest BIOS ISR needs the whole poll interval to read
    // it. Ordering therefore matters. A modifier must reach the guest *before*
    // the key it modifies (Aa+1 has to be 0x2A then 0x02, otherwise the BIOS has
    // already decoded a plain '1'; that is what used to break !@#$%^&*()), and a
    // key release must reach it before the modifier release.
    auto key_is_modifier = [](uint8_t sc) {
        return sc == PS2_KC_L_SHIFT || sc == PS2_KC_R_SHIFT ||
               sc == PS2_KC_CTRL || sc == PS2_KC_ALT;
    };

    // Pass 1: modifier presses.
    for (size_t i = 0; i < key_count; i++) {
        if (!key_is_modifier(monitored_keys[i])) continue;
        if (gb_key_cur[i] == gb_key_before[i]) continue;
        if (gb_key_cur[i] != 0) continue;              // 0 = pressed
        unsigned char xt_code = TraduceTecla(monitored_keys[i]);
        gb_key_before[i] = gb_key_cur[i];
        if (xt_code) { CheckTeclaSDL(xt_code, gb_key_cur[i]); return; }
    }

    // Pass 2: every non-modifier change, presses and releases.
    for (size_t i = 0; i < key_count; i++) {
        if (key_is_modifier(monitored_keys[i])) continue;
        if (gb_key_cur[i] == gb_key_before[i]) continue;
        unsigned char xt_code = TraduceTecla(monitored_keys[i]);
        gb_key_before[i] = gb_key_cur[i];
        if (xt_code) { CheckTeclaSDL(xt_code, gb_key_cur[i]); return; }
    }

    // Pass 3: modifier releases.
    for (size_t i = 0; i < key_count; i++) {
        if (!key_is_modifier(monitored_keys[i])) continue;
        if (gb_key_cur[i] == gb_key_before[i]) continue;
        unsigned char xt_code = TraduceTecla(monitored_keys[i]);
        gb_key_before[i] = gb_key_cur[i];
        if (xt_code) { CheckTeclaSDL(xt_code, gb_key_cur[i]); return; }
    }
}

// ===============================================
// Main setup
// ===============================================
#ifdef use_lib_log_serial
// HWCDC has no vprintf, so format into a small buffer and push raw bytes.
// Recursion is not a concern here: the SD/FatFS logs we care about never come
// from inside the CDC driver.
static int cardputer_esp_log_vprintf(const char *format, va_list arg) {
    char buffer[192];
    va_list copy;
    va_copy(copy, arg);
    const int written = vsnprintf(buffer, sizeof(buffer), format, copy);
    va_end(copy);
    if (written > 0) {
        size_t length = strlen(buffer);
        if (length > sizeof(buffer) - 1) length = sizeof(buffer) - 1;
        Serial.write((const uint8_t *)buffer, length);
    }
    return written;
}
#endif

/* 内存预算仪表（仅调试固件）。WiFi / 内部盘 FatFS / guest 页缓存三者抢的是同一
   块 ~320 KB 内部 SRAM，出了问题只能靠逐点数字判断谁把连续块吃掉了，所以 setup
   每跨过一步就打一行。free = 总空闲，max = 最大连续块（FatFS 与 WiFi 都只认后者）。 */
#ifdef use_lib_log_serial
static void mem_log(const char *label) {
    Serial.printf("[mem] %-20s free=%lu max=%lu\n", label,
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMaxAllocHeap());
}
#else
#define mem_log(label) do { } while (0)
#endif

/* guest 页缓存是否已建好。存储/启动盘选择提前到 CreateRAM 之前以后，
   boot 菜单里的 set_boot_image(apply_now) 可能在模拟器还没内存时就要求重启 guest。 */
bool cardputer_emulator_ready = false;

void setup() {
    // 主频基线：先按最快的 240 MHz 起步。省电档（80 MHz）必须等 NVS 与显示
    // 都就绪之后才能读出来，见 cardputer_settings_init() 之后的 apply 调用 ——
    // 把 nvs_flash_init() 放在 setup() 第一行会让整机在串口/显示初始化之前挂死
    // （实测：屏幕停在上一帧、COM4 一字节都不出）。
    setCpuFrequencyMhz(240);

#ifdef use_lib_log_serial
    Serial.begin(115200);
    // Never block forever when the host is not reading: HWCDC stalls the
    // firmware once its TX buffer fills, which strands the device and leaves
    // COM4 unusable ("PermissionError(13, ...)"). A 0 ms TX timeout drops whole
    // bytes instead - and the 16-line screen dump (720 B against a 256 B ring)
    // then arrived truncated, which made the debug output hard to read. 20 ms
    // drains ~230 B per stall, so every single line (45 B) goes out intact while
    // a missing host still cannot wedge the firmware.
    Serial.setTxTimeoutMs(20);
    // 2.5 s: when the host opens the port right after a DTR reset the CDC is
    // not enumerated yet and every early printf is silently dropped. This delay
    // exists only in the -dbg/-dbglean builds.
    delay(2500);
    // Route ESP_LOG (the SD/FatFS driver reports card-init failures through it)
    // to the USB CDC console instead of UART0, which the Cardputer does not
    // expose. Debug builds only.
    esp_log_set_vprintf(cardputer_esp_log_vprintf);
    Serial.printf("\n=== CardPuter86 ===\n");
    Serial.printf("ESP32-S3 CPU: %lu MHz\n", (unsigned long)getCpuFrequencyMhz());
    Serial.printf("Free heap: %lu\n", (unsigned long)ESP.getFreeHeap());
    Serial.printf("PSRAM: %lu\n", (unsigned long)ESP.getPsramSize());
#endif

    // Initialize TFT display (draws boot log on screen)
    cardputer_display_init();
    tft_log("Display ready");

    // 顺序很重要（实测出来的）：
    //   1) WiFi 栈先起：它要一次吃掉约 62KB 动态内存；
    //   2) 页缓存再去 malloc 大块（单块 4KB×N，退让下限 12 页）；
    //   3) 最后挂内部盘 FatFS（要 >10KB 连续块，max_files×4KB）。
    // 反过来（页缓存先拿 114KB）时，WiFi 一上来就把最大连续块压到 10,228B，
    // `[flash] mount rc=0x101 (ESP_ERR_NO_MEM)` → images=0 → 没有启动盘。
    cardputer_settings_init();
    mem_log("after settings");
    // 蓝牙键盘模式（见 cardputer_btkbd.cpp）：
    //   - BLE 一次要吃约 40KB 连续堆，必须在任何模拟器/内部盘初始化之前起来，
    //     否则 `[flash] mount rc=0x101 (ESP_ERR_NO_MEM)` → images=0（实测踩过）。
    //   - 所以 BT 模式下**整个模拟器都不初始化**（不 CreateRAM、不挂内部盘、
    //     不 reset86），setup() 直接返回，loop() 只跑 BT 键盘。
    //   - 退出（按 b t n g o）时置一次性 skip 标记 + ESP.restart()，重启后
    //     走正常 DOS 启动；NVS 里的开关不动，下次上电仍进 BT。
    if (cardputer_settings_bt_kbd_enabled()) {
        bool bt_handled = false;
        if (cardputer_btkbd_take_skip_once()) {
            // 上一轮 btngo 退出后要求这次正常启动
            tft_log("BT off for this boot");
#ifdef use_lib_log_serial
            Serial.printf("bt: skipped once (exited via btngo)\n");
#endif
            bt_handled = true;
        } else if (!cardputer_btkbd_should_autostart()) {
            /* 崩溃循环保护：连续 3 次没活过 5 秒就不再自动进 BT（否则「复位→
               又进 BT→又崩」无限循环，连菜单都进不去）。计数在 RTC_NOINIT。 */
            tft_log("BT skipped (crash loop)");
#ifdef use_lib_log_serial
            Serial.printf("bt: autostart skipped by crash guard\n");
#endif
            bt_handled = true;
        } else if (cardputer_btkbd_start()) {
            tft_log("BT keyboard mode");
#ifdef use_lib_log_serial
            Serial.printf("bt: start=1 heap=%lu max=%lu (emulator skipped)\n",
                          (unsigned long)ESP.getFreeHeap(),
                          (unsigned long)ESP.getMaxAllocHeap());
#endif
            return;                 // ← 不初始化模拟器/存储，loop() 直接进 BT 分支
        } else {
            tft_log("BT keyboard init failed");
#ifdef use_lib_log_serial
            Serial.printf("bt: start=0 heap=%lu max=%lu\n",
                          (unsigned long)ESP.getFreeHeap(),
                          (unsigned long)ESP.getMaxAllocHeap());
#endif
            bt_handled = true;
        }
        (void)bt_handled;
    }
    /* 内部盘（flash 里的 FAT 镜像分区）必须**先于 WiFi**挂载。
       实测（N24）：WiFi 一旦 start + 关联，连同 lwIP/驱动缓冲共吃掉约 100 KB，
       之后 `esp_vfs_fat_*` 再也拿不到 ~16 KB 连续块 →
       `[flash] mount rc=0x101 (ESP_ERR_NO_MEM)` → images=0 → guest 没有启动盘、
       直接掉进 ROM BASIC。反过来（盘先挂、WiFi 后起）盘是稳的，WiFi 也仍能起来。
       盘是系统盘，优先级高于网络。 */
    cardputer_storage_init_and_select();
    mem_log("after storage");

    cardputer_modem_init();
#ifdef use_lib_log_serial
    Serial.printf("Modem init done, free heap: %lu max: %lu\n",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMaxAllocHeap());
#endif
    mem_log("after modem");    // 省电档：NVS 已就绪，现在读 "mcu80" 并切 240/80 MHz（在模拟器开跑之前；
    // 显示/SPI 已按 240 MHz 初始化完毕，S3 的 APB 固定 80 MHz 不受影响）。
    cardputer_settings_apply_mcu_clock();

    tft_log("Allocating paged guest RAM...");
    // Cold guest pages use the swap partition.
    if (!CreateRAM()) {
        char fail_text[48];
        snprintf(fail_text, sizeof(fail_text), "RAM failed: %.32s",
                 guest_memory_diag_text());
        tft_log(fail_text);
        tft_log("ERROR: emulator RAM allocation failed");
#ifdef use_lib_log_serial
        Serial.printf("RAM allocation failed. Free heap: %lu max: %lu\n",
                      (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)ESP.getMaxAllocHeap());
        Serial.printf("guestmem: %s\n", guest_memory_diag_text());
#endif
        // 不要死等：以前这里是 `while (true) delay(1000);`，一旦页缓存分配失败，
        // loop() 就永远不跑 → 串口调试台（0x01 + 字母）、屏幕、菜单全都没反应，
        // 只能重新刷机才救得回来（实测踩过一次：`guestmem: FAIL pages=1`）。
        // 现在改成「空转但保持调试通道活着」，每秒重试一次分配：进了菜单把
        // Guest RAM 调低、或者射频掉线腾出内存，都可能自己恢复。
        while (true) {
#ifdef use_lib_log_serial
            cardputer_kbd_serial_inject();
            {
                const int dbg_cmd = cardputer_kbd_serial_take_command();
                if (dbg_cmd) cardputer_debug_command(dbg_cmd);
                const char *dbg_line = cardputer_kbd_serial_take_line();
                if (dbg_line) cardputer_debug_line(dbg_line);
            }
#endif
            // delay() 就是 vTaskDelay，会把 CPU 让给 IDLE，不需要看门狗服务的那个
            // 周期 vTaskDelay（cardputer_service_scheduler 定义在 loop 之前，
            // setup 这里看不到它）。
            delay(100);
            if (CreateRAM()) {
                tft_log("Guest RAM recovered");
                break;
            }
        }
    }
    cardputer_emulator_ready = true;
    mem_log("after CreateRAM");
    {
        char ram_text[56];
        if (guest_memory_ram_fell_back()) {
            // 用户选了 128 KB，但开机这一刻凑不齐 32 页常驻（显示帧缓冲 + 内部盘
            // + WiFi 把 SRAM 占满了）。档位仍然生效（guest 就是 128 KB 机器），
            // 只是缺页会走 flash swap —— 明确说出来，别让人以为设置没保存。
            snprintf(ram_text, sizeof(ram_text),
                     "%lu KB RAM (paging, SRAM short)",
                     (unsigned long)guest_memory_ram_kb());
        } else {
            snprintf(ram_text, sizeof(ram_text), "%lu KB guest RAM ready",
                     (unsigned long)guest_memory_ram_kb());
        }
        tft_log(ram_text);
    }

    cardputer_cpu_init();
    cardputer_rtc_init();
    cardputer_power_init();
    mem_log("after cpu init");

    /* 存储/启动盘选择已经在 modem_init 之前做完了（见上面的长注释）：
       内部盘的 FatFS 必须趁堆还完整时挂上，否则 WiFi 一起来它就拿不到连续块。
       菜单改「Guest RAM / Boot system」仍然可用，set_boot_image(apply_now) 会在
       这时直接换盘（此时 CreateRAM 已完成，guest RAM 可用）。 */

    ClearRAM();
    SetRAMTruco();
    bootstrapPoll();
    memset(gb_key_cur, 1, sizeof(gb_key_cur));
    memset(gb_key_before, 1, sizeof(gb_key_before));
    FuerzoParityRAM();
    bootdrive = gb_disk_image.mounted ? gb_disk_image.drive : 255;
    cardputer_storage_show_boot_status();

    // Initialize keyboard
    cardputer_kbd_init();

    // Initialize I2S speaker
    cardputer_speaker_init();

    // BIOS data area: number of installed hard disks.
    hdcount = cardputer_storage_hard_drive_count();
    write86(0x475, hdcount);

    // Initialize emulator
    running = 1;
    reset86();
#ifdef use_lib_log_serial
    Serial.printf("CPU reset OK!\n");
#endif
    inithardware();

    lasttick = starttick = 0;
    gb_ini_vga = lasttick;
    gb_keyboard_time_before = gb_keyboard_time_cur = gb_cpu_timer_before = gb_cpu_timer_cur = millis();

#ifdef use_lib_log_serial
    Serial.printf("Setup complete. Free heap: %lu\n", (unsigned long)ESP.getFreeHeap());
#endif
}

// ===============================================
// Scheduler / watchdog hygiene  (N2-③)
// ===============================================
// Why a bare yield() is not enough on this core:
//   arduino-esp32 2.0.x defines yield() as vPortYield(), i.e. it only defers to
//   tasks of EQUAL OR HIGHER priority. loopTask runs at priority 1 and the IDLE
//   task at priority 0, so a loop() that never blocks starves IDLE forever.
//   The task watchdog (TG0 on ESP32-S3, CONFIG_ESP_TASK_WDT_TIMEOUT_S = 5) is
//   fed by the IDLE task only, and arduino-esp32 leaves loopTaskWDTEnabled =
//   false (cores/esp32/main.cpp), so nothing resets it either. delayMicroseconds()
//   is a pure esp_timer busy-wait and does not help. Net effect: a long-running
//   guest workload ends in rst:0x7 (TG0WDT_SYS_RST).
// The only thing that lets IDLE run is a real block on a tick, hence vTaskDelay(1).
// 100 ms period keeps the watchdog margin at ~50x while costing ~1% throughput.
static const uint32_t CARDPUTER_IDLE_YIELD_PERIOD_US = 100000;
static uint32_t cardputer_last_idle_yield_us = 0;

static inline void cardputer_service_scheduler(void) {
    const uint32_t now = (uint32_t)esp_timer_get_time();
    // Unsigned difference keeps this correct across the ~71 min wrap.
    if ((uint32_t)(now - cardputer_last_idle_yield_us) >=
        CARDPUTER_IDLE_YIELD_PERIOD_US) {
        cardputer_last_idle_yield_us = now;
        vTaskDelay(1);
    }
}

// ===============================================
// Main loop
// ===============================================
unsigned char gb_cpunoexe = 0;
unsigned int gb_cpunoexe_timer_ini;
unsigned int tiempo_ini_cpu, tiempo_fin_cpu;
unsigned int total_tiempo_ms_cpu;
unsigned int tiene_que_tardar = 0;

// 重启 guest（不重启 ESP32）。串口调试命令 'r' 与「设置菜单换启动镜像」
// 共用这一段：清空 guest RAM、重建 BIOS 数据区、按当前挂载的镜像设 bootdrive，
// 再 reset86() 让 BIOS 重新 POST 并从 A:/C: 引导。
// 注意：必须放在 #ifdef use_lib_log_serial 之外 —— lean 固件里设置菜单也会调它。
void cardputer_guest_restart(void) {
    // 存储/启动盘选择现在发生在 CreateRAM 之前，那次 boot 菜单若带 apply_now 会
    // 走到这里 —— 此时 guest 页缓存还没建，重启没有意义（后面 setup 自己会 reset86）。
    if (!cardputer_emulator_ready) return;
    ClearRAM();
    SetRAMTruco();
    bootstrapPoll();
    FuerzoParityRAM();
    bootdrive = gb_disk_image.mounted ? gb_disk_image.drive : 255;
    reset86();
}

#ifdef use_lib_log_serial
/* Debug console driven from the USB CDC line (see cardputer_kbd_serial_inject):
   send 0x01 then one of these bytes.
     r  reboot the guest (re-run POST + boot from the mounted image)
     s  print the storage diagnostics
     b  mount the SD floppy image as B: now
     n  close B: again
     x  drop B: and unmount the SD card entirely
     d  dump the SD MBR partition table
     z  wipe the first 8 SD sectors, then dump
     k  dump guest CS:IP/SS:SP, INT histogram, 768-instruction ring, INT trail */
/* Raw line from the debug console (armed with 0x01 'Q'). Currently the only
   payload is "<ssid>,<password>": it enables Network COM1 and stores the
   credentials in NVS. A reboot is still required — the radio has to come up in
   modem_init(), i.e. before CreateRAM() allocates the guest page cache. */
static void cardputer_debug_line(const char *line) {
    const char *comma = strchr(line, ',');
    if (!comma) {
        Serial.printf("[dbg] need <ssid>,<pass> (got %u chars)\n",
                      (unsigned)strlen(line));
        return;
    }
    const size_t ssid_len = (size_t)(comma - line);
    char ssid[33];
    char pass[65];
    if (ssid_len == 0 || ssid_len >= sizeof(ssid) ||
        strlen(comma + 1) >= sizeof(pass)) {
        Serial.printf("[dbg] bad ssid/pass length\n");
        return;
    }
    memcpy(ssid, line, ssid_len);
    ssid[ssid_len] = '\0';
    strlcpy(pass, comma + 1, sizeof(pass));
    cardputer_settings_set_com1_enabled(true);
    const bool ok = cardputer_modem_save_wifi(ssid, pass);
    Serial.printf("[dbg] wifi saved=%d ssid='%s' passlen=%u com1=%d\n",
                  (int)ok, ssid, (unsigned)strlen(pass),
                  (int)cardputer_settings_com1_enabled());
    Serial.printf("[dbg] reboot the device so the radio starts before the page cache\n");
}

static void cardputer_debug_command(int cmd) {
    switch (cmd) {
        case 'Q':
            // 原文行模式：接下来收到的字节不再翻成 PS/2 扫描码，而是原样收进
            // 一行（到 CR 结束），交给 cardputer_debug_line() 解析。用它写 WiFi
            // 凭据，避免密码里的 shift 字符被注入路径弄丢。
            cardputer_kbd_serial_begin_line();
            Serial.printf("[dbg] line mode: send <ssid>,<pass> then CR\n");
            break;
        case 'G':
            cardputer_modem_debug_net_report();
            break;
        case 'R':
            // Reboot the whole MCU from the firmware itself. Lets a single serial
            // session provision WiFi and then reboot into it, instead of handing
            // the port back to esptool (every extra port open risks wedging COM4).
            Serial.printf("[dbg] restarting MCU now\n");
            Serial.flush();
            delay(120);
            ESP.restart();
            break;
        case 'H':
            cardputer_modem_debug_http_get("example.com", "/");
            break;
        case 'W':
            // 远程验收 WiFi：不进菜单就能开/关 Network COM1（WiFi 只在 COM1
            // 使能且 SSID 非空时才 start_wifi）。开后强制起射频，方便在 guest
            // 里跑 WTERM + AT+WIFISCAN 验证。
            {
                const bool enable = !cardputer_settings_com1_enabled();
                if (!cardputer_settings_set_com1_enabled(enable)) {
                    Serial.printf("[dbg] COM1 toggle failed (NVS)\n");
                } else {
                    Serial.printf("[dbg] COM1 -> %s heap=%lu max=%lu\n",
                                  enable ? "Enabled" : "Disabled",
                                  (unsigned long)ESP.getFreeHeap(),
                                  (unsigned long)ESP.getMaxAllocHeap());
                    if (enable) {
                        cardputer_modem_debug_start_radio();
                        cardputer_modem_resume_wifi();
                        Serial.printf("[dbg] wifi configured=%d connected=%d\n",
                                      (int)cardputer_modem_wifi_configured(),
                                      (int)cardputer_modem_wifi_connected());
                    }
                }
            }
            break;
        case 'r':
            Serial.printf("[dbg] guest reboot, bootdrive=%u\n", (unsigned)bootdrive);
            cardputer_guest_restart();
            break;
        case 's':
            Serial.printf("[dbg] %s\n", cardputer_storage_sd_diag_text());
            break;
        case 'b':
            cardputer_storage_debug_set_second_drive(true);
            Serial.printf("[dbg] B: mount -> %s\n", cardputer_storage_sd_diag_text());
            break;
        case 'n':
            cardputer_storage_debug_set_second_drive(false);
            Serial.printf("[dbg] B: closed -> %s\n", cardputer_storage_sd_diag_text());
            break;
        case 'x':
            cardputer_storage_debug_drop_sd();
            Serial.printf("[dbg] SD dropped -> %s\n", cardputer_storage_sd_diag_text());
            break;
        case 'z':
            // Fresh start for the external hard disk: blank the MBR so the
            // guest's FDISK sees an empty disk and can use the whole 504 MB.
            Serial.printf("[dbg] wiping SD MBR...\n");
            cardputer_storage_raw_wipe(8);
            cardputer_storage_raw_dump_mbr();
            break;
        case 'd':
            cardputer_storage_raw_dump_mbr();
            break;
        case 'p':
            // Card read-path benchmark + counters: is `dir c:` slow because of
            // the SD card or because of the emulated guest?
            cardputer_storage_debug_bench_sd();
            break;
        case 'i':
            // Instruction counter + millis. Sample it right before and right
            // after a guest command to get the exact instruction count and the
            // effective MIPS of that command (g_cdp_instrs counts every 16th
            // executed instruction, see the sampler in exec86()).
            Serial.printf("INSTR %lu ms %lu\n", (unsigned long)g_cdp_instrs,
                          (unsigned long)millis());
            break;
        case 'M':
            // Cycle guest RAM 128 -> 256 -> 512 -> 128 (same switch the settings
            // menu drives). Follow with 'r' so the guest boots on the new size.
            {
                const uint32_t now_kb = guest_memory_ram_kb();
                const uint32_t next_kb = now_kb == 128 ? 256 : (now_kb == 256 ? 512 : 128);
                if (!guest_memory_set_ram_kb(next_kb)) {
                    Serial.printf("[dbg] guest RAM %lu KB FAILED (heap=%lu max=%lu)"
                                  " - see guestmem diag\n",
                                  (unsigned long)next_kb,
                                  (unsigned long)ESP.getFreeHeap(),
                                  (unsigned long)ESP.getMaxAllocHeap());
                } else {
                    Serial.printf("[dbg] guest RAM -> %lu KB pages=%u paging=%d"
                                  " (send 'r' to reboot guest)\n",
                                  (unsigned long)guest_memory_ram_kb(),
                                  (unsigned)guest_memory_cache_pages(),
                                  (int)guest_memory_512k_enabled());
                }
            }
            break;
        case 'C':
            // Toggle the MCU clock 240 <-> 80 MHz (the "Processor" menu item).
            {
                const bool eco = !cardputer_settings_mcu_80mhz_enabled();
                cardputer_settings_set_mcu_80mhz_enabled(eco);
                Serial.printf("[dbg] processor -> %lu MHz\n",
                              (unsigned long)getCpuFrequencyMhz());
            }
            break;
        case 'B':
            // Toggle the NVS "btkbd" flag (BT keyboard mode). Takes effect on the
            // NEXT boot, because BLE must be started before the guest page cache
            // (CreateRAM) grabs its 68KB reserve.
            {
                const bool enable = !cardputer_settings_bt_kbd_enabled();
                cardputer_settings_set_bt_kbd_enabled(enable);
                Serial.printf("[dbg] btkbd=%d (reboot to apply)\n", enable ? 1 : 0);
            }
            break;
        case 'T':
            // Try to start / stop the BLE HID keyboard right now (runtime path used
            // by the settings menu item). Prints heap so we can see whether BLE fit.
            if (cardputer_btkbd_active()) {
                cardputer_btkbd_stop();
                Serial.printf("[dbg] bt stopped, heap=%lu\n",
                              (unsigned long)ESP.getFreeHeap());
            } else {
                const bool ok = cardputer_btkbd_start();
                Serial.printf("[dbg] bt start=%d heap=%lu max=%lu\n", ok ? 1 : 0,
                              (unsigned long)ESP.getFreeHeap(),
                              (unsigned long)ESP.getMaxAllocHeap());
            }
            break;
        case 'm':
            // Retry the SD hard-disk handshake. If the card missed its boot-time
            // CMD0/ACMD41 (rc=0x108 ESP_ERR_INVALID_RESPONSE) the guest booted
            // without a C: drive; this mounts it now, then 'r' reboots the guest
            // so POST sees the drive.
            Serial.printf("[dbg] SD retry -> %s (reboot guest with 'r' if it worked)\n",
                          cardputer_storage_debug_retry_sd() ? "mounted" : "still failing");
            cardputer_storage_raw_dump_mbr();
            break;
        case 'k':
            // Full guest snapshot on demand. Used to answer "where is the guest
            // stuck?" for a hung DOS program without a second firmware build:
            //   K1  -- CS:IP, SS:SP, BIOS keyboard head/tail, video state
            //   K2  -- INT histogram (intcall86 counters, nonzero only)
            //   KR  -- the 48-entry CS:IP ring (one sample every 16 instructions,
            //          so this is the last ~768 instructions, oldest first)
            //   KT  -- the 20-entry interrupt trail (intnum@caller CS:IP)
            {
                unsigned k_cs = GetRegCS(), k_ip = GetRegIP();
                unsigned long k_lin = ((unsigned long)k_cs << 4) + k_ip;
                Serial.printf("K1 %04X:%04X lin=%05lX ss %04X:%04X kb %04X/%04X fl %02X vidmode=%u mode37=%02X\n",
                    k_cs, k_ip, k_lin,
                    (unsigned)GetRegSS(), (unsigned)GetRegSP(),
                    (unsigned)(read86(0x41A) | (read86(0x41B) << 8)),
                    (unsigned)(read86(0x41C) | (read86(0x41D) << 8)),
                    (unsigned)read86(0x417),
                    (unsigned)vidmode,
                    (unsigned)gb_portramTiny[fast_tiny_port_0x3D8]);
                Serial.printf("K1c tk %lu ms %lu\n",
                    (unsigned long)((unsigned long)read86(0x46C) |
                                    ((unsigned long)read86(0x46D) << 8) |
                                    ((unsigned long)read86(0x46E) << 16) |
                                    ((unsigned long)read86(0x46F) << 24)),
                    (unsigned long)millis());
                Serial.printf("K1b ops %02X %02X %02X %02X %02X %02X %02X %02X ds %04X\n",
                    (unsigned)read86(k_lin), (unsigned)read86(k_lin + 1),
                    (unsigned)read86(k_lin + 2), (unsigned)read86(k_lin + 3),
                    (unsigned)read86(k_lin + 4), (unsigned)read86(k_lin + 5),
                    (unsigned)read86(k_lin + 6), (unsigned)read86(k_lin + 7),
                    (unsigned)GetRegDS());
                Serial.printf("K2 int hist:");
                {
                    uint8_t k_shown = 0;
                    for (unsigned k_n = 0; k_n < 256 && k_shown < 20; k_n++) {
                        if (g_cdp_int_hist[k_n]) {
                            Serial.printf(" %02lX:%lu", (unsigned long)k_n, g_cdp_int_hist[k_n]);
                            k_shown++;
                        }
                    }
                }
                Serial.printf("\n");
                {
                    uint8_t k_h = g_cdp_ring_head;
                    for (uint8_t k_i = 0; k_i < CDP_RING_LEN; k_i++) {
                        uint8_t k_idx = (uint8_t)((k_h + k_i) % CDP_RING_LEN);
                        if ((k_i % 12) == 0) Serial.printf("KR%c", k_i ? ' ' : ':');
                        Serial.printf(" %04X:%04X",
                            (unsigned)g_cdp_ring_cs[k_idx], (unsigned)g_cdp_ring_ip[k_idx]);
                        if ((k_i % 12) == 11) Serial.printf("\n");
                    }
                }
                {
                    uint8_t k_h = g_cdp_trail_head;
                    Serial.printf("KT:");
                    for (uint8_t k_i = 0; k_i < CDP_TRAIL_LEN; k_i++) {
                        uint8_t k_idx = (uint8_t)((k_h + k_i) % CDP_TRAIL_LEN);
                        Serial.printf(" %u@%04X:%04X",
                            (unsigned)g_cdp_trail_num[k_idx],
                            (unsigned)g_cdp_trail_cs[k_idx],
                            (unsigned)g_cdp_trail_ip[k_idx]);
                    }
                    Serial.printf("\n");
                }
            }
            break;
        case 'w':
            // WiFi / COM1 modem status. Also proves whether this build even
            // carries the modem (lean defines CARDPUTER86_NO_MODEM and the
            // cardputer_modem.h stubs answer "off" for everything).
            Serial.printf("[dbg] wifi connected=%d configured=%d ssid='%s' com1=%d\n",
                          cardputer_modem_wifi_connected() ? 1 : 0,
                          cardputer_modem_wifi_configured() ? 1 : 0,
                          cardputer_modem_wifi_ssid(),
                          cardputer_settings_com1_enabled() ? 1 : 0);
            break;
        default:
            Serial.printf("[dbg] unknown command 0x%02X\n", (unsigned)cmd);
            break;
    }
}
#endif

void loop() {
    // 蓝牙键盘模式：guest 冻结（不跑 exec86、键盘不进 PS/2 控制器），
    // 所有按键转成 BLE HID 报文发给主机；只有依次按 b-t-n-g-o 才退回模拟器
    // （那 5 个字母被拦下，不会发给主机）。cardputer_service_scheduler() 不能漏：
    // 它负责每 100ms 让出一次 CPU，否则 TG0 任务看门狗会把整机复位。
    if (cardputer_btkbd_active()) {
        cardputer_btkbd_poll();
        cardputer_service_scheduler();
        delay(2);
        return;
    }
#ifdef use_lib_log_serial
    cardputer_serial_screen_dump();
    cardputer_kbd_serial_inject();
    {
        const int dbg_cmd = cardputer_kbd_serial_take_command();
        if (dbg_cmd) cardputer_debug_command(dbg_cmd);
        const char *dbg_line = cardputer_kbd_serial_take_line();
        if (dbg_line) cardputer_debug_line(dbg_line);
    }
#endif
    jj_ini_cpu = micros();
    static const uint32_t cpu_batch_instructions = 2000;
    exec86(cpu_batch_instructions);
    cardputer_modem_poll();
    jj_end_cpu = micros();
    gb_cur_cpu_ticks = (jj_end_cpu - jj_ini_cpu);
    total_tiempo_ms_cpu = gb_cur_cpu_ticks / 1000;
    if (gb_cur_cpu_ticks > gb_max_cpu_ticks)
        gb_max_cpu_ticks = gb_cur_cpu_ticks;
    if (gb_cur_cpu_ticks < gb_min_cpu_ticks)
        gb_min_cpu_ticks = gb_cur_cpu_ticks;
    cardputer_cpu_throttle(cpu_batch_instructions);

    // Scan keyboard on schedule
    gb_keyboard_time_cur = millis();
    if ((gb_keyboard_time_cur - gb_keyboard_time_before) > gb_keyboard_poll_milis) {
        gb_keyboard_time_before = gb_keyboard_time_cur;

        // Scan Cardputer keyboard matrix
        cardputer_update();
        if (cardputer_input_any_pressed()) {
            cardputer_power_note_activity();
        }
        if (cardputer_input_consume(CARDPUTER_VK_AUTO)) {
            cardputer_storage_show_settings_menu(false);
            gb_keyboard_time_before = millis();
            gb_ini_vga = millis();
            return;
        }
        cardputer_display_update_mode_button();
        cardputer_kbd_fill_keymap();
#ifdef use_lib_log_serial
        cardputer_kbd_serial_apply();
#endif
        cardputer_power_poll();

        handleinput();
        ProcesarAcciones();
        do_tinyOSD();
    }

    if (scrmodechange) {
        doscrmodechange();
    }

    // Video rendering on schedule
    gb_cur_vga = millis();
    if ((gb_cur_vga - gb_ini_vga) >= gb_vga_poll_milis) {
        draw();
        gb_ini_vga = gb_cur_vga;
    }

    // Periodic debug output
    gb_cpu_timer_cur = millis();
    // Debug builds: this block used to fire every second, which (together with
    // setTxTimeoutMs(0) dropping bytes when the host stalls) drowned the SDK's
    // own ESP_LOG error messages about SD/FatFS. Every 5 s keeps the signal.
    if ((gb_cpu_timer_cur - gb_cpu_timer_before) > 5000) {
        gb_cpu_timer_before = gb_cpu_timer_cur;
        if (tiempo_vga == 1) {
#ifdef use_lib_log_serial
            Serial.printf("c:%u m:%u mx:%u vc:%u m:%u mx:%u\n",
                gb_cur_cpu_ticks, gb_min_cpu_ticks, gb_max_cpu_ticks,
                gb_cur_vga_ticks, gb_min_vga_ticks, gb_max_vga_ticks);
#endif
            gb_min_vga_ticks = 1000000;
            gb_max_vga_ticks = 0;
            gb_cur_vga_ticks = 0;
            tiempo_vga = 0;
        } else {
#ifdef use_lib_log_serial
            Serial.printf("MEM ram=%luKB paging=%d pages=%u free=%u mcu=%luMHz\n",
                (unsigned long)guest_memory_ram_kb(),
                guest_memory_512k_enabled() ? 1 : 0,
                (unsigned)guest_memory_cache_pages(),
                (unsigned)ESP.getFreeHeap(),
                (unsigned long)getCpuFrequencyMhz());
            Serial.printf("c:%u m:%u mx:%u\n",
                gb_cur_cpu_ticks, gb_min_cpu_ticks, gb_max_cpu_ticks);
            {
                // CDP86 诊断：512K 模式的吞吐上限完全由 flash 换页决定。
                //   evict/avg  = 淘汰次数 / 每次「擦 4KB + 写 4KB」的平均微秒
                //   reload/avg = 从 swap 读回次数 / 平均微秒
                //   i19        = guest 走了多少次 INT 19h（自重启），1 秒涨几次就是
                //                启动循环；i19 不涨说明卡死而不是复位。
                uint32_t sw_ops = 0, sw_us = 0, sw_ld = 0, sw_ldus = 0;
                guest_memory_swap_stats(&sw_ops, &sw_us, &sw_ld, &sw_ldus);
                Serial.printf("SW evict=%lu avg=%luus reload=%lu avg=%luus i19=%lu\n",
                    (unsigned long)sw_ops,
                    (unsigned long)(sw_ops ? sw_us / sw_ops : 0),
                    (unsigned long)sw_ld,
                    (unsigned long)(sw_ld ? sw_ldus / sw_ld : 0),
                    g_cdp_int19_count);
            }
            {
                // CDP86 诊断：BDA 0x413/0x414 = DOS 通过 INT 12h 看到的内存 KB 数。
                // 它决定 IO.SYS 把自身重定位到哪个地址；bootstrapPoll() 每 54ms
                // 会按 gb_ram_truco_* 重写这两个字节，所以必须实测而不是读代码猜。
                //   boot= = cyl0/head0/sect1 读取次数 = 机器被引导的次数（重启计数）
                unsigned g_mem = read86(0x413) | (read86(0x414) << 8);
                Serial.printf("BDA mem=%uKB dsk req=%lu boot=%lu last=%u/%u/%u n%u\n",
                    g_mem, g_cdp_disk_reqs, g_cdp_disk_bootsec,
                    (unsigned)g_cdp_disk_last_cyl,
                    (unsigned)g_cdp_disk_last_head,
                    (unsigned)g_cdp_disk_last_sect,
                    (unsigned)g_cdp_disk_last_cnt);
            }
            {
                // SD 读路径计数：dir c: 慢到底该怪卡还是怪模拟器，看这两行。
                //   ops/card_us = 真正下到卡的 transaction 数与耗时（含 CMD 开销）
                //   reads/hits  = guest 申请读的扇区数 / 命中 16 扇区预读窗口数
                // hits 占比高说明预读生效；card_us 很小而 dir 仍慢 → 慢在 guest。
                uint32_t sd_ops = 0, sd_us = 0, sd_reads = 0, sd_hits = 0;
                cardputer_storage_sd_io_stats(&sd_ops, &sd_us, &sd_reads, &sd_hits);
                Serial.printf("SDIO ops=%lu card_us=%lu avg=%luus reads=%lu hits=%lu\n",
                    (unsigned long)sd_ops, (unsigned long)sd_us,
                    (unsigned long)(sd_ops ? sd_us / sd_ops : 0),
                    (unsigned long)sd_reads, (unsigned long)sd_hits);
            }
            {
                // 指令吞吐：g_cdp_instrs 每 16 条指令 +1，所以真实指令数 = ×16。
                // 和 c:（一批 2000 条的微秒数）互相校验，也用来算某条命令到底
                // 跑了多少条指令（dir c: 的 FAT 扫描是最典型的例子）。
                Serial.printf("INS n=%lu ms=%lu\n",
                    (unsigned long)g_cdp_instrs, (unsigned long)millis());
            }
            {
                // WiFi/COM1 modem 状态：up = 已连上 AP；cfg = NVS 里有凭据；
                // SSID 与本地 IP 一起打出来，省得再敲 'w' 命令。
                Serial.printf("NET up=%d cfg=%d ssid='%s'\n",
                    cardputer_modem_wifi_connected() ? 1 : 0,
                    cardputer_modem_wifi_configured() ? 1 : 0,
                    cardputer_modem_wifi_ssid());
            }
            {
                // CDP86 诊断：intcall86 实际收到的中断直方图（只打非零项，最多 12 个）。
                // 判定「磁盘中断到底有没有到达模拟器」靠这一行：如果 13 从不出现，
                // 说明这盘是由 ROM 里另一条路径读的，diskhandler 的计数器就没意义。
                Serial.printf("INT hist:");
                uint8_t cdp_shown = 0;
                for (unsigned cdp_k = 0; cdp_k < 256 && cdp_shown < 12; cdp_k++) {
                    if (g_cdp_int_hist[cdp_k]) {
                        Serial.printf(" %02lX:%lu", (unsigned long)cdp_k,
                                      g_cdp_int_hist[cdp_k]);
                        cdp_shown++;
                    }
                }
                Serial.printf("\n");
            }
            if (g_cdp_ring_frozen) {
                static unsigned long cdp_reset_shown = 0;
                static uint8_t cdp_reset_prints = 0;
                if (g_cdp_reset_count != cdp_reset_shown && cdp_reset_prints < 6) {
                    cdp_reset_shown = g_cdp_reset_count;
                    cdp_reset_prints++;
                    Serial.printf("RESET#%lu at %04X:%04X ss %04X:%04X ring:",
                        g_cdp_reset_count,
                        (unsigned)g_cdp_frz_cs, (unsigned)g_cdp_frz_ip,
                        (unsigned)g_cdp_frz_ss, (unsigned)g_cdp_frz_sp);
                    for (uint8_t k = 0; k < CDP_RING_LEN; k++) {
                        Serial.printf(" %04X:%04X",
                            (unsigned)g_cdp_ring_snap_cs[k],
                            (unsigned)g_cdp_ring_snap_ip[k]);
                    }
                    Serial.printf("\n");
                }
            }
#endif
        }
#ifdef use_lib_log_serial
        // Guest-visible diagnostics (debugging tap, per second):
        //   guest CS:IP  -- where the emulated 8086 is. A value in F000:xxxx
        //                   means the guest is inside the BIOS ROM, which is
        //                   what a program blocked in INT 16h AH=0 looks like.
        //   kb head/tail -- BIOS keyboard buffer pointers at 0040:001A/001C.
        //                   key_hit() in the guest compares exactly these two,
        //                   so head != tail with a frozen F000 CS:IP proves the
        //                   "thinks a key is pending, blocks forever in ROM"
        //                   failure mode.
        //   ss:sp / op[]  -- the stack pointer and the actual instruction bytes
        //                   sitting at CS:IP. If op[0] == 0xCD && op[1] == 0x16
        //                   (int 16h) the guest is waiting for a keystroke; if
        //                   the bytes are stack garbage (e.g. 0x2A 0x2E) the
        //                   control flow was driven into the stack area instead.
        {
            unsigned g_cs = GetRegCS(), g_ip = GetRegIP();
            unsigned long g_lin = ((unsigned long)g_cs << 4) + g_ip;
            Serial.printf("sd %s\n", cardputer_storage_sd_diag_text());
            Serial.printf("guest %04X:%04X kb %04X/%04X ss %04X:%04X op %02X %02X %02X %02X\n",
                g_cs, g_ip,
                (unsigned)(read86(0x41A) | (read86(0x41B) << 8)),
                (unsigned)(read86(0x41C) | (read86(0x41D) << 8)),
                (unsigned)GetRegSS(), (unsigned)GetRegSP(),
                (unsigned)read86(g_lin), (unsigned)read86(g_lin + 1),
                (unsigned)read86(g_lin + 2), (unsigned)read86(g_lin + 3));
        }
        // CDP86: INT 轨迹（cpu.cpp 的 intcall86 每次记一条 intnum@CS:IP）。
        //   冻结版：guest 走过 INT 19h 自重启，快照保留重启前最后 20 个 INT ——
        //           这就是「DOS 5.0 到底死在哪」的直接答案。
        //   实时版：程序卡死（如 JMP $）没有 INT 19h，则每秒打一次当前环。
        {
            static uint8_t trail_prints = 0;
            if (g_cdp_snap_valid && trail_prints == 0) {
                trail_prints = 100;      // 冻结快照只打一次
                Serial.printf("trail FROZEN i19=%lu ss %04X:%04X :",
                    g_cdp_int19_count,
                    (unsigned)GetRegSS(), (unsigned)GetRegSP());
                for (uint8_t k = 0; k < CDP_TRAIL_LEN; k++) {
                    Serial.printf(" %u@%04X:%04X",
                        (unsigned)g_cdp_snap_num[k],
                        (unsigned)g_cdp_snap_cs[k],
                        (unsigned)g_cdp_snap_ip[k]);
                }
                Serial.printf("\n");
            } else if (!g_cdp_snap_valid && trail_prints < 10) {
                trail_prints++;
                uint8_t h = g_cdp_trail_head;
                Serial.printf("trail live :");
                for (uint8_t k = 0; k < CDP_TRAIL_LEN; k++) {
                    uint8_t idx = (uint8_t)((h + k) % CDP_TRAIL_LEN);
                    Serial.printf(" %u@%04X:%04X",
                        (unsigned)g_cdp_trail_num[idx],
                        (unsigned)g_cdp_trail_cs[idx],
                        (unsigned)g_cdp_trail_ip[idx]);
                }
                Serial.printf("\n");
            }
        }
#endif
        gb_min_cpu_ticks = 1000000;
        gb_max_cpu_ticks = 0;
        gb_cur_cpu_ticks = 0;
    }

    // Let the IDLE task run so the task watchdog is fed (N2-③).
    cardputer_service_scheduler();
}
