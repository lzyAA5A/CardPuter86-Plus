#include "cardputer_settings.h"
#include <nvs.h>
#include <nvs_flash.h>

static uint32_t sleep_timeout_seconds = 120;
static uint8_t usb_mode = 0; // 0 charge only, 1 CDC serial, 2 USB disk
static bool sleep_led_enabled = true;
static bool audio_enabled = true;
static bool com1_enabled = true;
static bool mcu_80mhz = false;
// 蓝牙键盘模式（BLE HID keyboard）。开启后下次启动直接进 BT 模式，
// 键盘全部转给配对的主机；要退出只能依次按 b-t-n-g-o。
static bool bt_kbd_enabled = false;

void cardputer_settings_init(void) {
    sleep_timeout_seconds = 120;
    usb_mode = 0;
    sleep_led_enabled = true;
    audio_enabled = true;
    com1_enabled = true;
    mcu_80mhz = false;
    bt_kbd_enabled = false;
    if (nvs_flash_init() != ESP_OK) return;

    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t saved = 0;
    uint32_t saved_sleep = 120;
    if (nvs_get_u32(handle, "sleep_sec", &saved_sleep) == ESP_OK &&
        (saved_sleep == 0 || saved_sleep == 30 || saved_sleep == 120 ||
         saved_sleep == 300 || saved_sleep == 600)) {
        sleep_timeout_seconds = saved_sleep;
    }
    uint8_t saved_usb = 0;
    if (nvs_get_u8(handle, "usb_mode", &saved_usb) == ESP_OK && saved_usb <= 2) {
        usb_mode = saved_usb;
    }
    uint8_t saved_sleep_led = 1;
    if (nvs_get_u8(handle, "sleep_led", &saved_sleep_led) == ESP_OK) {
        sleep_led_enabled = saved_sleep_led != 0;
    }
    uint8_t saved_audio = 1;
    if (nvs_get_u8(handle, "audio", &saved_audio) == ESP_OK) {
        audio_enabled = saved_audio != 0;
    }
    uint8_t saved_com1 = 1;
    if (nvs_get_u8(handle, "com1", &saved_com1) == ESP_OK) {
        com1_enabled = saved_com1 != 0;
    }
    uint8_t saved_mcu80 = 0;
    if (nvs_get_u8(handle, "mcu80", &saved_mcu80) == ESP_OK) {
        mcu_80mhz = saved_mcu80 != 0;
    }
    uint8_t saved_btkbd = 0;
    if (nvs_get_u8(handle, "btkbd", &saved_btkbd) == ESP_OK) {
        bt_kbd_enabled = saved_btkbd != 0;
    }
    nvs_close(handle);
}

bool cardputer_settings_mcu_80mhz_enabled(void) {
    return mcu_80mhz;
}

uint32_t cardputer_settings_apply_mcu_clock(void) {
#ifdef G86_FORCE_MCU_MHZ
    // 编译期强制主频：仅用于真机验证（串口写方向可能不通，只能靠日志读回）。
    // pio run -e cardputer86-dbglean，build_flags 加 -DG86_FORCE_MCU_MHZ=80
    mcu_80mhz = (G86_FORCE_MCU_MHZ == 80);
    setCpuFrequencyMhz(G86_FORCE_MCU_MHZ);
    return G86_FORCE_MCU_MHZ;
#endif
    // setup() 最开头就要定下主频：TFT/SD/I2S 的初始化时序都跟主频相关，
    // 等 cardputer_settings_init() 之后再改会让显示和 SD 出怪问题。
    // 这里自己读一次 NVS（nvs_flash_init 可重复调用）。
    bool eco = false;
    if (nvs_flash_init() == ESP_OK) {
        nvs_handle_t handle;
        if (nvs_open("cardputer86", NVS_READONLY, &handle) == ESP_OK) {
            uint8_t saved = 0;
            if (nvs_get_u8(handle, "mcu80", &saved) == ESP_OK) eco = saved != 0;
            nvs_close(handle);
        }
    }
    mcu_80mhz = eco;
    const uint32_t mhz = eco ? 80 : 240;
    if (getCpuFrequencyMhz() != mhz) setCpuFrequencyMhz(mhz);
    return mhz;
}

bool cardputer_settings_set_mcu_80mhz_enabled(bool enabled) {
    mcu_80mhz = enabled;

    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t result = nvs_set_u8(handle, "mcu80", enabled ? 1 : 0);
    const esp_err_t commit_result = result == ESP_OK ? nvs_commit(handle) : result;
    nvs_close(handle);
    if (result == ESP_OK && commit_result == ESP_OK) {
        const uint32_t mhz = enabled ? 80 : 240;
        if (getCpuFrequencyMhz() != mhz) setCpuFrequencyMhz(mhz);
    }
    return result == ESP_OK && commit_result == ESP_OK;
}

bool cardputer_settings_sleep_led_enabled(void) {
    return sleep_led_enabled;
}

bool cardputer_settings_set_sleep_led_enabled(bool enabled) {
    sleep_led_enabled = enabled;

    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t result = nvs_set_u8(handle, "sleep_led", enabled ? 1 : 0);
    const esp_err_t commit_result = result == ESP_OK ? nvs_commit(handle) : result;
    nvs_close(handle);
    return result == ESP_OK && commit_result == ESP_OK;
}

bool cardputer_settings_audio_enabled(void) {
    return audio_enabled;
}

bool cardputer_settings_set_audio_enabled(bool enabled) {
    audio_enabled = enabled;

    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t result = nvs_set_u8(handle, "audio", enabled ? 1 : 0);
    const esp_err_t commit_result = result == ESP_OK ? nvs_commit(handle) : result;
    nvs_close(handle);
    return result == ESP_OK && commit_result == ESP_OK;
}

bool cardputer_settings_com1_enabled(void) {
    return com1_enabled;
}

bool cardputer_settings_set_com1_enabled(bool enabled) {
    com1_enabled = enabled;

    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t result = nvs_set_u8(handle, "com1", enabled ? 1 : 0);
    const esp_err_t commit_result = result == ESP_OK ? nvs_commit(handle) : result;
    nvs_close(handle);
    return result == ESP_OK && commit_result == ESP_OK;
}

uint8_t cardputer_settings_usb_mode(void) {
    return usb_mode;
}

bool cardputer_settings_bt_kbd_enabled(void) {
    return bt_kbd_enabled;
}

bool cardputer_settings_set_bt_kbd_enabled(bool enabled) {
    bt_kbd_enabled = enabled;

    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t result = nvs_set_u8(handle, "btkbd", enabled ? 1 : 0);
    const esp_err_t commit_result = result == ESP_OK ? nvs_commit(handle) : result;
    nvs_close(handle);
    return result == ESP_OK && commit_result == ESP_OK;
}

bool cardputer_settings_set_usb_mode(uint8_t mode) {
    if (mode > 2) return false;
    usb_mode = mode;

    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t result = nvs_set_u8(handle, "usb_mode", mode);
    const esp_err_t commit_result = result == ESP_OK ? nvs_commit(handle) : result;
    nvs_close(handle);
    return result == ESP_OK && commit_result == ESP_OK;
}

uint32_t cardputer_settings_sleep_timeout_seconds(void) {
    return sleep_timeout_seconds;
}

bool cardputer_settings_set_sleep_timeout_seconds(uint32_t seconds) {
    if (!(seconds == 0 || seconds == 30 || seconds == 120 ||
          seconds == 300 || seconds == 600)) {
        return false;
    }
    sleep_timeout_seconds = seconds;

    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t result = nvs_set_u32(handle, "sleep_sec", seconds);
    const esp_err_t commit_result = result == ESP_OK ? nvs_commit(handle) : result;
    nvs_close(handle);
    return result == ESP_OK && commit_result == ESP_OK;
}
