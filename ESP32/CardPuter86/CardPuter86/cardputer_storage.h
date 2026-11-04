#ifndef CARDPUTER_STORAGE_H
#define CARDPUTER_STORAGE_H

#include <Arduino.h>

#define CARDPUTER_IMAGE_PATH_LEN 96
#define CARDPUTER_DRIVE_COUNT 4

enum CardputerStorageType : uint8_t {
    CARDPUTER_STORAGE_NONE = 0,
    CARDPUTER_STORAGE_FLASH,
    CARDPUTER_STORAGE_SD,
    /* Raw sector access to the whole SD card, exposed to the guest as a hard
       disk (C:, 0x80) instead of an image file: the architecture the user asked
       for is "flash = system disk, SD card = external hard disk". No FatFS and
       no VFS is involved, so this costs ~1.2 KB of static memory and can be
       mounted during boot. */
    CARDPUTER_STORAGE_SD_RAW
};

struct CardputerDiskImage {
    bool mounted;
    CardputerStorageType storage;
    uint8_t drive;
    uint32_t size;
    uint16_t cylinders;
    uint8_t sectors;
    uint8_t heads;
    char path[CARDPUTER_IMAGE_PATH_LEN];
};

extern CardputerDiskImage gb_disk_image;
extern CardputerDiskImage gb_disk_drives[CARDPUTER_DRIVE_COUNT];

bool cardputer_storage_init_and_select(void);
bool cardputer_storage_show_settings_menu(bool allow_usb_disk);
void cardputer_storage_show_boot_status(void);
bool cardputer_storage_read_sector(unsigned long lba, unsigned char *buffer);
bool cardputer_storage_write_sector(unsigned long lba, const unsigned char *buffer);
bool cardputer_storage_read_sector(uint8_t drive, unsigned long lba, unsigned char *buffer);
bool cardputer_storage_write_sector(uint8_t drive, unsigned long lba, const unsigned char *buffer);
bool cardputer_storage_drive_geometry(uint8_t drive, uint16_t *cylinders,
                                      uint8_t *heads, uint8_t *sectors);
uint8_t cardputer_storage_hard_drive_count(void);
bool cardputer_storage_show_mount_menu(void);
bool cardputer_storage_second_drive_present(void);

/* ---- 多系统镜像：选择下次启动哪个系统 --------------------------------
   flash 上可以放多张可引导 IMG（内置的 1.44MB MS-DOS 5.0 = cardputer86.img，
   外加 360KB 的 IBM PC-DOS 3.x = dos3.img）。设置菜单（Fn+空格 → More
   settings → Boot system）写入 NVS 里的偏好，它的优先级高于「上次启动的镜像」，
   所以选完之后断电/重启都还是同一个系统，直到用户再改。 */
uint8_t cardputer_storage_boot_image_count(void);
/* "[F] cardputer86.img" / "[S] dos.img"：静态缓冲区，取到后请立刻拷走。 */
const char *cardputer_storage_boot_image_label(uint8_t index);
/* 只有文件名（给设置条目标题用）。越界返回 "none"。 */
const char *cardputer_storage_boot_image_name(uint8_t index);
/* 当前偏好在 boot_images[] 里的下标；没有偏好时回落到正在挂载的那张。 */
uint8_t cardputer_storage_boot_image_pref(void);
/* 记住选择；apply_now = true 时立刻换盘并重启 guest（等价于断电重开）。 */
bool cardputer_storage_set_boot_image(uint8_t index, bool apply_now);
#ifdef use_lib_log_serial
const char *cardputer_storage_sd_diag_text(void);
void cardputer_storage_debug_set_second_drive(bool enable);
void cardputer_storage_debug_drop_sd(void);
/* external hard disk (raw SD) helpers: wipe the MBR area, dump the partition
   table.  Both act on the card directly, independent of the guest's FDISK. */
bool cardputer_storage_raw_wipe(uint32_t sectors);
void cardputer_storage_raw_dump_mbr(void);
bool cardputer_storage_debug_retry_sd(void);
/* read-path counters + 'p' benchmark: splits "dir c: is slow" into card time
   versus guest time instead of guessing. */
void cardputer_storage_sd_io_stats(uint32_t *ops, uint32_t *op_us, uint32_t *reads,
                                   uint32_t *hits);
void cardputer_storage_debug_bench_sd(void);
#endif

#endif
