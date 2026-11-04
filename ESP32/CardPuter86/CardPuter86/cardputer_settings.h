#ifndef CARDPUTER_SETTINGS_H
#define CARDPUTER_SETTINGS_H

#include <Arduino.h>

void cardputer_settings_init(void);
uint32_t cardputer_settings_sleep_timeout_seconds(void);
bool cardputer_settings_set_sleep_timeout_seconds(uint32_t seconds);
uint8_t cardputer_settings_usb_mode(void);
bool cardputer_settings_set_usb_mode(uint8_t mode);
bool cardputer_settings_sleep_led_enabled(void);
bool cardputer_settings_set_sleep_led_enabled(bool enabled);
bool cardputer_settings_audio_enabled(void);
bool cardputer_settings_set_audio_enabled(bool enabled);
bool cardputer_settings_com1_enabled(void);
bool cardputer_settings_set_com1_enabled(bool enabled);
// 处理器主频：false = 240 MHz（默认，最快），true = 80 MHz（省电，模拟器约慢 3 倍）
bool cardputer_settings_mcu_80mhz_enabled(void);
bool cardputer_settings_set_mcu_80mhz_enabled(bool enabled);
// 在 setup() 最开头调用：直接读 NVS 并 setCpuFrequencyMhz()，返回实际主频 MHz。
// 必须早于任何外设初始化（TFT/SD/I2S 的时序都按主频算）。
uint32_t cardputer_settings_apply_mcu_clock(void);

// 蓝牙键盘模式（BLE HID keyboard）：true = 下次启动直接进 BT 键盘模式，
// guest 冻结、键盘全部转给配对主机，只有依次按 b-t-n-g-o 才退出。
bool cardputer_settings_bt_kbd_enabled(void);
bool cardputer_settings_set_bt_kbd_enabled(bool enabled);

#endif
