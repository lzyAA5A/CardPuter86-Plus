# 镜像打包 / Image packing

**中文** | [English](#english)

模拟器没有硬盘控制器固件，它把一张**软盘镜像**当成整台机器的启动源：上电后 ROM 从
镜像的第 0 号扇区读引导扇区并执行。所以"系统盘"就是你给它哪张软盘镜像。

本仓库提供一个打包工具 `tools/cp86img/cp86img.py`，把**你自己的** DOS 启动盘变成
一张带好全部程序的 CardPuter86 系统盘。

---

## 一、模拟器对镜像的要求（硬性）

| 要求 | 说明 |
| --- | --- |
| 文件大小 | 必须命中下表之一。**不在表里的尺寸会被当成硬盘几何**，DOS 读不出盘，表现为黑屏或 `Non-System disk` |
| 文件系统 | **FAT12**（数据簇 < 4085）。FAT16/exFAT/NTFS 不支持 |
| 引导扇区 | 必须是 DOS 自己的。工具**不修改**引导扇区和 `IO.SYS` / `MSDOS.SYS` / `COMMAND.COM` |
| 分区表 | 不需要。软盘镜像的 BPB 直接描述整个卷，没有 MBR |

支持的几何（与 `configure_image_geometry()` 一致）：

```
160K  = 163840    1 面  8 扇区/道  40 柱
180K  = 184320    1 面  9 扇区/道  40 柱
320K  = 327680    2 面  8 扇区/道  40 柱
360K  = 368640    2 面  9 扇区/道  40 柱
720K  = 737280    2 面  9 扇区/道  80 柱
1.2M  = 1228800   2 面 15 扇区/道  80 柱
1.44M = 1474560   2 面 18 扇区/道  80 柱   ← 推荐
2.88M = 2949120   2 面 36 扇区/道  80 柱
```

> 一张 1,720,320 字节、21 扇区/道、OEM 为 `GRLDR` 的 GRUB4DOS 盘就是典型的**不合格**例子：
> 尺寸不在表里，第 0 扇区是 GRUB 而不是 DOS。工具会直接拒绝它。

## 二、镜像怎么送进设备

设备里有两个地方能放镜像：

**1）内部 flash 的 fatfs 分区（推荐，开机默认用这个）**

```
分区表 partitions_cardputer86.csv:
  nvs      0x9000   0x5000
  coredump 0xe000   0x2000
  app0     0x10000  0x200000      ← 固件
  swap     0x210000 0x090000      ← guest 内存换页后端
  disk     0x2A0000 0x560000      ← 系统盘，5.4 MB
```

`disk` 分区前 `0x1000` 字节是磨损均衡头，**FAT 从 `0x2A1000` 开始**。所以：

```sh
cp mycard.img ESP32/CardPuter86/data/cardputer86.img
cd ESP32/CardPuter86
pio run -t buildfs -e cardputer86-lean      # 产出 .pio/build/cardputer86-lean/fatfs.bin
esptool.py --chip esp32s3 --port COM4 --baud 921600 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 8MB \
  0x2A1000 .pio/build/cardputer86-lean/fatfs.bin
```

`fatfs.bin` 固定 5,632,000 字节（整个分区），所以 5.4 MB 能放**多张**镜像共存。

**2）多系统启动**

`data/` 下所有 `*.img` 都会被打进 fatfs。开机的设置菜单（`Fn` + 空格 → Settings →
More settings → **Boot system**）可以选择下次启动哪一张，选择记在 NVS 里。适合
"360K PC-DOS 3.x + 1.44M MS-DOS 5.0" 这种双系统。

> SD 卡上的镜像**现在不能当启动盘**：卡已经被整卡注册成 C: 硬盘（见 README）。
> 想要多系统请把镜像放内部 flash。

## 三、打包工具用法

```
python3 tools/cp86img/cp86img.py <命令> ...

info    IMG                    体检：BPB 几何、空闲空间、FAT 两份是否一致、泄漏簇
list    IMG                    列根目录（含"簇链是否连续"）
build   --base DOS.IMG --out CARD.IMG [选项]     ★ 一条命令生成系统盘
add     IMG DOSNAME FILE       加一个文件（可替换）
del     IMG NAME...            删文件并释放簇
get     IMG NAME OUT           取出文件
verify  IMG NAME FILE          镜像内文件与本地文件逐字节对比
```

`build` 的主要选项：

| 选项 | 作用 |
| --- | --- |
| `--strip-installer` | 删掉 DOS 零售盘的安装负载（默认清单：`SETUP.EXE` `SETUP.INI` `DOS50.ZIP` `PKUNZIP.EXE` `WINA20.386`）。MS-DOS 5.0 单盘版把这 5 个删掉能腾出约 1.06 MB |
| `--apps DIR` | 从这个目录注入程序（默认就是 `tools/cp86img/apps`；`src/` 与源码后缀会被跳过） |
| `--add NAME=FILE` | 额外注入单个文件，可重复 |
| `--drop NAME[,NAME...]` | 删指定文件，可重复 |
| `--path "A:\;C:\;C:\CPP86"` | 生成 `AUTOEXEC.BAT` 里的 PATH；给 `""` 就不写 |
| `--extra-bat "LINE"` | 往 `AUTOEXEC.BAT` 追加一行，可重复 |
| `--autobat-file FILE` | 用整个文件当 `AUTOEXEC.BAT` |
| `--no-autobat` | 完全不碰 `AUTOEXEC.BAT` |
| `--min-free BYTES` | 剩余空间低于这个值就失败（防止做出装不进东西的盘） |
| `--force` / `--in-place` | 覆盖输出 / 允许就地改 |

工具保证的四件事：

1. **按 BPB 解析**，不假设尺寸；尺寸不在支持表里就直接失败。
2. **名字不合法就报错，不截断**（`CPPSCREEN.EXE` 会被拒绝，而不是变成 `CPPSCREE.EXE`）。
3. **两份 FAT 一起写**，并在收尾时校验两份逐字节一致。
4. **写完逐字节回读**；任一文件不一致就**不写输出文件**，退出码 1。

## 四、完整示例：MS-DOS 5.0 单盘 1.44MB

假设你手上有 MS-DOS 5.0 的 1.44MB 启动盘镜像 `mydos.img`（`imgprobe` 显示 OEM=`MSDOS5.0`、
2880 扇区、18 扇区/道、media 0xF0）。这张盘只剩 15 簇（7,680 字节）空闲，因为整份发行包
`DOS50.ZIP` 就占了 1,081,632 字节：

```sh
python3 tools/cp86img/cp86img.py build \
    --base mydos.img \
    --out  ESP32/CardPuter86/data/cardputer86.img \
    --strip-installer
```

实测输出（本机运行原文）：

```
== base ==
  mydos.img: 1474560 bytes, 1.44M, 7680 bytes free
== plan ==
  drop   SETUP.EXE       41251 bytes
  drop   SETUP.INI        1871 bytes
  drop   DOS50.ZIP     1081632 bytes
  drop   PKUNZIP.EXE     17805 bytes
  drop   WINA20.386       9349 bytes
  put    ASM.EXE         44638 bytes  <- ASM.EXE
  put    DIAG.COM        13748 bytes  <- DIAG.COM
  put    G2048.EXE       15262 bytes  <- G2048.EXE
  put    KEYDBG.EXE      12848 bytes  <- KEYDBG.EXE
  put    MARIO.EXE       16236 bytes  <- MARIO.EXE
  put    MINE.EXE        16136 bytes  <- MINE.EXE
  put    MINIGAME.EXE    40264 bytes  <- MINIGAME.EXE
  put    SNAKE.EXE       17232 bytes  <- SNAKE.EXE
  put    TE.EXE          45216 bytes  <- TE.EXE
  put    WTERM.EXE       13278 bytes  <- WTERM.EXE
  write  AUTOEXEC.BAT       47 bytes  (prompt/path)
  free after build: 923648 bytes (1804 clusters)
== verify ==
  ASM.EXE      OK  (44638 bytes)
  ...
  MINIGAME.EXE OK  (40264 bytes)
  ...
  FAT copies    OK  (all 2 in sync)
== done ==
  wrote ...cardputer86.img: 1474560 bytes, 25 files, 923648 bytes free
  sha256 141eeb7aa565316a471a81af054cddb76cca6afaf3fcd0707fc3ac2da4209c1d
```

结果：25 个文件、剩 923,648 字节（比预测一分不差）。在 QEMU（真 SeaBIOS + 真 MS-DOS 5.0）
里引导验证：

```
Booting from Floppy...
A:\>dir
       23 file(s)     456697 bytes
                      923648 bytes free
A:\>md work
A:\>cd work
A:\WORK>path
PATH=A:\;C:\;C:\CPP86
A:\WORK>te
Usage: TE FILE.TXT
```

## 五、360K 盘怎么办

360K 镜像只有 ~181KB 空闲，装不下全部 10 个程序（合计 234KB）。用 `--add` 挑着装：

```sh
python3 tools/cp86img/cp86img.py build --base pc-dos3.img --out card.img \
    --no-apps \
    --add "MINIGAME.EXE=tools/cp86img/apps/MINIGAME.EXE" \
    --add "WTERM.EXE=tools/cp86img/apps/WTERM.EXE" \
    --strip-installer
```

空间不够时工具**不会写出半成品**，而是先算好再报：

```
error: build would not fit: short by 57 cluster(s) (58368 bytes).
       Add more --drop names or start from a larger DOS image.
```

（注意：很多 360K 镜像有"FAT 标记已用但没有文件拥有"的**泄漏簇**，`info` 会报出来。
工具按 FAT 的实际状态算可用空间，所以算出来的空闲字节数可能比 `dir` 显示的少。）

## 六、常见故障速查

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 开机黑屏、无 `A:\>` | 镜像尺寸不在支持表里，被当成硬盘 | `cp86img.py info` 看 `emulator geom` 是否 `supported` |
| `Non-System disk or disk error` | 引导扇区不是 DOS 的，或 `IO.SYS`/`MSDOS.SYS` 不在根目录 | 换一张真 DOS 启动盘做 base |
| `Invalid drive specification`（指 C:） | SD 没挂上（多在没插卡/卡坏状态） | 长按电源彻底断电重开；或换卡 |
| 打完字符少一个、`: > *` 打不出 | 旧固件的 shift 符号 bug | 用本仓库的固件（已修） |
| `Bad command or file name`，但文件明明在盘上 | 名字被截断（>8.3），或盘上同名 `.COM` 挡在前面 | 用 8.3 名字；同目录不要同时放 `.COM` 和 `.EXE` |
| 文件写进去了但 DOS 说读错 | 只更新了一份 FAT | 用本工具（两份一起写并校验） |
| `dir` 显示的可用空间和自己算的不一样 | 泄漏簇 | `cp86img.py info` 看 `leaked` |

## 七、写自己的 DOS 程序

`apps/src/` 里有全部源码，用 Digital Mars 免费编译器编译（16 位、small 模型）：

```sh
dmc -msd -w minigame.cpp -o MINIGAME.EXE
dmc -msd -w te.c         -o TE.EXE
dmc -msd -w mario.c      -o MARIO.EXE
```

注意事项（都是本项目真踩过的）：

- **不要用 `-mt`**：Digital Mars 不带 tiny 模型库，会死在 `Error 2: File Not Found ct.obj`。
- 单个对象/数组 ≤ 64KB（small 模型）；栈只有几 KB，别放大数组在栈上。
- 直接写显存最快：`unsigned char far *v = (unsigned char far *)0xB8000000UL;`
  40 列模式下每行 80 字节（`off = (row * 80 + col) << 1`），一个字符占 2 字节（字符+属性）。
- `INT 16h` 返回 **AH=扫描码、AL=ASCII**，判断字符比 AL。
- 判键有没有：`*((unsigned far *)0x0040001AUL) != *((unsigned far *)0x0040001CUL)`，
  取键再 `int86(0x16, ...)`。`kbhit()` 在 DOS 5.0 下要走 INT 21h，更慢。
- 计时：`*(unsigned long far *)0x0040006CUL`（18.2 tick/秒）。
- 关硬件光标：`r.x.cx = 0x2000; r.h.ah = 0x01; int86(0x10, &r, &r);`
- 退出时把显示模式设回 3 并开光标，否则回到 DOS 提示符会是黑屏。

---

# English

The emulator has no disk-controller firmware: it treats one **floppy image** as the whole boot
source. On reset the ROM reads sector 0 of that image and jumps into it. So "the system disk"
is simply whichever floppy image you give it.

`tools/cp86img/cp86img.py` turns **your own** DOS boot disk into a CardPuter86 system disk with
the bundled programs on it.

## 1. What the emulator requires

| Requirement | Detail |
| --- | --- |
| File size | Must be one of the sizes below. **Any other size is treated as a hard-disk geometry**, so DOS cannot read the disk and you get a black screen or `Non-System disk` |
| Filesystem | **FAT12** (fewer than 4085 data clusters). FAT16 / exFAT / NTFS are not supported |
| Boot sector | Must be DOS's own. The tool **never** touches the boot sector or `IO.SYS` / `MSDOS.SYS` / `COMMAND.COM` |
| Partition table | Not needed - a floppy BPB describes the whole volume; there is no MBR |

Supported geometries (`configure_image_geometry()`):

```
160K  = 163840    1 head   8 sectors/track  40 cylinders
180K  = 184320    1 head   9 sectors/track  40 cylinders
320K  = 327680    2 heads  8 sectors/track  40 cylinders
360K  = 368640    2 heads  9 sectors/track  40 cylinders
720K  = 737280    2 heads  9 sectors/track  80 cylinders
1.2M  = 1228800   2 heads 15 sectors/track  80 cylinders
1.44M = 1474560   2 heads 18 sectors/track  80 cylinders   <- recommended
2.88M = 2949120   2 heads 36 sectors/track  80 cylinders
```

> A 1,720,320-byte GRUB4DOS image with 21 sectors/track and OEM `GRLDR` is the classic
> counter-example: its size is not in the table and its sector 0 is GRUB, not DOS.
> The tool refuses it outright.

## 2. How an image reaches the device

There are two places an image can live:

**Internal flash `disk` partition (recommended - this is what boots by default)**

```
nvs      0x9000   0x5000
coredump 0xe000   0x2000
app0     0x10000  0x200000     <- firmware
swap     0x210000 0x090000     <- guest RAM swap backend
disk     0x2A0000 0x560000     <- system disk, 5.4 MB
```

The first `0x1000` bytes of the `disk` partition are a wear-levelling header, so **the FAT
starts at `0x2A1000`**. Therefore:

```sh
cp mycard.img ESP32/CardPuter86/data/cardputer86.img
cd ESP32/CardPuter86
pio run -t buildfs -e cardputer86-lean      # -> .pio/build/cardputer86-lean/fatfs.bin
esptool.py --chip esp32s3 --port COM4 --baud 921600 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 8MB \
  0x2A1000 .pio/build/cardputer86-lean/fatfs.bin
```

`fatfs.bin` is always 5,632,000 bytes (the whole partition), so several images fit at once.

**Multi-boot**

Every `*.img` under `data/` is baked into fatfs. The settings menu (`Fn` + Space -> Settings ->
More settings -> **Boot system**) selects which one boots next, remembered in NVS.
Multi-boot is what makes "360K PC-DOS 3.x + 1.44M MS-DOS 5.0" practical.

> An image placed on the **SD card is not usable as a boot disk** any more: the card is
> registered whole as hard drive C: (see the README). Keep boot images on internal flash.

## 3. Tool reference

```
python3 tools/cp86img/cp86img.py <command> ...

info    IMG                    BPB geometry, free space, FAT consistency, leaked clusters
list    IMG                    root directory listing (with cluster-chain contiguity)
build   --base DOS.IMG --out CARD.IMG [options]    build a system disk
add     IMG DOSNAME FILE       add or replace one file
del     IMG NAME...            delete files and free their clusters
get     IMG NAME OUT           extract a file
verify  IMG NAME FILE          compare an image file against a local file, byte for byte
```

Main `build` options:

| Option | Effect |
| --- | --- |
| `--strip-installer` | Remove the DOS retail installer payload (`SETUP.EXE` `SETUP.INI` `DOS50.ZIP` `PKUNZIP.EXE` `WINA20.386`). On the single-disk MS-DOS 5.0 image this frees about 1.06 MB |
| `--apps DIR` | Inject programs from a directory (default `tools/cp86img/apps`; `src/` and source extensions are skipped) |
| `--add NAME=FILE` | Inject one extra file (repeatable) |
| `--drop NAME[,NAME...]` | Delete named files (repeatable) |
| `--path "A:\;C:\;C:\CPP86"` | PATH line written into `AUTOEXEC.BAT`; pass `""` to omit |
| `--extra-bat "LINE"` | Append a line to `AUTOEXEC.BAT` (repeatable) |
| `--autobat-file FILE` | Use a whole file as `AUTOEXEC.BAT` |
| `--no-autobat` | Do not touch `AUTOEXEC.BAT` at all |
| `--min-free BYTES` | Fail unless at least this much stays free |
| `--force` / `--in-place` | Overwrite the output / allow writing over the base |

Four guarantees:

1. **Parse the BPB**, never assume a size - and fail loudly on an unsupported one.
2. **Reject invalid names instead of truncating** (`CPPSCREEN.EXE` is refused, not silently
   turned into `CPPSCREE.EXE`).
3. **Write every FAT copy** and verify all copies agree before finishing.
4. **Read every file back**; if anything differs, **no output file is written** and the exit
   code is 1.

## 4. Worked example: MS-DOS 5.0 single-disk 1.44 MB

Take a stock MS-DOS 5.0 1.44 MB boot image `mydos.img` (OEM `MSDOS5.0`, 2880 sectors,
18 sectors/track, media 0xF0). It has only 15 free clusters (7,680 bytes) because the whole
distribution ships as `DOS50.ZIP` (1,081,632 bytes) on the disk:

```sh
python3 tools/cp86img/cp86img.py build \
    --base mydos.img \
    --out  ESP32/CardPuter86/data/cardputer86.img \
    --strip-installer
```

Real output from this machine: 25 files, 923,648 bytes free - exactly what the planner
predicted - and every injected file verified byte-identical, with both FATs in sync.

Booted in QEMU (real SeaBIOS, real MS-DOS 5.0):

```
Booting from Floppy...
A:\>dir
       23 file(s)     456697 bytes
                      923648 bytes free
A:\>md work
A:\>cd work
A:\WORK>path
PATH=A:\;C:\;C:\CPP86
A:\WORK>te
Usage: TE FILE.TXT
```

## 5. What about 360K disks

A 360K image has about 181 KB free - not enough for all ten programs (234 KB). Pick a subset:

```sh
python3 tools/cp86img/cp86img.py build --base pc-dos3.img --out card.img \
    --no-apps \
    --add "MINIGAME.EXE=tools/cp86img/apps/MINIGAME.EXE" \
    --add "WTERM.EXE=tools/cp86img/apps/WTERM.EXE" \
    --strip-installer
```

When it does not fit, the tool computes that **before** writing and reports the shortfall:

```
error: build would not fit: short by 57 cluster(s) (58368 bytes).
       Add more --drop names or start from a larger DOS image.
```

(Note: many 360K images contain **leaked clusters** - marked in use by the FAT but owned by no
file. `info` reports them, and the tool counts free space from the actual FAT state, so the
number can be lower than what `dir` shows.)

## 6. Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| Black screen, never reaches `A:\>` | Image size not in the geometry table, so it was treated as a hard disk | `cp86img.py info` - check that `emulator geom` says `supported` |
| `Non-System disk or disk error` | Boot sector is not DOS's, or `IO.SYS`/`MSDOS.SYS` are missing from the root | Base the image on a real DOS boot disk |
| `Invalid drive specification` on C: | The SD card is not mounted (no card, or a wedged card) | Long-press power to fully cycle; try another card |
| Characters missing, `: > *` cannot be typed | Shift-symbol bug in older firmware | Use this repository's firmware (fixed) |
| `Bad command or file name` although the file is there | Name was truncated (>8.3), or a same-named `.COM` shadows the `.EXE` | Use 8.3 names; never keep both `.COM` and `.EXE` for one program |
| File is on the disk but DOS reports read errors | Only one FAT copy was updated | Use this tool (it writes and verifies both) |
| `dir` free-space figure differs from your own count | Leaked clusters | Inspect `leaked` in `cp86img.py info` |

## 7. Writing your own DOS programs

All sources are in `apps/src/`; build them with the free Digital Mars compiler
(16-bit, small model):

```sh
dmc -msd -w minigame.cpp -o MINIGAME.EXE
dmc -msd -w te.c         -o TE.EXE
dmc -msd -w mario.c      -o MARIO.EXE
```

Lessons this project learned the hard way:

- **Do not use `-mt`**: Digital Mars ships no tiny-model library and the link dies with
  `Error 2: File Not Found ct.obj`.
- Objects and arrays are limited to 64 KB (small model); the stack is a few KB, so keep big
  arrays off it.
- Write video memory directly: `unsigned char far *v = (unsigned char far *)0xB8000000UL;`
  In 40-column mode a row is 80 bytes (`off = (row * 80 + col) << 1`) and a cell is two bytes
  (character then attribute).
- `INT 16h` returns **AH = scan code, AL = ASCII**; compare AL for characters.
- Test for a pending key with
  `*((unsigned far *)0x0040001AUL) != *((unsigned far *)0x0040001CUL)` and then read it with
  `int86(0x16, ...)`. `kbhit()` goes through INT 21h and is slower.
- Timing: `*(unsigned long far *)0x0040006CUL` (18.2 ticks per second).
- Hide the hardware cursor with `r.x.cx = 0x2000; r.h.ah = 0x01; int86(0x10, &r, &r);`
- On exit, restore video mode 3 and re-enable the cursor, or the DOS prompt comes back black.
