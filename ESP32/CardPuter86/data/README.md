# data/ — 系统盘镜像 / system disk image

**中文** | **English**

`pio run -t buildfs -e cardputer86-lean` 会把这个目录里的**所有文件**打进 `fatfs.bin`
（5,632,000 字节，写进 flash 的 `disk` 分区 `0x2A0000`，FAT 从 `0x2A1000` 开始）。

## `cardputer86.img`

**这是上游 CardPuter86 随仓库发布的那张 360KB PC-DOS 3.x 镜像，原样未改。**
本仓库没有新增任何 DOS 系统文件（原因见 [../NOTICE.md](../NOTICE.md)）。

它可以直接构建、直接开机，空间很小（约 181KB 空闲）。想换成 1.44MB 的
MS-DOS 5.0 / 6.22，用你自己的镜像跑一遍打包工具：

```sh
python3 ../tools/cp86img/cp86img.py build \
    --base ~/my-msdos5.img \
    --out  cardputer86.img \
    --strip-installer
```

## 多系统启动

这个目录下的每个 `*.img` 都会被打进 fatfs。开机后 `Fn` + 空格 → Settings →
More settings → **Boot system** 选下次启动哪一张。例如同时放：

```
cardputer86.img   PC-DOS 3.x（360K，快、省空间）
dos5.img          MS-DOS 5.0（1.44M，工具全）
```

注意：disk 分区一共 5.4MB，放两张 1.44MB + 一张 360KB 完全够。

## ⚠️ 别把带版权的内容提交上来

如果你在自己 fork 里放入了 MS-DOS / PC-DOS 的镜像，**不要 push 到公开仓库**。
在 `.gitignore` 里加一行排除它，或者干脆只在本地放。

---

# English

`pio run -t buildfs` packs **every file in this directory** into `fatfs.bin` (5,632,000 bytes),
which is written to the flash `disk` partition at `0x2A0000`; the FAT starts at `0x2A1000`.

## `cardputer86.img`

**This is the 360 KB PC-DOS 3.x image published by upstream CardPuter86, unmodified.**
This fork adds no DOS system file of its own (see [../NOTICE.md](../NOTICE.md)).

It builds and boots as-is, but has only about 181 KB free. To use a 1.44 MB MS-DOS 5.0 / 6.22
disk you own, run the packer:

```sh
python3 ../tools/cp86img/cp86img.py build \
    --base ~/my-msdos5.img \
    --out  cardputer86.img \
    --strip-installer
```

## Multi-boot

Every `*.img` here is baked into fatfs. On the device: `Fn` + Space -> Settings ->
More settings -> **Boot system** picks the image for the next boot. The 5.4 MB partition holds
two 1.44 MB images plus a 360 KB one comfortably.

## Do not commit copyrighted images

If you drop an MS-DOS / PC-DOS image into your own fork, **do not push it to a public
repository**. Add it to `.gitignore` or keep it local only.
