# apps/ — DOS programs bundled with this fork

**中文** | **English**

`cp86img.py build` 默认把这个目录下的顶层文件注入系统盘。`src/` 放源码，不会被注入。

| 文件 | 大小 | 说明 |
| --- | --- | --- |
| `MINIGAME.EXE` | 40,264 | 七合一：贪吃蛇 / 扫雷 / 五子棋 / 俄罗斯方块 / 吃豆人 / 小恐龙 / 走迷宫 |
| `MARIO.EXE` | 16,236 | 40×16 文本版马里奥，120 列滚动世界 |
| `TE.EXE` | 45,216 | 文本编辑器（直写显存、软换行、`^O` 存 `^X` 退） |
| `WTERM.EXE` | 13,278 | COM1 终端，配 Hayes / WiFi modem |
| `ASM.EXE` | 44,638 | 8086 汇编器 |
| `DIAG.COM` | 13,748 | 启动诊断（A20 / 定时器 / 视频 / 中断表） |
| `SNAKE.EXE` | 17,232 | 贪吃蛇（早期单游戏版） |
| `G2048.EXE` | 15,262 | 2048（早期单游戏版） |
| `MINE.EXE` | 16,136 | 扫雷（早期单游戏版，显存直写） |
| `KEYDBG.EXE` | 12,848 | 键码查看器（打印 AX / al / ah） |

源码在 `src/`：`minigame.cpp`、`mario.c`、`te.c`、`wterm.c`、`asm.c`、`diag.c`、
`snake.c`、`game2048.c`、`mine.cpp`、`keydbg.c`、`gomoku.h`。

## 重新编译

用 Digital Mars C/C++（免费）的 16 位 small 模型：

```sh
dmc -msd -w minigame.cpp -o MINIGAME.EXE
dmc -msd -w te.c         -o TE.EXE
dmc -msd -w mario.c      -o MARIO.EXE
dmc -msd -w wterm.c      -o WTERM.EXE
```

- **不要用 `-mt`**：Digital Mars 不带 tiny 模型库，链接会死在
  `Error 2: File Not Found ct.obj`。
- 文件名必须 **8.3**（主名 ≤8 字符）。`cp86img.py` 会拒绝超长名字而不是截断它。
- DOS 里 **`.COM` 优先于 `.EXE`**：盘上别同时留同名的 `.COM` 和 `.EXE`。

## English

`cp86img.py build` injects the top-level files of this directory into the system disk; `src/`
holds the sources and is never injected. Rebuild with the free Digital Mars C/C++ compiler in
16-bit small model (`dmc -msd -w`). Do **not** use `-mt` (no tiny-model library ships with it,
and the link dies with `Error 2: File Not Found ct.obj`). Names must be 8.3, and DOS prefers a
`.COM` over a same-named `.EXE`.

These programs are new in this fork and are covered by the repository's GPL licence. They are
ordinary DOS programs: they also run on real hardware and in emulators such as DOSBox or QEMU,
which is how they were verified (QEMU with real SeaBIOS + real MS-DOS 5.0).
