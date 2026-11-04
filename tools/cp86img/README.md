# cp86img.py — CardPuter86 image packer

**中文** | [English](#english)

把**你自己的** DOS 启动软盘镜像变成一张 CardPuter86 系统盘：删安装负载、注入程序、
写好 `AUTOEXEC.BAT`，最后**逐字节回读验证**。纯 Python 标准库，单文件，无依赖。

```sh
python3 cp86img.py build --base mydos.img --out card.img --strip-installer
```

关于镜像本身的硬性要求（尺寸、FAT12、引导扇区）、怎么把镜像送进设备、故障速查表，
见 [../../docs/IMAGE.md](../../docs/IMAGE.md)。

---

## 安装

不需要安装。把 `cp86img.py` 拷到任何地方，用 Python 3.8+ 直接跑：

```sh
python3 tools/cp86img/cp86img.py --version
python3 tools/cp86img/cp86img.py --help
```

`apps/` 目录要和 `cp86img.py` 放在一起（`build` 的默认注入源），也可以在别处用
`--apps <目录>` 指过去。

## 命令

| 命令 | 作用 |
| --- | --- |
| `info IMG` | BPB 几何、各分区参数、空闲簇/字节、根目录剩余槽位、FAT 两份是否一致、泄漏簇/交叉链接簇、簇链有无结尾标记 |
| `list IMG` | 列根目录，并标出每个文件的簇链是否连续 |
| `build --base X --out Y` | 生成系统盘（见下） |
| `add IMG NAME FILE` | 加/替换单个文件（`--force` 才允许替换） |
| `del IMG NAME...` | 删文件并释放簇链（同时把目录项标 0xE5） |
| `get IMG NAME OUT` | 取出文件 |
| `verify IMG NAME FILE` | 镜像内文件与本地文件逐字节比对，不一致时打印首个差异偏移 |

退出码：`0` 成功，`1` 失败（错误信息在 stderr）。

## build 选项

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `--base` | 必填 | 你的 DOS 启动镜像，**永远不被修改** |
| `--out` | 必填 | 输出镜像 |
| `--apps` | `./apps` | 注入目录；顶层普通文件都会进盘，源码后缀和 `src/` 会被跳过 |
| `--no-apps` | off | 跳过注入目录（显式 `--add` 仍然生效） |
| `--add NAME=FILE` | — | 额外注入，可重复 |
| `--drop NAME[,NAME...]` | — | 删除指定文件，可重复 |
| `--strip-installer` | off | 删除 DOS 零售安装负载：`SETUP.EXE` `SETUP.INI` `DOS50.ZIP` `PKUNZIP.EXE` `WINA20.386` |
| `--path` | `A:\;C:\;C:\CPP86` | 写进 `AUTOEXEC.BAT` 的 PATH；传 `""` 不写 |
| `--extra-bat LINE` | — | 追加 `AUTOEXEC.BAT` 行，可重复 |
| `--autobat-file FILE` | — | 整个文件当 `AUTOEXEC.BAT` |
| `--no-autobat` | off | 完全不碰 `AUTOEXEC.BAT` |
| `--min-free BYTES` | 0 | 剩余空间低于此值即失败 |
| `--mtime-now` | off | 目录项用当前时间（默认固定 1991-06-15 12:00，保证可复现） |
| `--force` | off | 覆盖已存在的 `--out` |
| `--in-place` | off | 允许 `--out` 等于 `--base` |

## 为什么这样实现（每条都对应一个真事故）

| 设计 | 事故 |
| --- | --- |
| 按 BPB 解析几何，绝不假设 360K/1.44M | 写死尺寸的工具会静默写坏其它尺寸的盘 |
| 名字不合法就**报错**，不截断 | 早先的脚本把 `CPPSCREEN.EXE` 截成 `CPPSCREE.EXE`，DOS 回 `Bad command or file name` |
| **所有** FAT 副本一起写，收尾校验逐字节一致 | 第二份 FAT 过期是 FAT12 最典型的损坏，DOS 会报 `General failure reading drive A` |
| 扫 FAT 里为 0 的项来分配，不靠遍历根目录 | 遍历根目录会漏掉"已删除文件仍占着的簇"以及子目录占用的簇 |
| **先算空间再写第一个字节** | 失败的构建不能留下半成品镜像 |
| 写完**回读每个文件**比对 | "看起来没问题"不是构建结果 |
| 尺寸不在几何表里直接拒绝 | 不在表里的尺寸会被模拟器当硬盘，DOS 读不出盘：黑屏，且没有任何提示 |

## 输出示例

```
== base ==
  mydos.img: 1474560 bytes, 1.44M, 7680 bytes free
== plan ==
  drop   DOS50.ZIP     1081632 bytes
  ...
  put    MINIGAME.EXE    40264 bytes  <- MINIGAME.EXE
  write  AUTOEXEC.BAT       47 bytes  (prompt/path)
  free after build: 923648 bytes (1804 clusters)
== verify ==
  MINIGAME.EXE OK  (40264 bytes)
  AUTOEXEC.BAT OK  (47 bytes)
  FAT copies    OK  (all 2 in sync)
== done ==
  wrote card.img: 1474560 bytes, 25 files, 923648 bytes free
  sha256 141eeb7a...
```

`free after build` 是按**簇**算的，和最终 `dir` 显示的空闲字节一致（实测分毫不差）。

---

# English

Turn **your own** DOS boot floppy image into a CardPuter86 system disk: drop the installer
payload, inject the programs, write a sane `AUTOEXEC.BAT`, and then **prove every byte** by
reading it back. Pure Python standard library, one file, no dependencies.

```sh
python3 cp86img.py build --base mydos.img --out card.img --strip-installer
```

For the emulator's hard requirements (size, FAT12, boot sector), how to get an image onto the
device, and the troubleshooting table, see [../../docs/IMAGE.md](../../docs/IMAGE.md).

## No install

Copy `cp86img.py` anywhere and run it with Python 3.8+. Keep the `apps/` directory next to it
(that is `build`'s default injection source), or point at another directory with `--apps`.

## Commands

| Command | Purpose |
| --- | --- |
| `info IMG` | BPB geometry, free clusters/bytes, free root slots, FAT copy consistency, leaked/cross-linked clusters, missing end-of-chain markers |
| `list IMG` | root directory listing that also flags whether each cluster chain is contiguous |
| `build --base X --out Y` | build a system disk (options below) |
| `add IMG NAME FILE` | add or replace one file (`--force` required to replace) |
| `del IMG NAME...` | delete files, free their chains, mark the entries `0xE5` |
| `get IMG NAME OUT` | extract a file |
| `verify IMG NAME FILE` | byte-compare an image file against a local file; prints the first differing offset |

Exit code: `0` success, `1` failure (message on stderr).

## Why it is built this way

| Design rule | The accident behind it |
| --- | --- |
| Parse the BPB, never assume 360K/1.44M | a tool with a hardcoded size silently corrupts every other size |
| **Reject** invalid names instead of truncating | an earlier script cut `CPPSCREEN.EXE` down to `CPPSCREE.EXE`; DOS replied `Bad command or file name` |
| Write **every** FAT copy and verify they agree | a stale second FAT is the classic FAT12 corruption; DOS reports `General failure reading drive A` |
| Allocate by scanning the FAT for zero entries, not by walking the root directory | walking the root misses clusters a deleted file still owns, and clusters owned by subdirectories |
| Compute free space **before** writing a single byte | a failed build must not leave a half-written image |
| Read every added file back and compare | "it looked fine" is not a build result |
| Refuse sizes outside the geometry table | an unlisted size is treated as a hard disk; DOS cannot read it - black screen, no diagnostic |
