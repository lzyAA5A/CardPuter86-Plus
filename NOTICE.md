# Provenance / 来源与致谢

This repository is `CardPuter86-Plus`, a derivative work. **It is not the original project.**
If you are looking for the upstream emulator, go to
<https://github.com/Clearwavelet/CardPuter86>.

## Chain of authorship

| Layer | Project | Author / maintainer | Licence |
| --- | --- | --- | --- |
| 5 (this repo) | **CardPuter86-Plus** | see the git history of this repository | GPL |
| 4 | [CardPuter86](https://github.com/Clearwavelet/CardPuter86) | Clearwave | GPL (see `LICENSE`) |
| 3 | ESP32TinyFake86 | port of Fake86 to ESP32 | GPL |
| 2 | [Fake86](https://github.com/mikechambers/fake86) | Mike Chambers | GPL |
| 1 | 8086 emulation concepts, BIOS/BASIC ROM images | as documented by upstream | see upstream notes |

`LICENSE` is the upstream GNU General Public License file and is **unmodified**. Every file
carried over from upstream keeps its original copyright and licence; the GPL requires that
this derivative also be distributed under the GPL, which it is.

## What this fork adds (and therefore holds new copyright on)

- Modifications to `ESP32/CardPuter86/CardPuter86/*` - see the diff against upstream tag
  `v0.5.0`: 25 files, +3147 / -318 lines.
- New files `ESP32/CardPuter86/CardPuter86/cardputer_btkbd.cpp` and `cardputer_btkbd.h`.
- `tools/cp86img/cp86img.py` (new, standalone image packing tool).
- The DOS programs and their sources under `tools/cp86img/apps/` and `apps/src/`
  (`minigame.cpp`, `mario.c`, `te.c`, `wterm.c`, `asm.c`, `diag.c`, `snake.c`,
  `game2048.c`, `mine.cpp`, `keydbg.c`, `gomoku.h`).
- The documentation written for this fork (`README.md`, `README.en.md`, `docs/*`,
  this file).

## Third-party material included

| Item | Where | Note |
| --- | --- | --- |
| `data/cardputer86.img` | 360 KB PC-DOS 3.x floppy image | **Exactly the file upstream publishes**, byte for byte. This fork does not add any DOS system file of its own. |
| `tools/ima2h/input/**` | `compaqdos211.img`, `digger.com`, font data | Carried over from upstream unchanged. |
| `preview/*.png`, `preview/*.svg`, `preview/*.zip` | screenshots, cover art, PLL configs | Carried over from upstream. |
| `docs/README-upstream*.md` | upstream READMEs (en / zh-CN / es) | Kept verbatim for attribution. |

The DOS programs under `tools/cp86img/apps/` were compiled with Digital Mars C/C++
(`dmc -msd`). Digital Mars distributes its compiler free of charge, including for commercial
use; only the compiled output is redistributed here.

## Nothing here is a Microsoft or IBM distribution

No MS-DOS, PC-DOS, or IBM/Microsoft system file was added by this fork. If you want to run
MS-DOS 5.0 or 6.22 on this emulator, supply your own legally obtained image - see
[docs/IMAGE.md](docs/IMAGE.md).

## If you believe something here is misattributed

Open an issue with the file path and the claim, and it will be corrected or removed.

---

# 中文摘要

- 本仓库是 `CardPuter86-Plus`，**派生作品**，不是原始项目。上游在
  <https://github.com/Clearwavelet/CardPuter86>。
- 许可链：Fake86（Mike Chambers）→ ESP32TinyFake86 → CardPuter86（Clearwave）→ 本仓库，
  全程 GPL。`LICENSE` 是上游原文件，未改动。
- 本仓库新增（因此持有新版权）：对 `ESP32/CardPuter86/CardPuter86/*` 的修改
  （相对上游 `v0.5.0`：25 文件、+3147/−318 行）、新文件 `cardputer_btkbd.cpp/.h`、
  `tools/cp86img/cp86img.py`、`tools/cp86img/apps/` 下的十个 DOS 程序及其源码、
  以及本仓库撰写的中英文文档。
- 第三方内容：`data/cardputer86.img` 是**上游原样发布的那张 360KB PC-DOS 3.x 镜像**，
  本仓库没有新增任何 DOS 系统文件；`tools/ima2h/input/**`、`preview/**`、
  `docs/README-upstream*.md` 均沿用上游原文。
- 若你认为某处归属有误，请开 issue 指出文件路径，会据实修正或删除。
