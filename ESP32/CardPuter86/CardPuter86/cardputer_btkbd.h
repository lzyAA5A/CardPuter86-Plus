#ifndef CARDPUTER_BTKBD_H
#define CARDPUTER_BTKBD_H

#include <Arduino.h>

/* 蓝牙键盘模式：CardPuter 变成一台 BLE HID 键盘，按键转给配对的主机。
   由设置菜单（Fn+空格 → Settings → BT keyboard）开启，写在 NVS 的 "btkbd"，
   下次启动仍然有效。进入之后主机拿到全部按键，所以要退出只能按机器**背后那颗
   btnGO（GPIO0 / BtnA）**——按一下即“Leaving BT mode...”，干净重启进 DOS。

   注意 BT 模式下模拟器完全不初始化（见 CardPuter86.ino 的 setup()）：BLE 吃掉
   连续堆后内部盘 FatFS 会以 ESP_ERR_NO_MEM 挂不上，所以退出只能靠重启。

   内存：BLE 协议栈要一次吃下约 40KB 动态内存，所以必须在 guest 页缓存
   （CreateRAM）之前启动 —— 和 WiFi 当年同一个坑。启动期失败就意味着堆不够，
   设置菜单里也提供运行期尝试（失败会提示重启）。 */
bool cardputer_btkbd_start(void);
void cardputer_btkbd_stop(void);
bool cardputer_btkbd_active(void);
/* 崩溃循环保护：BT 连续把设备搞崩 3 次后返回 false（本次启动不进 BT，
   保证还能回 DOS/菜单）。计数放 RTC_NOINIT，BT 循环活过 5 秒自动清零。 */
bool cardputer_btkbd_should_autostart(void);
/* 一次性「这次启动先别进 BT」标记（RTC_NOINIT）：btngo 退出时置上再 ESP.restart()，
   重启后 take_skip_once() 返回 true → 正常跑 DOS；再下次上电又回 BT 模式。 */
bool cardputer_btkbd_take_skip_once(void);
void cardputer_btkbd_request_skip_once(void);
/* 在 loop() 里每轮调用一次：扫键盘、发 HID 报文、监视退出序列、画状态屏。 */
void cardputer_btkbd_poll(void);
const char *cardputer_btkbd_status_text(void);

#endif
