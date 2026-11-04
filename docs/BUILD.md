# 构建与刷写 / Build and flash

**中文** | [English](#english)

## 一、准备

| 需要 | 版本 / 说明 |
| --- | --- |
| 硬件 | M5Stack Cardputer（ESP32-S3，8MB flash） |
| PlatformIO Core | `pip install platformio`（本项目在 6.2 上验证） |
| Python | 3.8+（打包工具只用标准库） |
| Arduino-ESP32 框架 | 由 PlatformIO 自动拉取，本项目验证版本 **2.0.14** |
| 数据线 | 必须是**能传数据**的 USB-C 线；充电线会让设备枚举失败（`未知 USB 设备(设备描述符请求失败)`） |

## 二、构建

```sh
cd ESP32/CardPuter86
pio run -e cardputer86-lean                       # 固件
pio run -t buildfs -e cardputer86-lean            # 文件系统镜像（把 data/ 打进 fatfs）
```

产物：

```
.pio/build/cardputer86-lean/firmware.bin    约 1.03 MB（Flash 49%）
.pio/build/cardputer86-lean/fatfs.bin       固定 5,632,000 字节（整个 disk 分区）
```

`pio` 每次只会保留**最后构建的那个 env** 的 `.pio/build/<env>` 目录，其它 env 的产物会被清掉。
要保留多个变体，构建完立刻把 bin 拷到别处。

### 构建环境对照

| env | 用途 | 备注 |
| --- | --- | --- |
| `cardputer86` | 完整版（含 WiFi / Hayes modem），**不带**串口日志 | 上游默认 |
| `cardputer86-lean` | **日常使用推荐** | `-DCARDPUTER86_NO_MODEM=1`，省约 25KB 静态 RAM，guest 页缓存更多、换页更少 |
| `cardputer86-dbg` | 带 USB-CDC 串口日志 | 用来看 `Serial.printf` |
| `cardputer86-dbglean` | 串口日志 + 周期性 40×16 屏幕 dump | 调试 guest（DOS 里跑的）程序时最有用 |
| `cardputer86-warn` | 只做编译器告警诊断，**不出可用固件** | 用来把 `-w` 压住的栈越界类警告吐出来 |

体积参考（本机实测）：`lean` Flash 49.3%；加上蓝牙键盘（BLE）后 78.5%；
`dbglean`（modem + BLE 同时开）约 99.6%，只剩 9KB —— **不要指望在那种配置下还能加功能**。

## 三、刷写

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 8MB \
  0x10000  .pio/build/cardputer86-lean/firmware.bin \
  0x2A1000 .pio/build/cardputer86-lean/fatfs.bin
```

Windows：`--port COM4`。地址不能改：

| 地址 | 内容 | 说明 |
| --- | --- | --- |
| `0x10000` | `firmware.bin` | app0 分区（2 MB） |
| `0x2A1000` | `fatfs.bin` | disk 分区**跳过 0x1000 字节磨损均衡头**后的 FAT 起点。写 `0x2A0000` 会挂不上 |

### ⚠️ 必须 `--flash-mode dio`

`platformio.ini` 里 `board_build.flash_mode = qio`，但**刷写时必须覆盖成 `dio`**。
用 QIO 刷进去会在 ROM 层直接进入
`rst:0x7 (TG0WDT_SYS_RST)` 无限重启，屏幕全黑，串口也不会给你任何提示。
所以请不要用 `pio run -t upload`，直接用上面的 esptool 命令（或改用 DIO 后自己验证）。

### 只更新盘内文件

只改了 `data/` 里的镜像、没动固件时，可以只刷 fatfs：

```sh
pio run -t buildfs -e cardputer86-lean
esptool.py ... 0x2A1000 .pio/build/cardputer86-lean/fatfs.bin
```

## 四、第一次开机

1. 上电：POST（`Turbo XT BIOS v2.5` → `Testing RAM: 256K OK`）→ 引导扇区 → DOS 提示符。
2. `cardputer86-lean` 大约 **30 秒**到 `A:\>`；`dbglean` 因为要在串口打日志会慢一些。
3. 没有键盘焦点问题时，直接敲 `dir`、`minigame` 等。

按键：WASD 直用；方向键是 `Fn` + `; , . /`；`Fn` + `` ` `` 是 Esc；
`Fn` + 空格 打开固件设置菜单（Settings → More settings 里有 Guest RAM、CPU 档位、
ESP32 主频、**Boot system**、蓝牙键盘开关等）。

## 五、串口日志（调试用）

只有 `dbg` / `dbglean` 会输出日志，波特率 115200、走 USB CDC。

- **打开串口的瞬间会拉 DTR 复位设备**，所以「等它启动完再发命令」必须在一个脚本会话里完成。
- 主机断开后如果固件还在写串口会卡住，因此固件里设了 `Serial.setTxTimeoutMs(20)`。
- `dbglean` 在屏幕内容变化时会重打整屏 16 行（`|...|` 包裹），看它就知道 guest 屏幕上是什么。

## 六、常见构建/刷写故障

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 刷完黑屏、无限重启，`rst:0x7 (TG0WDT_SYS_RST)` | 用 QIO 刷的 | 加 `--flash-mode dio` 重刷 |
| `undefined reference to 'jj_fast_putpixel(...)'`（在 `-O2` 下） | 非 `static` 的 `inline` 定义在 `-O2` 下不再发射外部副本 | 本仓库已把这两个函数改成头文件里的 `static inline` |
| `Stack smashing protect failure!` | `-O2` 后栈布局变化，暴露了定长数组越界 | 本仓库已修（`cardputer_display.cpp` 的屏幕 dump 缓冲区） |
| `error: 'class HardwareSerial' has no member named 'setTxTimeoutMs'` | 给 `lean` 硬加 `-Duse_lib_log_serial=1`，但 lean 没有 USB-CDC 编译开关 | 用 `dbg` / `dbglean` 看日志，不要改 lean |
| `error: redefinition of 'bool cardputer_modem_save_wifi(...)'` | 用 `-DCARDPUTER86_NO_MODEM=1` 编 `dbglean`：头文件里的 inline 空实现和 `.cpp` 实现同时存在 | 要么用 `build_src_filter` 排掉 `.cpp`，要么别定义 `NO_MODEM` |
| `pio run -e <新 env>` 长时间零输出 | 新建 env 需要联网解析 platform 包 | 离线环境请只复用已有 env |
| 串口打不开、`could not open port` | 上一次会话没正常关闭 / 设备在重新枚举 / 充电线 | 等几秒重试；换数据线；必要时拔插设备 |
| `error: cannot convert 'bool*' to 'uint8_t*'` | `slot_dirty[]` 是 `bool[]` | 指针类型要跟数组一致（本仓库已处理） |

## 七、目录/分区

```
partitions_cardputer86.csv
  nvs      0x9000   0x5000      WiFi 凭据、设置项、镜像偏好
  coredump 0xe000   0x2000
  app0     0x10000  0x200000    固件
  swap     0x210000 0x090000    guest RAM 换页后端（576 KB）
  disk     0x2A0000 0x560000    文件系统（启动盘镜像），前 0x1000 字节是磨损均衡头
```

设置项存放在 NVS 的 `cardputer86` 命名空间里，键名：

| 键 | 含义 |
| --- | --- |
| `ramkb` | guest 内存 KB（128 / 256 / 512） |
| `mcu80` | ESP32 主频是否 80MHz |
| `btkbd` | 蓝牙键盘模式开关 |
| `pref_img` / `pref_store` | 下次启动用哪张镜像 |
| `wifi_ssid` / `wifi_pass` | WiFi 凭据 |

---

# English

## 1. Requirements

| Need | Detail |
| --- | --- |
| Hardware | M5Stack Cardputer (ESP32-S3, 8 MB flash) |
| PlatformIO Core | `pip install platformio` (validated on 6.2) |
| Python | 3.8+ (the packer is stdlib-only) |
| Arduino-ESP32 framework | Fetched by PlatformIO; validated against **2.0.14** |
| Cable | A USB-C cable that **carries data**. Charge-only cables make the device fail enumeration (`Unknown USB device (Device Descriptor Request Failed)`) |

## 2. Build

```sh
cd ESP32/CardPuter86
pio run -e cardputer86-lean                       # firmware
pio run -t buildfs -e cardputer86-lean            # filesystem image (packs data/ into fatfs)
```

Outputs:

```
.pio/build/cardputer86-lean/firmware.bin    ~1.03 MB (49% of Flash)
.pio/build/cardputer86-lean/fatfs.bin       exactly 5,632,000 bytes (the whole disk partition)
```

PlatformIO keeps only the `.pio/build/<env>` directory of the **last** env it built and wipes
the others; copy a variant's bins elsewhere right after building it.

### Environments

| env | Purpose | Notes |
| --- | --- | --- |
| `cardputer86` | Full build (WiFi / Hayes modem), **no** serial logs | upstream default |
| `cardputer86-lean` | **recommended for daily use** | `-DCARDPUTER86_NO_MODEM=1`, saves ~25 KB of static RAM, so more guest page cache and fewer swaps |
| `cardputer86-dbg` | Serial logs over USB CDC | for `Serial.printf` |
| `cardputer86-dbglean` | Serial logs plus a periodic 40x16 screen dump | the one to use when debugging guest (DOS-side) programs |
| `cardputer86-warn` | Compiler-warning diagnostics only, **not flashable** | surfaces stack-overflow-class warnings that `-w` hides |

Measured sizes: `lean` uses 49.3% of Flash; adding the BLE keyboard pushes it to 78.5%;
`dbglean` with modem **and** BLE lands around 99.6% with 9 KB to spare - do not expect room
for more features there.

## 3. Flash

```sh
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 8MB \
  0x10000  .pio/build/cardputer86-lean/firmware.bin \
  0x2A1000 .pio/build/cardputer86-lean/fatfs.bin
```

On Windows use `--port COM4`. The addresses are fixed:

| Address | Content | Why |
| --- | --- | --- |
| `0x10000` | `firmware.bin` | app0 partition (2 MB) |
| `0x2A1000` | `fatfs.bin` | the disk partition **skipping its 0x1000-byte wear-levelling header**; writing at `0x2A0000` will not mount |

### You must pass `--flash-mode dio`

`platformio.ini` sets `board_build.flash_mode = qio`, but the flash step has to override it to
`dio`. Flashed as QIO the device enters `rst:0x7 (TG0WDT_SYS_RST)` in a boot loop: black
screen, no serial output, no clue. That is why the documented flow calls esptool directly
rather than `pio run -t upload`.

### Updating only the filesystem

If you only changed the image under `data/`, flash just fatfs:

```sh
pio run -t buildfs -e cardputer86-lean
esptool.py ... 0x2A1000 .pio/build/cardputer86-lean/fatfs.bin
```

## 4. First boot

1. Power on: POST (`Turbo XT BIOS v2.5` -> `Testing RAM: 256K OK`) -> boot sector -> DOS prompt.
2. `cardputer86-lean` reaches `A:\>` in roughly **30 seconds**; `dbglean` is slower because it
   logs over serial.
3. Then just type `dir`, `minigame`, and so on.

Keys: WASD is direct; arrows are `Fn` + `; , . /`; `Fn` + `` ` `` is Esc;
`Fn` + Space opens the firmware settings menu (Settings -> More settings holds Guest RAM,
CPU profile, ESP32 clock, **Boot system** and the Bluetooth keyboard switch).

## 5. Serial logs

Only `dbg` / `dbglean` produce logs, 115200 baud over USB CDC.

- **Opening the port pulls DTR and resets the device**, so "wait for boot, then send a
  command" must happen inside a single script session.
- If the host disconnects while the firmware is still writing, the write blocks and the device
  can wedge; the firmware therefore calls `Serial.setTxTimeoutMs(20)`.
- `dbglean` re-prints the whole 16-line screen (wrapped in `|...|`) whenever it changes, which
  tells you exactly what the guest sees.

## 6. Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| Black screen, boot loop, `rst:0x7 (TG0WDT_SYS_RST)` | flashed as QIO | re-flash with `--flash-mode dio` |
| `undefined reference to 'jj_fast_putpixel(...)'` under `-O2` | a non-`static` `inline` definition is no longer emitted at `-O2` | fixed here: those two helpers are `static inline` in the header |
| `Stack smashing protect failure!` | `-O2` changed the stack layout and exposed a fixed-size array overflow | fixed here (`cardputer_display.cpp` screen-dump buffer) |
| `error: 'class HardwareSerial' has no member named 'setTxTimeoutMs'` | adding `-Duse_lib_log_serial=1` to `lean`, which has no USB-CDC flags | use `dbg` / `dbglean` for logs; leave lean alone |
| `error: redefinition of 'bool cardputer_modem_save_wifi(...)'` | building `dbglean` with `-DCARDPUTER86_NO_MODEM=1`: header stubs and the `.cpp` both exist | exclude the `.cpp` via `build_src_filter`, or drop `NO_MODEM` |
| A brand-new `pio run -e <env>` hangs silently | a new env needs network access to resolve platform packages | reuse existing envs when offline |
| Port will not open (`could not open port`) | previous session did not close cleanly / re-enumeration / charge-only cable | retry after a few seconds; swap the cable; replug |
| `error: cannot convert 'bool*' to 'uint8_t*'` | `slot_dirty[]` is `bool[]` | match the pointer type (already handled here) |

## 7. Partitions and settings

```
partitions_cardputer86.csv
  nvs      0x9000   0x5000     WiFi credentials, settings, boot image preference
  coredump 0xe000   0x2000
  app0     0x10000  0x200000   firmware
  swap     0x210000 0x090000   guest RAM swap backend (576 KB)
  disk     0x2A0000 0x560000   filesystem (boot disk image); first 0x1000 bytes are a wear-levelling header
```

Settings live in the NVS namespace `cardputer86`:

| Key | Meaning |
| --- | --- |
| `ramkb` | guest RAM in KB (128 / 256 / 512) |
| `mcu80` | whether the ESP32 runs at 80 MHz |
| `btkbd` | Bluetooth keyboard mode |
| `pref_img` / `pref_store` | which image boots next |
| `wifi_ssid` / `wifi_pass` | WiFi credentials |
