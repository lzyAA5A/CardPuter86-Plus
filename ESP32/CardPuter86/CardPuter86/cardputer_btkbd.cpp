/* 蓝牙键盘模式（BLE HID keyboard）
 * =====================================================================
 * 用户需求：「做一个蓝牙键盘模式，在 fn+空格菜单中开启，下次启动仍然保留，
 *            如果进入要退出就要按下 btngo 了」
 *
 * 行为：
 *   - 设置菜单打开后写 NVS("cardputer86"/"btkbd")，下次启动直接进 BT 模式。
 *   - 进入 BT 模式后 guest（DOS 模拟器）被冻结：loop() 不再跑 exec86，
 *     键盘也不喂给 PS/2 控制器，所有按键转成 BLE HID 报文发给配对主机。
 *   - 唯一退出方式：在本机键盘上依次按 b - t - n - g - o（间隔 <900ms）。
 *     这 5 个字母被"压住"再决定：序列成立就吞掉不发给主机；一旦走岔就原样
 *     补发出去，所以正常打字（哪怕出现单独的 b）不会真的丢键。
 *
 * 内存：BLE 协议栈要一次吃下约 40KB 动态内存，因此必须在 guest 页缓存
 * （CreateRAM）之前启动 —— 和 WiFi 当年一模一样的坑（见 NEXT-STEPS N15）。
 * 在设置菜单里运行期开启时堆已经碎片化，可能失败；失败会提示重启。
 *
 * BLE HID 细节：用 arduino-esp32 自带的 BLEHIDDevice（HID over GATT，
 * HID Service 0x1812）。它的 inputReport() 把特征值权限设成"需加密"，
 * 所以这里配了 Just Works 绑定（ESP_LE_AUTH_BOND + IO_CAP_NONE），
 * 主机会在首次连接时自动配对。
 */
#include "cardputer_btkbd.h"
#include "cardputer_input.h"

#include <M5Cardputer.h>
#include <esp_attr.h>
#include <sdkconfig.h>

#if defined(CONFIG_BLUEDROID_ENABLED)
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#define BTKBD_AVAILABLE 1
#else
#define BTKBD_AVAILABLE 0
#endif

#if BTKBD_AVAILABLE

/* ---- HID 常量（HID Usage Tables, Keyboard/Keypad page 0x07）---------- */
#define BTKBD_MOD_CTRL  0x01
#define BTKBD_MOD_SHIFT 0x02
#define BTKBD_MOD_ALT   0x04

#define BTKBD_KEY_ENTER     0x28
#define BTKBD_KEY_ESC       0x29
#define BTKBD_KEY_BACKSPACE 0x2A
#define BTKBD_KEY_TAB       0x2B
#define BTKBD_KEY_SPACE     0x2C
#define BTKBD_KEY_HOME      0x4A
#define BTKBD_KEY_DELETE    0x4C
#define BTKBD_KEY_RIGHT     0x4F
#define BTKBD_KEY_LEFT      0x50
#define BTKBD_KEY_DOWN      0x51
#define BTKBD_KEY_UP        0x52
#define BTKBD_KEY_F1        0x3A   /* F1..F12 连续 12 个 */

/* 标准键盘 Report Map：第 1 字节修饰键、第 2 字节保留、后 6 字节是按键数组。
   报文长度固定 8 字节，走 Report ID 1。 */
static const uint8_t kbd_report_map[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop)  */
    0x09, 0x06,        /* Usage (Keyboard)              */
    0xA1, 0x01,        /* Collection (Application)      */
    0x85, 0x01,        /*   Report ID (1)               */
    0x05, 0x07,        /*   Usage Page (Keypad)         */
    0x19, 0xE0,        /*   Usage Minimum (LCtrl)       */
    0x29, 0xE7,        /*   Usage Maximum (RGUI)        */
    0x15, 0x00,        /*   Logical Minimum (0)         */
    0x25, 0x01,        /*   Logical Maximum (1)         */
    0x75, 0x01,        /*   Report Size (1)             */
    0x95, 0x08,        /*   Report Count (8)            */
    0x81, 0x02,        /*   Input (Data,Var,Abs) 修饰键 */
    0x95, 0x01,        /*   Report Count (1)            */
    0x75, 0x08,        /*   Report Size (8)             */
    0x81, 0x01,        /*   Input (Const) 保留字节      */
    0x95, 0x06,        /*   Report Count (6)            */
    0x75, 0x08,        /*   Report Size (8)             */
    0x15, 0x00,        /*   Logical Minimum (0)         */
    0x25, 0x65,        /*   Logical Maximum (101)       */
    0x05, 0x07,        /*   Usage Page (Keypad)         */
    0x19, 0x00,        /*   Usage Minimum (0)           */
    0x29, 0x65,        /*   Usage Maximum (101)         */
    0x81, 0x00,        /*   Input (Data,Array) 6 个按键 */
    0xC0               /* End Collection                */
};

static bool bt_active = false;
static volatile bool bt_connected = false;
static BLECharacteristic *bt_input = nullptr;
static BLEHIDDevice *bt_hid = nullptr;

/* 退出方式：按机器后面那颗 btnGO（= GPIO0 / BtnA）。
   原本还留过「依次按 b t n g o」的键序列，已删除 —— 在宿主里打这五个字母
   会把 BT 模式退掉，太容易误触。 */

/* 上一次的键状态，用来做「按下沿」判断（M5 的 keysState 是电平式的） */
static char prev_chars[12];
static uint8_t prev_char_count = 0;
static bool prev_enter = false;
static bool prev_space = false;
static bool prev_tab = false;
static bool prev_del = false;
static uint8_t prev_fn_index = 0xFF;

static uint32_t status_drawn_ms = 0;
static bool status_drawn_connected = false;
static uint32_t bt_alive_since_ms = 0;   /* BT 循环存活计时（崩溃计数清零用） */
/* BLE 回调只置这两个标志，主循环（poll）负责重画和报电量 */
static volatile bool bt_status_dirty = false;
static volatile bool bt_battery_pending = false;

/* ---- 字符 -> (HID usage, 修饰键) ------------------------------------- */
static uint8_t bt_hid_for_char(char c, uint8_t *mods_out) {
    *mods_out = 0;
    if (c >= 'a' && c <= 'z') return (uint8_t)(0x04 + (c - 'a'));
    if (c >= 'A' && c <= 'Z') {
        *mods_out = BTKBD_MOD_SHIFT;
        return (uint8_t)(0x04 + (c - 'A'));
    }
    if (c >= '1' && c <= '9') return (uint8_t)(0x1E + (c - '1'));
    switch (c) {
        case '0': return 0x27;
        case ' ': return BTKBD_KEY_SPACE;
        case '\n': case '\r': return BTKBD_KEY_ENTER;
        case '\t': return BTKBD_KEY_TAB;
        case '-': return 0x2D;
        case '_': *mods_out = BTKBD_MOD_SHIFT; return 0x2D;
        case '=': return 0x2E;
        case '+': *mods_out = BTKBD_MOD_SHIFT; return 0x2E;
        case '[': return 0x2F;
        case '{': *mods_out = BTKBD_MOD_SHIFT; return 0x2F;
        case ']': return 0x30;
        case '}': *mods_out = BTKBD_MOD_SHIFT; return 0x30;
        case '\\': return 0x31;
        case '|': *mods_out = BTKBD_MOD_SHIFT; return 0x31;
        case ';': return 0x33;
        case ':': *mods_out = BTKBD_MOD_SHIFT; return 0x33;
        case '\'': return 0x34;
        case '"': *mods_out = BTKBD_MOD_SHIFT; return 0x34;
        case '`': return 0x35;
        case '~': *mods_out = BTKBD_MOD_SHIFT; return 0x35;
        case ',': return 0x36;
        case '<': *mods_out = BTKBD_MOD_SHIFT; return 0x36;
        case '.': return 0x37;
        case '>': *mods_out = BTKBD_MOD_SHIFT; return 0x37;
        case '/': return 0x38;
        case '?': *mods_out = BTKBD_MOD_SHIFT; return 0x38;
        /* 数字行上排的 shift 符号 */
        case '!': *mods_out = BTKBD_MOD_SHIFT; return 0x1E;
        case '@': *mods_out = BTKBD_MOD_SHIFT; return 0x1F;
        case '#': *mods_out = BTKBD_MOD_SHIFT; return 0x20;
        case '$': *mods_out = BTKBD_MOD_SHIFT; return 0x21;
        case '%': *mods_out = BTKBD_MOD_SHIFT; return 0x22;
        case '^': *mods_out = BTKBD_MOD_SHIFT; return 0x23;
        case '&': *mods_out = BTKBD_MOD_SHIFT; return 0x24;
        case '*': *mods_out = BTKBD_MOD_SHIFT; return 0x25;
        case '(': *mods_out = BTKBD_MOD_SHIFT; return 0x26;
        case ')': *mods_out = BTKBD_MOD_SHIFT; return 0x27;
        default: return 0;
    }
}

/* ---- 报文发送 --------------------------------------------------------- */
static void bt_send_report(uint8_t mods, uint8_t usage) {
    if (!bt_input) return;
    uint8_t report[8] = {mods, 0, usage, 0, 0, 0, 0, 0};
    bt_input->setValue(report, sizeof(report));
    if (bt_connected) bt_input->notify();
}

/* 点一下：按下报文 + 空报文（松开）。主机自己按需重复按住不放，
   这里不做电平保持，所以长按不会自动连发 —— 对键盘输入足够用。 */
static void bt_tap(uint8_t usage, uint8_t mods) {
    if (!usage) return;
#ifdef use_lib_log_serial
    /* 调试：BT 模式下 loop 不跑任何周期打印，靠这两类日志才能远程确认键盘通路 */
    Serial.printf("[bt] tap usage=%02X mods=%02X conn=%d\n", usage, mods,
                  bt_connected ? 1 : 0);
#endif
    bt_send_report(mods, usage);
    delay(6);
    bt_send_report(0, 0);
    delay(4);
}

/* 退出 BT 模式（按背后那颗 btnGO / G0 触发）：
   注意 BT 模式下模拟器和内部盘**根本没初始化**（BLE 吃掉连续堆后
   `[flash] mount rc=0x101 (ESP_ERR_NO_MEM)` → images=0），所以不能就地恢复，
   只能干净重启一次；bt_skip_once 让这次重启跳过 BT 直接跑 DOS。 */
static void bt_leave_mode(void) {
#ifdef use_lib_log_serial
    Serial.printf("[bt] leave: restart into DOS\n");
#endif
    auto &display = M5Cardputer.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(8, 40);
    display.print("Leaving BT mode...");
    cardputer_btkbd_request_skip_once();
    cardputer_btkbd_stop();
    delay(400);
    ESP.restart();
}

/* ---- 崩溃循环保护 -----------------------------------------------------
 * BT 模式是在 setup() 里启动的：只要 BLE 初始化踩一次 panic（本次就踩到了
 * BLEHIDDevice::manufacturer() 的空指针），设备就会「复位 → 又进 BT → 又崩」，
 * 连菜单都摸不到。这里用 RTC_NOINIT 内存数开机次数：连崩 3 次就本次跳过 BT，
 * 保证永远能回到 DOS/菜单（断电或正常情况下计数会被清）。 */
#define BTKBD_GUARD_MAGIC 0xB7C0DA7AUL
RTC_NOINIT_ATTR static uint32_t bt_guard_magic;
RTC_NOINIT_ATTR static uint32_t bt_guard_count;
/* 「下一次启动先别进 BT」的一次性标记：btngo 退出时置上，然后 ESP.restart()
   干净重启去跑 DOS（BT 模式下模拟器/内部盘根本没初始化，见下面 poll 里的说明）。 */
RTC_NOINIT_ATTR static uint32_t bt_skip_once;

static uint32_t *bt_guard(void) {
    if (bt_guard_magic != BTKBD_GUARD_MAGIC) {
        bt_guard_magic = BTKBD_GUARD_MAGIC;
        bt_guard_count = 0;
        bt_skip_once = 0;
    }
    return &bt_guard_count;
}

bool cardputer_btkbd_take_skip_once(void) {
    bt_guard();                 /* 保证 magic 有效 */
    if (bt_skip_once) {
        bt_skip_once = 0;
        return true;
    }
    return false;
}

void cardputer_btkbd_request_skip_once(void) {
    bt_guard();
    bt_skip_once = 1;
}

bool cardputer_btkbd_should_autostart(void) {
    uint32_t *count = bt_guard();
    if (*count >= 3) {
        /* 连着崩 3 次：本次跳过 BT 让 DOS 起来（用户才有菜单可点），
           同时把计数压回 1 —— 下次开机还会再试一次（自愈，不会永久关闭）。 */
        *count = 1;
        return false;
    }
    return true;
}

/* ---- 状态屏 ----------------------------------------------------------- */
const char *cardputer_btkbd_status_text(void) {
    return bt_connected ? "Connected" : "Advertising - pair now";
}

static void bt_draw_status(void) {
    auto &display = M5Cardputer.Display;
    display.fillScreen(TFT_BLACK);
    display.fillRect(0, 0, display.width(), 16, TFT_BLUE);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE, TFT_BLUE);
    display.setCursor(4, 4);
    display.print("BT keyboard mode");
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(6, 26);
    display.print(cardputer_btkbd_status_text());
    display.setTextColor(bt_connected ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
    display.setCursor(6, 40);
    display.print(bt_connected ? "Keys go to the host." : "Open BT on the host,");
    if (!bt_connected) {
        display.setCursor(6, 52);
        display.print("pick 'CardPuter86 KBD'.");
    }
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(6, 70);
    display.print("Exit: press btnGO (back)");
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    display.setCursor(6, 82);
    display.print("(reboots into DOS)");
    const int32_t battery = M5Cardputer.Power.getBatteryLevel();
    if (battery >= 0) {
        char line[24];
        snprintf(line, sizeof(line), "Battery %ld%%", (long)battery);
        display.setCursor(6, 100);
        display.print(line);
    }
    status_drawn_ms = millis();
    status_drawn_connected = bt_connected;
}

/* ---- BLE 回调 -------------------------------------------------------- */
/* BLE 回调跑在蓝牙协议栈自己的任务里：这里**只能改标志位**，不要碰 TFT（SPI）
   也不要读 I2C 电量 —— 会和主循环抢总线。真正的重画/报电量都在 poll() 里做。 */
class BtKbdServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *server) override {
        (void)server;
        bt_connected = true;
        bt_status_dirty = true;
        bt_battery_pending = true;
    }
    void onDisconnect(BLEServer *server) override {
        (void)server;
        bt_connected = false;
        bt_battery_pending = false;
        /* 让主机可以重连（断开后 bluedroid 不会自动重新广播） */
        BLEAdvertising *adv = BLEDevice::getAdvertising();
        if (adv) adv->start();
        bt_status_dirty = true;
    }
};

/* ---- 生命周期 -------------------------------------------------------- */
bool cardputer_btkbd_start(void) {
    if (bt_active) return true;
    /* 记一次「进 BT 的尝试」：只要 BT 循环活过 5 秒就会清零（见 poll）。
       连续崩 3 次后 should_autostart() 会拦住自动进入。 */
    (*bt_guard())++;
    BLEDevice::init("CardPuter86 KBD");
    BLEDevice::setMTU(64);

    BLEServer *server = BLEDevice::createServer();
    if (!server) {
        BLEDevice::deinit(true);
        return false;
    }
    server->setCallbacks(new BtKbdServerCallbacks());

    /* inputReport() 的特征值权限是"需加密"，所以必须配 Just Works 绑定，
       否则 Windows/安卓订阅通知时会被拒。 */
    BLESecurity *security = new BLESecurity();
    security->setAuthenticationMode(ESP_LE_AUTH_BOND);
    security->setCapability(ESP_IO_CAP_NONE);
    security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

    bt_hid = new BLEHIDDevice(server);
    bt_input = bt_hid->inputReport(1);
    if (!bt_input) {
        BLEDevice::deinit(true);
        bt_hid = nullptr;
        return false;
    }
    /* 坑：BLEHIDDevice::manufacturer(std::string) 内部直接对
       m_manufacturerCharacteristic 调 setValue()，而那个指针只有无参的
       manufacturer() 才会 createCharacteristic —— 直接传字符串就是空指针
       （LoadProhibited，炸在 BLECharacteristic::setValue → Semaphore::take）。
       必须先调无参版本把特征建出来，再 setValue。 */
    BLECharacteristic *manufacturer_char = bt_hid->manufacturer();
    if (manufacturer_char) {
        std::string manufacturer_name = "CardPuter86";
        manufacturer_char->setValue(manufacturer_name);
    }
    bt_hid->pnp(0x02, 0x303A, 0x86A1, 0x0100);
    bt_hid->hidInfo(0x00, 0x01);
    bt_hid->reportMap((uint8_t *)kbd_report_map, sizeof(kbd_report_map));
    /* 坑：setBatteryLevel() 内部对 battery 特征调 notify()，而 notify() 里的
       assert(getService()->getServer()) 在 -O2/NDEBUG 下被编掉，服务还没 start()
       时就是空指针 → LoadProhibited 死机。所以电池电量一定要等到
       startServices() 之后、而且有主机连上时再报（见 onConnect）。 */
    bt_hid->startServices();

    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->setAppearance(0x03C1);   /* HID Keyboard */
    advertising->addServiceUUID(bt_hid->hidService()->getUUID());
    advertising->setScanResponse(true);
    advertising->setMinPreferred(0x06);
    advertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();

    prev_char_count = 0;
    prev_fn_index = 0xFF;
    prev_enter = prev_space = prev_tab = prev_del = false;
    bt_alive_since_ms = 0;
    bt_active = true;
    bt_draw_status();
    return true;
}

void cardputer_btkbd_stop(void) {
    if (!bt_active) return;
    bt_active = false;
    bt_connected = false;
    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    if (advertising) advertising->stop();
    bt_input = nullptr;
    bt_hid = nullptr;
    /* true = 连 controller 一起释放，把 ~40KB 还给堆 */
    BLEDevice::deinit(true);
}

bool cardputer_btkbd_active(void) {
    return bt_active;
}

void cardputer_btkbd_poll(void) {
    if (!bt_active) return;
    M5Cardputer.update();

    /* 活够 5 秒说明 BLE 真的起来了：清掉崩溃计数（见 bt_guard） */
    if (bt_alive_since_ms == 0) bt_alive_since_ms = millis();
    if ((uint32_t)(millis() - bt_alive_since_ms) > 5000) {
        *bt_guard() = 0;
    }
#ifdef use_lib_log_serial
    /* 心跳：BT 模式下没有别的周期打印，这条是「BT 循环还活着」的唯一证据 */
    static uint32_t bt_heartbeat_ms = 0;
    if ((uint32_t)(millis() - bt_heartbeat_ms) > 5000) {
        bt_heartbeat_ms = millis();
        Serial.printf("[bt] alive conn=%d heap=%lu max=%lu\n",
                      bt_connected ? 1 : 0,
                      (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)ESP.getMaxAllocHeap());
    }
#endif

    /* 退出序列超时：把一直压着没发的字符放出去 */
    if (M5Cardputer.BtnA.wasPressed()) {
        bt_leave_mode();
        return;
    }

    Keyboard_Class::KeysState state = M5Cardputer.Keyboard.keysState();
    const uint8_t ctrl_alt = (uint8_t)((state.ctrl ? BTKBD_MOD_CTRL : 0) |
                                       (state.alt ? BTKBD_MOD_ALT : 0));

    /* 可打印字符：只在"按下沿"发一次，M5 已经把 Aa 折算进字符本身 */
    for (char c : state.word) {
        bool fresh = true;
        for (uint8_t i = 0; i < prev_char_count; i++) {
            if (prev_chars[i] == c) {
                fresh = false;
                break;
            }
        }
        if (!fresh) continue;
        uint8_t mods = 0;
        bt_tap(bt_hid_for_char(c, &mods), (uint8_t)(mods | ctrl_alt));
    }
    prev_char_count = 0;
    for (char c : state.word) {
        if (prev_char_count >= sizeof(prev_chars)) break;
        prev_chars[prev_char_count++] = c;
    }

    /* Fn 层：方向键 / Esc / Delete / Home / F1..F12（都走 cardputer_input 的沿检测） */
    if (state.fn) {
        if (cardputer_input_consume(CARDPUTER_VK_UP)) bt_tap(BTKBD_KEY_UP, ctrl_alt);
        if (cardputer_input_consume(CARDPUTER_VK_DOWN)) bt_tap(BTKBD_KEY_DOWN, ctrl_alt);
        if (cardputer_input_consume(CARDPUTER_VK_LEFT)) bt_tap(BTKBD_KEY_LEFT, ctrl_alt);
        if (cardputer_input_consume(CARDPUTER_VK_RIGHT)) bt_tap(BTKBD_KEY_RIGHT, ctrl_alt);
        if (cardputer_input_consume(CARDPUTER_VK_ESC)) bt_tap(BTKBD_KEY_ESC, ctrl_alt);
        if (cardputer_input_consume(CARDPUTER_VK_HOME)) bt_tap(BTKBD_KEY_HOME, ctrl_alt);
        if (cardputer_input_consume(CARDPUTER_VK_DEL)) bt_tap(BTKBD_KEY_DELETE, ctrl_alt);

        const uint8_t fn_index = cardputer_input_function_index();
        if (fn_index != prev_fn_index && fn_index < 12) {
            bt_tap((uint8_t)(BTKBD_KEY_F1 + fn_index), ctrl_alt);
        }
        prev_fn_index = fn_index;
    } else {
        prev_fn_index = 0xFF;
    }

    /* 特殊键：回车 / 空格 / Tab / 退格（退格在 Fn 层让位给 Delete） */
    if (state.enter && !prev_enter) bt_tap(BTKBD_KEY_ENTER, ctrl_alt);
    if (state.space && !prev_space) bt_tap(BTKBD_KEY_SPACE, ctrl_alt);
    if (state.tab && !prev_tab) bt_tap(BTKBD_KEY_TAB, ctrl_alt);
    if (state.del && !state.fn && !prev_del) bt_tap(BTKBD_KEY_BACKSPACE, ctrl_alt);
    prev_enter = state.enter;
    prev_space = state.space;
    prev_tab = state.tab;
    prev_del = state.del;

    /* 状态屏：连接状态变化时立即重画，否则 1 秒一次刷电量。
       报电量留在主循环里做（setBatteryLevel 会 notify，必须在主任务且已连接时调）。 */
    if (bt_connected && bt_battery_pending && bt_hid) {
        bt_battery_pending = false;
        int32_t level = M5Cardputer.Power.getBatteryLevel();
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        bt_hid->setBatteryLevel((uint8_t)level);
        bt_status_dirty = true;
    }
    const bool stale = (uint32_t)(millis() - status_drawn_ms) > 1000;
    if (bt_status_dirty || bt_connected != status_drawn_connected || stale) {
        bt_status_dirty = false;
        bt_draw_status();
    }
}

#else  /* !BTKBD_AVAILABLE：这版 SDK 没编进 bluedroid，全部退化成空实现 */

bool cardputer_btkbd_start(void) { return false; }
void cardputer_btkbd_stop(void) {}
bool cardputer_btkbd_active(void) { return false; }
bool cardputer_btkbd_should_autostart(void) { return false; }
bool cardputer_btkbd_take_skip_once(void) { return false; }
void cardputer_btkbd_request_skip_once(void) {}
void cardputer_btkbd_poll(void) {}
const char *cardputer_btkbd_status_text(void) { return "unavailable"; }

#endif
