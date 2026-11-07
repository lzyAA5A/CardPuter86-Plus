#include "cardputer_storage.h"
#include "hardware.h"
#include "guest_memory.h"
#include "cardputer_cpu.h"
#include "cardputer_input.h"
#include "cardputer_rtc.h"
#include "cardputer_settings.h"
#include "cardputer_modem.h"
#include "cardputer_btkbd.h"
#include "gbGlobals.h"
#include <Arduino.h>
#include <M5Cardputer.h>
#include <SD.h>
#include <USB.h>
#include <USBCDC.h>
#include <USBMSC.h>
#include <dirent.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

extern "C" {
#include "esp_partition.h"#include "esp_vfs_fat.h"
#include "nvs.h"
#include "wear_levelling.h"
}

static const char *FLASH_MOUNT_POINT = "/flash";
static const char *FLASH_PARTITION_LABEL = "disk";
static const uint8_t MAX_BOOT_IMAGES = 20;
static const uint8_t MENU_CANCEL = 0xFF;
static const uint8_t DRIVE_IDS[CARDPUTER_DRIVE_COUNT] = {0x00, 0x01, 0x80, 0x81};

struct BootImageEntry {
    CardputerStorageType storage;
    uint32_t size;
    char path[CARDPUTER_IMAGE_PATH_LEN];
};

static SPIClass sd_spi;
static bool sd_available = false;
static bool flash_available = false;
static bool sd_detected = false;
static bool flash_detected = false;
static wl_handle_t flash_wl_handle = WL_INVALID_HANDLE;
static FILE *flash_image_files[CARDPUTER_DRIVE_COUNT] = {};
static FILE *sd_image_files[CARDPUTER_DRIVE_COUNT];
static BootImageEntry boot_images[MAX_BOOT_IMAGES];
static uint8_t boot_image_count = 0;

// Construct before TinyUSB starts so MSC is part of the USB descriptor. In
// normal boot it remains present with no media; USB mode makes it ready.
static USBMSC usb_msc;
static CardputerStorageType usb_storage = CARDPUTER_STORAGE_NONE;
static uint8_t sd_usb_sector[512];
static wl_handle_t usb_flash_wl = WL_INVALID_HANDLE;
static uint8_t *usb_flash_cache = nullptr;
static size_t usb_flash_cache_address = SIZE_MAX;
static size_t usb_flash_cache_size = 0;
static bool usb_flash_cache_dirty = false;
static uint16_t usb_block_size = 512;
static volatile bool sd_mount_finished = false;
static volatile bool sd_mount_result = false;
static TaskHandle_t sd_mount_task_handle = nullptr;

CardputerDiskImage gb_disk_image = {};
CardputerDiskImage gb_disk_drives[CARDPUTER_DRIVE_COUNT] = {};
static USBCDC usb_cdc;

static void fit_status_text(char *buffer, size_t buffer_size, const char *text,
                            int max_chars) {
    if (!buffer || buffer_size == 0) return;
    if (!text) text = "";
    if (max_chars < 4) max_chars = 4;
    snprintf(buffer, buffer_size, "%s", text);
    const size_t len = strlen(buffer);
    if ((int)len <= max_chars) return;
    buffer[max_chars - 3] = '.';
    buffer[max_chars - 2] = '.';
    buffer[max_chars - 1] = '.';
    buffer[max_chars] = '\0';
}

static void draw_cardputer86_logo(int center_x, int center_y) {
    auto &display = M5Cardputer.Display;
    const int logo_w = 132;
    const int logo_h = 56;
    const int x = center_x - logo_w / 2;
    const int y = center_y - logo_h / 2;

    display.fillRect(x + 4, y + 4, logo_w, logo_h, TFT_DARKGREY);
    display.fillRect(x, y, logo_w, logo_h, TFT_BLACK);
    display.drawRect(x, y, logo_w, logo_h, TFT_CYAN);
    display.drawRect(x + 3, y + 3, logo_w - 6, logo_h - 6, TFT_BLUE);

    display.setTextSize(2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setCursor(x + 16, y + 12);
    display.print("Card");
    display.setTextColor(TFT_ORANGE, TFT_BLACK);
    display.print("86");

    display.setTextSize(1);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setCursor(x + 18, y + 34);
    display.print("PC/XT ON CARDPUTER");

    for (int i = 0; i < 6; ++i) {
        const int px = x + 8 + i * 5;
        display.fillRect(px, y + 47, 3, 3, (i & 1) ? TFT_ORANGE : TFT_CYAN);
        display.fillRect(x + logo_w - 11 - i * 5, y + 47, 3, 3,
                         (i & 1) ? TFT_CYAN : TFT_ORANGE);
    }
}

static void show_probe_status(const char *status, const char *hint = nullptr) {
    auto &display = M5Cardputer.Display;
    display.fillScreen(TFT_BLACK);
    display.fillRect(0, 0, display.width(), 12, TFT_BLUE);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE, TFT_BLUE);
    display.setCursor(4, 2);
    display.print("CardPuter86 POST");

    draw_cardputer86_logo(display.width() / 2, 62);

    display.fillRect(0, display.height() - 19, display.width(), 19, TFT_NAVY);
    display.drawFastHLine(0, display.height() - 20, display.width(), TFT_BLUE);

    char line[48];
    fit_status_text(line, sizeof(line), status, (display.width() - 8) / 6);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.setCursor(4, display.height() - 16);
    display.print(line);

    if (hint && hint[0]) {
        fit_status_text(line, sizeof(line), hint, (display.width() - 8) / 6);
        display.setTextColor(TFT_YELLOW, TFT_NAVY);
        display.setCursor(4, display.height() - 8);
        display.print(line);
    }
}

static bool is_image_name(const char *name) {
    const size_t len = strlen(name);
    if (len <= 4) return false;
    const char *extension = name + len - 4;
    return strcasecmp(extension, ".img") == 0 ||
           strcasecmp(extension, ".dsk") == 0;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int8_t drive_slot(uint8_t drive) {
    for (uint8_t i = 0; i < CARDPUTER_DRIVE_COUNT; i++) {
        if (DRIVE_IDS[i] == drive) return i;
    }
    return -1;
}

static const char *drive_label(uint8_t drive) {
    switch (drive) {
        case 0x00: return "A:";
        case 0x01: return "B:";
        case 0x80: return "C:";
        case 0x81: return "D:";
        default: return "?:";
    }
}

static void set_disk_geometry(CardputerDiskImage &image, uint8_t drive,
                              uint32_t size) {
    image.size = size;
    image.drive = drive;
    image.heads = 2;
    image.sectors = 18;
    image.cylinders = 80;

    switch (size) {
        case 163840: image.heads = 1; image.sectors = 8; image.cylinders = 40; break;
        case 184320: image.heads = 1; image.sectors = 9; image.cylinders = 40; break;
        case 327680: image.heads = 2; image.sectors = 8; image.cylinders = 40; break;
        case 368640: image.heads = 2; image.sectors = 9; image.cylinders = 40; break;
        case 737280: image.heads = 2; image.sectors = 9; image.cylinders = 80; break;
        case 1228800: image.heads = 2; image.sectors = 15; image.cylinders = 80; break;
        case 1474560: image.heads = 2; image.sectors = 18; image.cylinders = 80; break;
        case 2949120: image.heads = 2; image.sectors = 36; image.cylinders = 80; break;
        default:
            image.heads = 16;
            image.sectors = 63;
            image.cylinders = size / (512UL * 16UL * 63UL);
            if (image.cylinders == 0) image.cylinders = 1;
            if (image.cylinders > 1024) image.cylinders = 1024;
            break;
    }
}

static uint8_t default_drive_for_image(uint32_t size) {
    return size > 2949120 ? 0x80 : 0x00;
}

static bool nvs_read_last_image(CardputerStorageType *storage, char *path,
                                size_t path_size) {
    if (!storage || !path || path_size == 0) return false;
    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t saved_storage = 0;
    size_t len = path_size;
    const bool ok = nvs_get_u8(handle, "last_store", &saved_storage) == ESP_OK &&
                    nvs_get_str(handle, "last_img", path, &len) == ESP_OK &&
                    (saved_storage == CARDPUTER_STORAGE_FLASH ||
                     saved_storage == CARDPUTER_STORAGE_SD);
    nvs_close(handle);
    if (ok) *storage = (CardputerStorageType)saved_storage;
    return ok;
}

static void nvs_save_last_image(CardputerStorageType storage, const char *path) {
    nvs_handle_t handle;
    if (!path || nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "last_store", storage);
    nvs_set_str(handle, "last_img", path);
    nvs_commit(handle);
    nvs_close(handle);
}

/* 「下次启动哪个系统」的用户选择（设置菜单写入），与 last_img 分开存：
   last_img 是「上次实际启动的」，每次启动都会被覆写，没法当设置用。 */
static bool nvs_read_boot_pref(CardputerStorageType *storage, char *path,
                               size_t path_size) {
    if (!storage || !path || path_size == 0) return false;
    nvs_handle_t handle;
    if (nvs_open("cardputer86", NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t saved_storage = 0;
    size_t len = path_size;
    const bool ok = nvs_get_u8(handle, "pref_store", &saved_storage) == ESP_OK &&
                    nvs_get_str(handle, "pref_img", path, &len) == ESP_OK &&
                    (saved_storage == CARDPUTER_STORAGE_FLASH ||
                     saved_storage == CARDPUTER_STORAGE_SD);
    nvs_close(handle);
    if (ok) *storage = (CardputerStorageType)saved_storage;
    return ok;
}

static void nvs_save_boot_pref(CardputerStorageType storage, const char *path) {
    nvs_handle_t handle;
    if (!path || nvs_open("cardputer86", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "pref_store", storage);
    nvs_set_str(handle, "pref_img", path);
    nvs_commit(handle);
    nvs_close(handle);
}

static bool sd_card_responds(void) {
    // Cardputer has no card-detect switch. Keep MISO high when the socket is
    // empty; otherwise a floating line can look like CMD0's 0x01 response.
    pinMode(SD_MISO, INPUT_PULLUP);
    sd_spi.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    sd_spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    for (uint8_t i = 0; i < 10; i++) sd_spi.transfer(0xFF);

    digitalWrite(SD_CS, LOW);
    sd_spi.transfer(0x40); // CMD0
    sd_spi.transfer(0x00);
    sd_spi.transfer(0x00);
    sd_spi.transfer(0x00);
    sd_spi.transfer(0x00);
    sd_spi.transfer(0x95);
    uint8_t cmd0_response = 0xFF;
    for (uint8_t i = 0; i < 10; i++) {
        cmd0_response = sd_spi.transfer(0xFF);
        if ((cmd0_response & 0x80) == 0) break;
    }
    digitalWrite(SD_CS, HIGH);
    sd_spi.transfer(0xFF);

    bool detected = false;
    if (cmd0_response == 0x01) {
        digitalWrite(SD_CS, LOW);
        sd_spi.transfer(0x48); // CMD8: SEND_IF_COND
        sd_spi.transfer(0x00);
        sd_spi.transfer(0x00);
        sd_spi.transfer(0x01);
        sd_spi.transfer(0xAA);
        sd_spi.transfer(0x87);
        uint8_t cmd8_response = 0xFF;
        for (uint8_t i = 0; i < 10; i++) {
            cmd8_response = sd_spi.transfer(0xFF);
            if ((cmd8_response & 0x80) == 0) break;
        }
        uint8_t r7[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        if (cmd8_response == 0x01) {
            for (uint8_t i = 0; i < 4; i++) r7[i] = sd_spi.transfer(0xFF);
            detected = r7[2] == 0x01 && r7[3] == 0xAA;
        }
        digitalWrite(SD_CS, HIGH);
        sd_spi.transfer(0xFF);
    }

    digitalWrite(SD_CS, HIGH);
    sd_spi.endTransaction();
    return detected;
}

#ifdef use_lib_log_serial
/* Hand-rolled SD SPI-mode init trace. arduino-esp32 2.0.14's own driver dies at
   APP_OP_COND with R1=0x05 (ILLEGAL_COMMAND) on the 32 GB card used here, so
   walk the documented sequence step by step and print every R1 to find out
   whether the card itself can be brought to ready state at all. */
static uint8_t sd_trace_cmd(const char *label, uint8_t cmd, uint32_t arg) {
    digitalWrite(SD_CS, LOW);
    sd_spi.transfer(0xFF);
    sd_spi.transfer((uint8_t)(0x40 | cmd));
    sd_spi.transfer((uint8_t)(arg >> 24));
    sd_spi.transfer((uint8_t)(arg >> 16));
    sd_spi.transfer((uint8_t)(arg >> 8));
    sd_spi.transfer((uint8_t)arg);
    sd_spi.transfer(0x01);
    uint8_t r1 = 0xFF;
    for (uint8_t i = 0; i < 16; i++) {
        r1 = sd_spi.transfer(0xFF);
        if ((r1 & 0x80) == 0) break;
    }
    Serial.printf("[sdtrace] %-12s cmd=%u arg=%08lX r1=0x%02X\n",
                  label, cmd, (unsigned long)arg, r1);
    return r1;
}

static void sd_trace_init_sequence(void) {
    pinMode(SD_MISO, INPUT_PULLUP);
    sd_spi.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    pinMode(SD_CS, OUTPUT);
    sd_spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(SD_CS, HIGH);
    for (uint8_t i = 0; i < 16; i++) sd_spi.transfer(0xFF);

    sd_trace_cmd("GO_IDLE", 0, 0);
    digitalWrite(SD_CS, HIGH);
    sd_spi.transfer(0xFF);

    uint8_t r1 = sd_trace_cmd("SEND_IF_COND", 8, 0x1AA);
    if (r1 == 0x01) {
        Serial.printf("[sdtrace]   R7 echo = %02X %02X %02X %02X\n",
                      sd_spi.transfer(0xFF), sd_spi.transfer(0xFF),
                      sd_spi.transfer(0xFF), sd_spi.transfer(0xFF));
    }
    digitalWrite(SD_CS, HIGH);
    sd_spi.transfer(0xFF);

    // CMD55 + ACMD41 must stay inside one CS session.
    for (uint8_t attempt = 0; attempt < 6; attempt++) {
        r1 = sd_trace_cmd("APP_CMD", 55, 0);
        if (r1 > 1) break;
        r1 = sd_trace_cmd("APP_OP_COND", 41, 0x40000000);
        digitalWrite(SD_CS, HIGH);
        sd_spi.transfer(0xFF);
        if (r1 == 0x00) break;
        delay(20);
    }

    sd_trace_cmd("READ_OCR", 58, 0);
    uint32_t ocr = 0;
    for (uint8_t i = 0; i < 4; i++) ocr = (ocr << 8) | sd_spi.transfer(0xFF);
    Serial.printf("[sdtrace]   OCR = %08lX (CCS=%lu)\n", (unsigned long)ocr,
                  (unsigned long)((ocr >> 30) & 1));
    digitalWrite(SD_CS, HIGH);
    sd_spi.transfer(0xFF);
    sd_spi.endTransaction();
}
#endif

/* Guest disk path counters: a silently failing sector read leaves the caller's
   buffer zeroed, which shows up as the guest executing 0x00 bytes. Shared by
   both builds, so declared outside the debug guard. */
static uint32_t disk_read_fail_count = 0;
static uint32_t disk_read_ok_count = 0;
static uint32_t disk_read_fail_lba = 0;
static uint8_t disk_read_fail_drive = 0;

#ifdef use_lib_log_serial
/* Diagnostics kept in RAM so the per-second status line can report them without
   a reboot (the Cardputer's USB CDC gives no way to force a reset from the host). */
static int32_t sd_diag_idf_rc = -1;
static int32_t sd_diag_idf_mb = -1;
static int32_t sd_diag_arduino_ok = -1;
static char sd_diag_b_path[48] = "";

const char *cardputer_storage_sd_diag_text(void) {
    static char text[224];
    snprintf(text, sizeof(text),
             "mountRc=0x%X(%s) sdMB=%ld mounted=%ld avail=%d imgs=%u "
             "r=%lu/rf=%lu heap=%lu max=%lu b=%.18s",
             (unsigned)sd_diag_idf_rc, esp_err_to_name((esp_err_t)sd_diag_idf_rc),
             (long)sd_diag_idf_mb,
             (long)sd_diag_arduino_ok, sd_available ? 1 : 0,
             boot_image_count, (unsigned long)disk_read_ok_count,
             (unsigned long)disk_read_fail_count,
             (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap(),
             sd_diag_b_path);
    return text;
}
#endif

#ifdef use_lib_log_serial
/* One-shot probe of the ESP-IDF sdspi path. arduino-esp32 2.0.14's bundled SD
   library cannot initialise the 32 GB card used here (ACMD41 alternates between
   0x01 and 0x05), so test the maintained IDF driver before refactoring. */
static bool sd_idf_probe_once(void) {
    static bool done = false;
    if (done) return false;
    done = true;

    // SPI2 is the free host here (SPI3 is owned by the display, which answers
    // sdspi with 0x108 because the pins belong to the LCD).
    spi_bus_config_t bus = {};
    bus.mosi_io_num = SD_MOSI;
    bus.miso_io_num = SD_MISO;
    bus.sclk_io_num = SD_CLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4000;
    esp_err_t bus_rc = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    Serial.printf("[idf] spi_bus_initialize(SPI2) rc=0x%X\n", (unsigned)bus_rc);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = (gpio_num_t)SD_CS;
    slot.host_id = SPI2_HOST;

    // 1) Bring the card up by hand and read the partition table / boot sector so
    //    we can tell FAT32 from exFAT instead of guessing why FatFS refuses it.
    sdspi_dev_handle_t dev_handle = 0;
    esp_err_t err = sdspi_host_init_device(&slot, &dev_handle);
    Serial.printf("[idf] sdspi_host_init_device rc=0x%X handle=%d\n", (unsigned)err,
                  (int)dev_handle);
    if (err == ESP_OK) {
        sdmmc_host_t card_host = host;
        card_host.slot = dev_handle;
        sdmmc_card_t *card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
        if (card) {
            err = sdmmc_card_init(&card_host, card);
            Serial.printf("[idf] sdmmc_card_init rc=0x%X\n", (unsigned)err);
            if (err == ESP_OK) {
                Serial.printf("[idf] card is_mmc=%d cap=%lu ss=%lu\n",
                              card->is_mmc ? 1 : 0,
                              (unsigned long)card->csd.capacity,
                              (unsigned long)card->csd.sector_size);
                uint8_t *sector = (uint8_t *)malloc(512);
                if (sector) {
                    if (sdmmc_read_sectors(card, sector, 0, 1) == ESP_OK) {
                        for (uint8_t p = 0; p < 4; p++) {
                            const uint8_t *e = sector + 446 + p * 16;
                            const uint32_t start = (uint32_t)e[8] |
                                                   ((uint32_t)e[9] << 8) |
                                                   ((uint32_t)e[10] << 16) |
                                                   ((uint32_t)e[11] << 24);
                            Serial.printf("[idf] mbr[%u] type=0x%02X lba=%lu\n", p,
                                          e[4], (unsigned long)start);
                            if (e[4] != 0 && start != 0) {
                                uint8_t *pbr = (uint8_t *)malloc(512);
                                if (pbr &&
                                    sdmmc_read_sectors(card, pbr, start, 1) == ESP_OK) {
                                    char fstype[9] = {0};
                                    memcpy(fstype, pbr + 82, 8);
                                    Serial.printf(
                                        "[idf] pbr@%lu sig=%02X%02X oem=%.8s "
                                        "fstype='%s' spc=%u rsvd=%u tot32=%lu\n",
                                        (unsigned long)start, pbr[510], pbr[511],
                                        (char *)(pbr + 3), fstype, pbr[13], pbr[14],
                                        (unsigned long)((uint32_t)pbr[32] |
                                                        ((uint32_t)pbr[33] << 8) |
                                                        ((uint32_t)pbr[34] << 16) |
                                                        ((uint32_t)pbr[35] << 24)));
                                }
                                free(pbr);
                            }
                        }
                    }
                    free(sector);
                }
            }
            free(card);
        }
    }

    // 2) Then let IDF mount it; allocation_unit_size 0 means "use the cluster
    //    size the volume already has".
    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 0;
    sdmmc_card_t *mounted_card = nullptr;
    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mount_config,
                                  &mounted_card);
    Serial.printf("[idf] mount rc=0x%X\n", (unsigned)err);
    sd_diag_idf_rc = (int32_t)err;
    if (err == ESP_OK && mounted_card) {
        sd_diag_idf_mb = (int32_t)(((unsigned long long)mounted_card->csd.capacity *
                                    mounted_card->csd.sector_size) / (1024ULL * 1024ULL));
        DIR *dir = opendir("/sdcard");
        int entries = 0;
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != nullptr && entries < 12) {
                Serial.printf("[idf]   %s\n", entry->d_name);
                entries++;
            }
            closedir(dir);
        }
        Serial.printf("[idf] mounted %ldMB, root entries listed=%d\n",
                      (long)sd_diag_idf_mb, entries);
        return true;
    }
    return false;
}
#endif

/* SD card access.
 *
 * The bundled arduino-esp32 SD library cannot initialise the 32 GB card used
 * here: its SPI-mode init fails at APP_OP_COND with R1=0x05 (ILLEGAL_COMMAND),
 * so the mount died with "f_mount failed: (3)". The maintained ESP-IDF sdspi
 * driver brings the very same card up cleanly (sdmmc_card_init rc=0), so the
 * card is mounted through IDF and accessed over the VFS with plain POSIX calls.
 *
 * Bus note: SPI3 belongs to the display stack (sdspi on SPI3 answers with
 * ESP_ERR_INVALID_RESPONSE because the pins drive the LCD), SPI2 is free.
 */
#define SD_SPI_HOST SPI2_HOST
#define SD_MOUNT_POINT "/sdcard"

static sdmmc_card_t *sd_card = nullptr;
static bool sd_bus_ready = false;

/* The SPI bus must be brought up while the heap is still large and contiguous.
   Once guest_memory has reserved its page cache the largest free block drops to
   ~14 KB, and the mount then fails with ESP_FAIL even though ~39 KB is free in
   total. So reserve the bus (DMA descriptors included) during storage init and
   only do the FatFS mount when the guest first touches B:. */
void cardputer_storage_prepare_sd_bus(void) {
    if (sd_bus_ready) return;
    spi_bus_config_t bus = {};
    bus.mosi_io_num = SD_MOSI;
    bus.miso_io_num = SD_MISO;
    bus.sclk_io_num = SD_CLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    const esp_err_t bus_rc = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
#ifdef use_lib_log_serial
    Serial.printf("[sd] bus init rc=0x%X heap=%lu max=%lu\n", (unsigned)bus_rc,
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMaxAllocHeap());
#endif
    if (bus_rc == ESP_OK || bus_rc == ESP_ERR_INVALID_STATE) sd_bus_ready = true;
}

static bool mount_sd(void) {
    if (sd_available) return true;

    cardputer_storage_prepare_sd_bus();
    if (!sd_bus_ready) return false;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = 20000;

#ifdef use_lib_log_serial
    const uint32_t heap_before = ESP.getFreeHeap();
#endif

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = (gpio_num_t)SD_CS;
    slot.host_id = SD_SPI_HOST;

    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    // esp_vfs_fat allocates FF_MAX_SS (4 KB) per open file, so keep this at the
    // minimum the code needs: one fd for the root scan, one for the B: image.
    // 5 fds cost 20 KB and the second mount (B:) failed with ESP_ERR_NO_MEM.
    mount_config.max_files = 2;
    mount_config.allocation_unit_size = 0;   // keep the volume's own cluster size

    const esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot,
                                                 &mount_config, &sd_card);
#ifdef use_lib_log_serial
    Serial.printf("[sd] mount rc=0x%X (%s) heap=%lu->%lu max=%lu\n", (unsigned)err,
                  esp_err_to_name(err), (unsigned long)heap_before,
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMaxAllocHeap());
    sd_diag_idf_rc = (int32_t)err;
#endif
    if (err != ESP_OK || !sd_card) {
        // 0xFFFFFFFF = mounted the card but FatFS rejected the volume: the
        // card is exFAT (OEM field "EXFAT", MBR type 0x07) or unformatted.
#ifdef use_lib_log_serial
        if ((uint32_t)err == 0xFFFFFFFFU) {
            Serial.printf("[sd] card mounted but volume rejected -> not FAT12/16/32\n");
        }
        sd_diag_arduino_ok = 0;
#endif
        sd_card = nullptr;
        return false;
    }

    sd_available = true;
#ifdef use_lib_log_serial
    sd_diag_idf_mb = (int32_t)(((unsigned long long)sd_card->csd.capacity *
                                sd_card->csd.sector_size) / (1024ULL * 1024ULL));
    sd_diag_arduino_ok = 1;
    Serial.printf("[sd] mounted %ldMB at %s\n", (long)sd_diag_idf_mb, SD_MOUNT_POINT);
#endif
    return true;
}

/* The IDF mount path has its own timeouts and no longer needs the helper task
   (which used vTaskDelete on a task that could be stuck mid-SPI-transaction,
   leaving the shared bus unusable). Kept as a thin wrapper for the call sites. */
static bool mount_sd_timed(void) {
    return mount_sd();
}

static bool alt_requests_sd(void) {
    show_probe_status("SD disabled by default", "Hold Alt to scan SD (1.5s)");
    const uint32_t deadline = millis() + 1500;
    uint32_t alt_pressed_at = 0;
    while ((int32_t)(deadline - millis()) > 0) {
        M5Cardputer.update();
        if (M5Cardputer.Keyboard.keysState().alt) {
            if (alt_pressed_at == 0) alt_pressed_at = millis();
            if (millis() - alt_pressed_at >= 200) return true;
        } else {
            alt_pressed_at = 0;
        }
        delay(10);
    }
    return false;
}

static void unmount_sd(void) {
    if (sd_available && sd_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, sd_card);
        sd_card = nullptr;
    }
    // The SPI bus itself is left initialised: tearing it down while the display
    // stack shares the driver made the next TFT update hang.
    sd_available = false;
}

static bool mount_flash(void) {
    // max_files is the dominant cost here too: esp_vfs_fat allocates FF_MAX_SS
    // (4 KB) per open file, so 4 fds = 16 KB. The boot path only ever has one
    // image open at a time, and the SD mount is resident as well, so keep this
    // at 2 - otherwise the internal image fails to open once SD is mounted.
    esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 4096
    };
    esp_err_t result = esp_vfs_fat_spiflash_mount(
        FLASH_MOUNT_POINT, FLASH_PARTITION_LABEL, &config, &flash_wl_handle);
#ifdef use_lib_log_serial
    Serial.printf("[flash] mount rc=0x%X heap=%lu\n", (unsigned)result,
                  (unsigned long)ESP.getFreeHeap());
#endif
    if (result != ESP_OK) {
#ifdef use_lib_log_serial
        Serial.printf("Internal IMG partition mount failed: %s\n",
                      esp_err_to_name(result));
#endif
        flash_wl_handle = WL_INVALID_HANDLE;
        return false;
    }
    flash_available = true;
    return true;
}

static void unmount_flash(void) {
    if (flash_available && flash_wl_handle != WL_INVALID_HANDLE) {
        esp_vfs_fat_spiflash_unmount(FLASH_MOUNT_POINT, flash_wl_handle);
    }
    flash_wl_handle = WL_INVALID_HANDLE;
    flash_available = false;
}

static void add_boot_image(CardputerStorageType storage, const char *path,
                           uint32_t size) {
    if (boot_image_count >= MAX_BOOT_IMAGES || size < 512 || size % 512 != 0) return;
    // SD is scanned twice in a session (boot scan, then the B: mount), so the
    // same file can be offered twice; keep the table free of duplicates.
    for (uint8_t i = 0; i < boot_image_count; i++) {
        if (boot_images[i].storage == storage &&
            strcmp(boot_images[i].path, path) == 0) {
            return;
        }
    }
    BootImageEntry &entry = boot_images[boot_image_count++];
    entry.storage = storage;
    entry.size = size;
    strncpy(entry.path, path, sizeof(entry.path) - 1);
    entry.path[sizeof(entry.path) - 1] = '\0';
}

static void scan_flash_images(void) {
    if (!flash_available) return;
    DIR *directory = opendir(FLASH_MOUNT_POINT);
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != nullptr) {
        if (!is_image_name(entry->d_name)) continue;
        char path[CARDPUTER_IMAGE_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", FLASH_MOUNT_POINT, entry->d_name);
        struct stat info;
        if (stat(path, &info) == 0 && S_ISREG(info.st_mode)) {
            add_boot_image(CARDPUTER_STORAGE_FLASH, path, info.st_size);
        }
    }
    closedir(directory);
}

static void scan_sd_images(void) {
    if (!sd_available) return;
    DIR *directory = opendir(SD_MOUNT_POINT);
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != nullptr) {
        if (!is_image_name(entry->d_name)) continue;
        char path[CARDPUTER_IMAGE_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, entry->d_name);
        struct stat info;
        if (stat(path, &info) == 0 && S_ISREG(info.st_mode)) {
            add_boot_image(CARDPUTER_STORAGE_SD, path, info.st_size);
        }
    }
    closedir(directory);
}

static void draw_menu(const char *title, const char *const *items, uint8_t count,
                      uint8_t selected, const char *footer) {
    auto &display = M5Cardputer.Display;
    display.fillScreen(TFT_BLACK);
    display.fillRect(0, 0, display.width(), 16, TFT_BLUE);
    display.setTextColor(TFT_WHITE, TFT_BLUE);
    display.setTextSize(1);
    display.setCursor(4, 4);
    if (strcmp(title, "CardPuter86 Settings") == 0) {
        display.print("Settings");
        CardputerRtcTime now = cardputer_rtc_now();
        char status_text[32];
        const int32_t battery = M5Cardputer.Power.getBatteryLevel();
        if (battery >= 0) {
            snprintf(status_text, sizeof(status_text), "%02u:%02u:%02u  BAT %ld%%",
                     now.hour, now.minute, now.second, (long)battery);
        } else {
            snprintf(status_text, sizeof(status_text), "%02u:%02u:%02u  BAT --%%",
                     now.hour, now.minute, now.second);
        }
        const int status_x = display.width() - (int)strlen(status_text) * 6 - 4;
        display.setCursor(status_x > 88 ? status_x : 88, 4);
        display.print(status_text);
    } else {
        display.print(title);
    }

    const uint8_t visible = 12;
    uint8_t first = selected >= visible ? selected - visible + 1 : 0;
    for (uint8_t row = 0; row < visible && first + row < count; row++) {
        uint8_t index = first + row;
        int y = 20 + row * 9;
        if (index == selected) {
            display.fillRect(0, y - 1, display.width(), 9, TFT_DARKGREEN);
            display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
        } else {
            display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        }
        display.setCursor(4, y);
        display.printf("%c %s", index == selected ? '>' : ' ', items[index]);
    }
    display.fillRect(0, display.height() - 10, display.width(), 10, TFT_NAVY);
    display.setTextColor(TFT_WHITE, TFT_NAVY);
    display.setCursor(3, display.height() - 9);
    display.print(footer);
}

static uint8_t choose_menu(const char *title, const char *const *items,
                           uint8_t count, uint8_t selected, bool timeout) {
    // Do not carry Ctrl/Enter/navigation keys from the previous screen into
    // this menu. A held Enter used to select item 0 (USB disk mode) when a
    // Settings submenu returned to its parent.
    do {
        M5Cardputer.update();
        if (!M5Cardputer.Keyboard.isPressed()) break;
        delay(10);
    } while (true);

    uint32_t deadline = millis() + 4000;
    bool redraw = true;

    while (true) {
        M5Cardputer.update();
        const bool up = cardputer_input_consume_char('w') ||
                        cardputer_input_consume_char('W') ||
                        cardputer_input_consume(CARDPUTER_VK_UP);
        const bool down = cardputer_input_consume_char('s') ||
                          cardputer_input_consume_char('S') ||
                          cardputer_input_consume(CARDPUTER_VK_DOWN);
        if (cardputer_input_consume(CARDPUTER_VK_ESC) ||
            cardputer_input_consume_char('`')) {
            return MENU_CANCEL;
        }
        bool enter = cardputer_input_consume(CARDPUTER_VK_ENTER);

        if (up) {
            selected = selected == 0 ? count - 1 : selected - 1;
            timeout = false;
            redraw = true;
        }
        if (down) {
            selected = (selected + 1) % count;
            timeout = false;
            redraw = true;
        }
        for (uint8_t i = 0; i < count && i < 9; i++) {
            if (cardputer_input_consume_char('1' + i)) {
                selected = i;
                enter = true;
                break;
            }
        }
        if (enter) return selected;
        if (timeout && (int32_t)(deadline - millis()) <= 0) return selected;

        if (redraw) {
            draw_menu(title, items, count, selected,
                      timeout ? "W/S/arrows Enter Esc (auto 4s)" :
                                "W/S/arrows Enter Esc");
            redraw = false;
        }
        delay(15);
    }
}

static char consume_printable_char(void) {
    for (char key = 32; key < 127; key++) {
        if (cardputer_input_consume_char(key)) return key;
    }
    return 0;
}

static bool input_text(const char *title, const char *prompt, char *out,
                       size_t out_size, bool mask) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    uint8_t pos = 0;
    do {
        M5Cardputer.update();
        if (!M5Cardputer.Keyboard.isPressed()) break;
        delay(10);
    } while (true);

    while (true) {
        auto &display = M5Cardputer.Display;
        display.fillScreen(TFT_BLACK);
        display.fillRect(0, 0, display.width(), 16, TFT_BLUE);
        display.setTextColor(TFT_WHITE, TFT_BLUE);
        display.setCursor(4, 4);
        display.print(title);
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.setCursor(6, 28);
        display.print(prompt);
        display.setCursor(6, 48);
        if (mask) {
            for (uint8_t i = 0; i < pos; i++) display.print('*');
        } else {
            display.print(out);
        }
        display.setTextColor(TFT_YELLOW, TFT_BLACK);
        display.setCursor(6, 78);
        display.print("Enter=save Backspace=del");
        display.setCursor(6, 92);
        display.print("Esc=cancel");

        M5Cardputer.update();
        if (cardputer_input_consume(CARDPUTER_VK_ESC) ||
            cardputer_input_consume_char('`')) {
            return false;
        }
        if (cardputer_input_consume(CARDPUTER_VK_BACKSPACE) && pos > 0) {
            out[--pos] = '\0';
        }
        if (cardputer_input_consume(CARDPUTER_VK_ENTER)) return true;
        const char c = consume_printable_char();
        if (c && pos + 1 < out_size) {
            out[pos++] = c;
            out[pos] = '\0';
        }
        delay(20);
    }
}

static bool show_wifi_modem_menu(void) {
    cardputer_modem_pause_wifi();
    char ssid[33] = {};
    char pass[65] = {};
    if (!input_text("WiFi SSID", "Enter SSID", ssid, sizeof(ssid), false)) {
        cardputer_modem_resume_wifi();
        return false;
    }
    if (ssid[0] == '\0') {
        show_probe_status("WiFi SSID empty", "Configuration skipped");
        delay(1200);
        cardputer_modem_resume_wifi();
        return false;
    }
    if (!input_text("WiFi password", "Empty OK for open AP",
                    pass, sizeof(pass), true)) {
        cardputer_modem_resume_wifi();
        return false;
    }

    if (!cardputer_modem_save_wifi(ssid, pass)) {
        show_probe_status("WiFi save failed", "NVS write error");
        delay(1500);
        cardputer_modem_resume_wifi();
        return false;
    }
    show_probe_status("WiFi saved", ssid);
    delay(1500);
    return true;
}

static bool close_drive(uint8_t drive) {
    const int8_t slot = drive_slot(drive);
    if (slot < 0) return false;
    if (gb_disk_drives[slot].storage == CARDPUTER_STORAGE_FLASH &&
        flash_image_files[slot]) {
        fclose(flash_image_files[slot]);
        flash_image_files[slot] = nullptr;
    }
    if (gb_disk_drives[slot].storage == CARDPUTER_STORAGE_SD &&
        sd_image_files[slot]) {
        fclose(sd_image_files[slot]);
        sd_image_files[slot] = nullptr;
    }
    gb_disk_drives[slot] = {};
    return true;
}

static bool open_image_on_drive(const BootImageEntry &entry, uint8_t drive) {
    const int8_t slot = drive_slot(drive);
    if (slot < 0) return false;
    close_drive(drive);

    if (entry.storage == CARDPUTER_STORAGE_FLASH && !flash_available &&
        !mount_flash()) {
        return false;
    }
    if (entry.storage == CARDPUTER_STORAGE_SD && !sd_available &&
        !mount_sd()) {
        return false;
    }

    CardputerDiskImage image = {};
    image.storage = entry.storage;
    strncpy(image.path, entry.path, sizeof(image.path) - 1);
    set_disk_geometry(image, drive, entry.size);

    if (entry.storage == CARDPUTER_STORAGE_FLASH) {
        flash_image_files[slot] = fopen(image.path, "r+b");
        image.mounted = flash_image_files[slot] != nullptr;
    } else {
        sd_image_files[slot] = fopen(image.path, "r+b");
        image.mounted = sd_image_files[slot] != nullptr;
    }
    if (!image.mounted) {
        close_drive(drive);
        return false;
    }
    gb_disk_drives[slot] = image;
    if (drive == 0x00 || drive == 0x80 || !gb_disk_image.mounted) {
        gb_disk_image = image;
    }
    return true;
}

/* ---- optional second floppy (B:) backed by an image on the SD card ----
   Any SD image whose size matches a known floppy geometry is mounted on B:.
   A: is left untouched, so A: can be the 1.44 MB DOS 5.0 disk from flash
   while B: comes from the SD card.  The SD volume stays mounted for the
   whole session; sector I/O for B: goes through sd_image_files[1].
   cpu.cpp patches INT 11h and BDA 0x410 so the guest sees two floppies.
   Sizes outside the floppy table are rejected on purpose: set_disk_geometry()
   would fall back to a 16-head hard disk geometry and DOS would read past
   the end of the image. */
static bool ext_floppy_drive = 0xFF;
static uint8_t second_drive_attempts = 0;

/* Mounting the SD card during boot starves the emulator: the boot then dies
   after a handful of sector reads (measured: INT 13h stops at 5, the guest ends
   up executing zeroed RAM). Mounting it *after* the guest is up is fine (298
   successful reads, DOS keeps running), so the card is only brought up when the
   guest first touches drive B:. */
static bool try_mount_second_floppy(void);

bool cardputer_storage_ensure_second_drive(void) {
    // Retired. The architecture is now "flash = system disk, SD card = external
    // hard disk": the card is already exposed as drive C: through raw sectors
    // (CARDPUTER_STORAGE_SD_RAW), so there is no B: floppy to mount lazily. The
    // image-based path is kept below for reference only - mounting a FatFS
    // volume after the guest reserved its RAM always failed (largest contiguous
    // heap block ~14.8 KB), and doing it before boot starved the guest.
    return false;
}

static bool floppy_geometry_known(uint32_t size) {
    switch (size) {
        case 163840:   // 160 KB, 1 side, 8 sectors/track
        case 184320:   // 180 KB, 1 side, 9 sectors/track
        case 327680:   // 320 KB, 2 sides, 8 sectors/track
        case 368640:   // 360 KB, 2 sides, 9 sectors/track
        case 737280:   // 720 KB, 2 sides, 9 sectors/track
        case 1228800:  // 1.2 MB, 2 sides, 15 sectors/track
        case 1474560:  // 1.44 MB, 2 sides, 18 sectors/track
        case 2949120:  // 2.88 MB, 2 sides, 36 sectors/track
            return true;
        default:
            return false;
    }
}

bool cardputer_storage_second_drive_present(void) {
    return ext_floppy_drive != 0xFF;
}

static bool try_mount_second_floppy(void) {
    if (ext_floppy_drive != 0xFF) return true;
    if (!gb_disk_image.mounted) return false;
    if (gb_disk_drives[drive_slot(0x01)].mounted) return false;
#ifdef use_lib_log_serial
    Serial.printf("[sd] B: enter sd_available=%d images=%u heap=%lu\n",
                  sd_available ? 1 : 0, boot_image_count,
                  (unsigned long)ESP.getFreeHeap());
#endif
    if (!sd_available) {
        if (!mount_sd_timed()) {
#ifdef use_lib_log_serial
            Serial.printf("[sd] B: no card\n");
#endif
            return false;
        }
        sd_detected = true;
        scan_sd_images();
    }
    for (uint8_t i = 0; i < boot_image_count; i++) {
        const BootImageEntry &entry = boot_images[i];
        if (entry.storage != CARDPUTER_STORAGE_SD) continue;
        if (!floppy_geometry_known(entry.size)) continue;
        // Never mount the image that already serves as the boot drive.
        if (gb_disk_image.storage == CARDPUTER_STORAGE_SD &&
            strcmp(gb_disk_image.path, entry.path) == 0) {
            continue;
        }
        if (open_image_on_drive(entry, 0x01)) {
            ext_floppy_drive = 0x01;
#ifdef use_lib_log_serial
            snprintf(sd_diag_b_path, sizeof(sd_diag_b_path), "%.40s", entry.path);
            Serial.printf("Second drive B: <- %s size=%lu heap=%lu\n", entry.path,
                          (unsigned long)entry.size,
                          (unsigned long)ESP.getFreeHeap());
#endif
            return true;
        }
    }
#ifdef use_lib_log_serial
    Serial.printf("Second drive: no floppy-sized IMG on SD (images=%u)\n",
                  boot_image_count);
#endif
    return false;
}
#ifdef use_lib_log_serial
void cardputer_storage_debug_set_second_drive(bool enable) {
    if (enable) {
        try_mount_second_floppy();
        return;
    }
    ext_floppy_drive = 0xFF;
    close_drive(0x01);
}

void cardputer_storage_debug_drop_sd(void) {
    cardputer_storage_debug_set_second_drive(false);
    unmount_sd();
}
#endif

static bool select_boot_image(bool force_menu) {
    if (boot_image_count == 0) {
        unmount_sd();
        unmount_flash();
        return false;
    }

    uint8_t selected = 0;
    const char *items[MAX_BOOT_IMAGES];
    static char labels[MAX_BOOT_IMAGES][32];
    CardputerStorageType last_storage = CARDPUTER_STORAGE_NONE;
    char last_path[CARDPUTER_IMAGE_PATH_LEN] = {};
    const bool has_last = nvs_read_last_image(&last_storage, last_path,
                                              sizeof(last_path));
    for (uint8_t i = 0; i < boot_image_count; i++) {
        snprintf(labels[i], sizeof(labels[i]), "[%c] %.26s",
                 boot_images[i].storage == CARDPUTER_STORAGE_FLASH ? 'F' : 'S',
                 base_name(boot_images[i].path));
        items[i] = labels[i];
        if (has_last && boot_images[i].storage == last_storage &&
            strcmp(boot_images[i].path, last_path) == 0) {
            selected = i;
        }
        if (boot_images[i].storage == CARDPUTER_STORAGE_FLASH &&
            strcasecmp(base_name(boot_images[i].path), "cardputer86.img") == 0 &&
            !has_last) {
            selected = i;
        }
    }
    // 用户明确选过的系统（设置菜单 "Boot system"）优先于「上次启动的镜像」：
    // 这样选完 DOS 2.11 之后不管重启多少次、断电多少次，都还是 DOS 2.11。
    CardputerStorageType pref_storage = CARDPUTER_STORAGE_NONE;
    char pref_path[CARDPUTER_IMAGE_PATH_LEN] = {};
    if (nvs_read_boot_pref(&pref_storage, pref_path, sizeof(pref_path))) {
        for (uint8_t i = 0; i < boot_image_count; i++) {
            if (boot_images[i].storage == pref_storage &&
                strcmp(boot_images[i].path, pref_path) == 0) {
                selected = i;
                break;
            }
        }
    }
    // SD is always scanned now so an IMG on the card can become B:, which
    // means a card full of .img files must not hijack the boot every time.
    // Show the picker only when there is no internal default to fall back on,
    // or when the user explicitly asked for it by holding Alt.
    bool have_internal_default = false;
    for (uint8_t i = 0; i < boot_image_count; i++) {
        if (boot_images[i].storage == CARDPUTER_STORAGE_FLASH &&
            strcasecmp(base_name(boot_images[i].path), "cardputer86.img") == 0) {
            have_internal_default = true;
            break;
        }
    }
    if (boot_image_count > 1 && (force_menu || !have_internal_default)) {
        selected = choose_menu("Select boot image", items, boot_image_count,
                               selected, true);
        if (selected == MENU_CANCEL) selected = 0;
    }

    const BootImageEntry &entry = boot_images[selected];
    const uint8_t drive = default_drive_for_image(entry.size);
#ifdef use_lib_log_serial
    Serial.printf("[boot] pick %u/%u src=%s drive=%02X size=%lu heap=%lu path=%s\n",
                  (unsigned)selected, boot_image_count,
                  entry.storage == CARDPUTER_STORAGE_FLASH ? "flash" : "sd",
                  drive, (unsigned long)entry.size,
                  (unsigned long)ESP.getFreeHeap(), entry.path);
#endif
    gb_disk_image = {};
    if (open_image_on_drive(entry, drive)) {
        nvs_save_last_image(entry.storage, entry.path);
    }
#ifdef use_lib_log_serial
    if (gb_disk_image.mounted) {
        Serial.printf("Boot IMG: source=%s path=%s size=%lu drive=%02X\n",
                      gb_disk_image.storage == CARDPUTER_STORAGE_FLASH ?
                      "flash" : "sd", gb_disk_image.path,
                      (unsigned long)gb_disk_image.size, gb_disk_image.drive);
    } else {
        Serial.printf("Boot IMG open failed: %s\n", gb_disk_image.path);
    }
    // 512KB 决策链诊断（guest_memory_init 阶段的 Serial 输出会因 CDC 未就绪丢失）
    Serial.printf("guestmem: %s\n", guest_memory_diag_text());
    // 512KB 分页层往返自检（仅调试固件跑；lean 里是 "(selftest not run)"）
    Serial.printf("guestmem selftest: %s\n", guest_memory_selftest_text());
#endif
    return gb_disk_image.mounted;
}

/* ---- 多系统镜像：设置菜单与启动流程共用的查询接口 ------------------------ */

/* 换过启动镜像之后要立刻重启 guest。这个函数定义在 CardPuter86.ino 里，
   等价于串口调试命令 'r'：清 RAM、重建 BIOS 数据区、按当前镜像设 bootdrive、
   reset86()。 */
extern void cardputer_guest_restart(void);

uint8_t cardputer_storage_boot_image_count(void) {
    return boot_image_count;
}

const char *cardputer_storage_boot_image_label(uint8_t index) {
    static char label[32];
    if (index >= boot_image_count) return "";
    snprintf(label, sizeof(label), "[%c] %.26s",
             boot_images[index].storage == CARDPUTER_STORAGE_FLASH ? 'F' : 'S',
             base_name(boot_images[index].path));
    return label;
}

/* 只有文件名（不含 [F]/[S] 前缀），给设置条目的标题用。返回的是
   boot_images[index].path 内部的指针，不会被覆写。 */
const char *cardputer_storage_boot_image_name(uint8_t index) {
    if (index >= boot_image_count) return "none";
    return base_name(boot_images[index].path);
}

uint8_t cardputer_storage_boot_image_pref(void) {
    if (boot_image_count == 0) return 0xFF;
    CardputerStorageType pref_storage = CARDPUTER_STORAGE_NONE;
    char pref_path[CARDPUTER_IMAGE_PATH_LEN] = {};
    if (nvs_read_boot_pref(&pref_storage, pref_path, sizeof(pref_path))) {
        for (uint8_t i = 0; i < boot_image_count; i++) {
            if (boot_images[i].storage == pref_storage &&
                strcmp(boot_images[i].path, pref_path) == 0) {
                return i;
            }
        }
    }
    if (gb_disk_image.mounted) {   // 没有偏好就显示当前正在跑的那张
        for (uint8_t i = 0; i < boot_image_count; i++) {
            if (strcmp(boot_images[i].path, gb_disk_image.path) == 0) return i;
        }
    }
    return 0;
}

bool cardputer_storage_set_boot_image(uint8_t index, bool apply_now) {
    if (index >= boot_image_count) return false;
    const BootImageEntry entry = boot_images[index];
    nvs_save_boot_pref(entry.storage, entry.path);
    if (!apply_now) return true;

    const bool same = gb_disk_image.mounted &&
                      strcmp(gb_disk_image.path, entry.path) == 0;
    if (!same) {
        gb_disk_image = {};
        if (!open_image_on_drive(entry, default_drive_for_image(entry.size))) {
            return false;
        }
        nvs_save_last_image(entry.storage, entry.path);
    }
    cardputer_guest_restart();
    return true;
}

/* ---- raw SD card exposed as an external hard disk (C:, 0x80) -------------
   Architecture requested by the user: "flash holds the system disk, the SD card
   is only an external hard disk".  The card is therefore handed to the guest
   sector by sector, exactly like a real external disk: the guest reads the
   card's own MBR/partition table through INT 13h and MS-DOS 5.0 mounts whatever
   DOS partition it finds there (FAT16 - FAT32 is newer than DOS 5.0).  The
   firmware parses nothing and mounts no FatFS volume, which removes both
   blockers found earlier:
     * heap fragmentation - esp_vfs_fat_sdspi_mount needs a large contiguous
       block that no longer exists once guest_memory reserved its page cache
       (largest free block ~14.8 KB => ESP_FAIL), while this path needs only the
       card structure plus one 512-byte bounce buffer, both static below;
     * boot-time starvation - mounting a FatFS volume during boot starved the
       guest and the boot died after 5 sector reads.
   The guest can only address 1024*16*63 sectors (INT 13h CHS), so the visible
   disk is capped at 528 MB even on a 32 GB card; that also keeps image.size
   inside uint32_t. */
static sdmmc_card_t *sd_raw_card = nullptr;
static uint32_t sd_raw_sectors = 0;              // physical sectors on the card
static unsigned char sd_raw_buffer[512];         // bounce buffer for the DMA

/* Sequential read-ahead window over the raw card. 16 sectors = 8 KB in .bss;
   see sd_raw_read() for why (MS-DOS rescans the whole FAT for every DIR). */
#define SD_RAW_READAHEAD 16
static unsigned char sd_raw_ra_buf[SD_RAW_READAHEAD * 512];
static unsigned long sd_raw_ra_lba = 0;          // first LBA held in the window
static uint32_t sd_raw_ra_count = 0;             // valid sectors in the window
static uint32_t sd_io_ops = 0;                   // real card transactions
static uint32_t sd_io_us = 0;                    // time spent inside them
static uint32_t sd_io_reads = 0;                 // sector reads requested
static uint32_t sd_io_read_hits = 0;             // served from the window

#define SD_RAW_MAX_CHS (1024UL * 16UL * 63UL)

static bool mount_sd_raw(void) {
    if (sd_raw_card) return true;
    cardputer_storage_prepare_sd_bus();
    if (!sd_bus_ready) return false;

    static sdmmc_card_t card;   // ~1.2 KB, static so the heap is untouched
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = 20000;

    // esp_vfs_fat_sdspi_mount() does exactly this internally: init the host
    // driver, attach the device to get a handle, then hand that handle to
    // sdmmc_card_init() as host.slot.
    if (host.init) {
        const esp_err_t hrc = host.init();
        if (hrc != ESP_OK && hrc != ESP_ERR_INVALID_STATE) return false;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = (gpio_num_t)SD_CS;
    slot.host_id = SD_SPI_HOST;

    sdmmc_host_t card_host = host;
    esp_err_t rc = ESP_FAIL;
    unsigned tries = 0;
    // Card init is the 400 kHz CMD0/CMD8/ACMD41 handshake. A card that lost
    // power (or was reset by the MCU) in the middle of a write can answer with
    // ESP_ERR_INVALID_RESPONSE (0x108) and then keep failing until it sees a
    // clean 74-clock preamble + CMD0 again. sdspi only emits that preamble when
    // the device is attached, so each attempt detaches and re-attaches instead
    // of just re-calling sdmmc_card_init(). This mount runs once per boot, and a
    // miss costs the guest its whole C: drive (DOS: "Invalid drive
    // specification"), so burn ~1 s retrying before giving up.
    for (tries = 1; tries <= 8; tries++) {
        sdspi_dev_handle_t handle = 0;
        rc = sdspi_host_init_device(&slot, &handle);
        if (rc != ESP_OK) {
            delay(40);
            continue;
        }
        card_host.slot = handle;
        memset(&card, 0, sizeof(card));
        rc = sdmmc_card_init(&card_host, &card);
        if (rc == ESP_OK) break;
        sdspi_host_remove_device(handle);
        delay(40);
    }
#ifdef use_lib_log_serial
    Serial.printf("[sd] raw init rc=0x%X tries=%u cap=%lu heap=%lu max=%lu\n", (unsigned)rc,
                  (unsigned)tries,
                  (unsigned long)card.csd.capacity,
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getMaxAllocHeap());
#endif
    if (rc != ESP_OK) return false;
    sd_raw_card = &card;
    sd_raw_sectors = card.csd.capacity;
    return true;
}

static bool sd_raw_read(unsigned long lba, unsigned char *buffer) {
    if (!sd_raw_card || lba >= sd_raw_sectors) return false;

    // Read-ahead window. MS-DOS re-reads the WHOLE FAT every time it needs the
    // free-space count (plain `DIR C:` does it), and on a 504 MB FAT16 volume
    // that FAT is 253 sectors: with one CMD17 per sector the SD transaction
    // overhead dominates and `dir c:` stalls the guest for a second or more.
    // Fetching 16 sectors per transaction (CMD18) turns those 253 commands into
    // 16. Everything is invalidated on write, so the cache can never serve a
    // sector the guest has already modified.
    if (sd_raw_ra_count && lba >= sd_raw_ra_lba && lba < sd_raw_ra_lba + sd_raw_ra_count) {
        memcpy(buffer, sd_raw_ra_buf + (uint32_t)(lba - sd_raw_ra_lba) * 512UL, 512);
        sd_io_reads++;
        sd_io_read_hits++;
        return true;
    }

    unsigned long want = SD_RAW_READAHEAD;
    if (lba + want > sd_raw_sectors) want = sd_raw_sectors - lba;
    if (want < 1) want = 1;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t rc = sdmmc_read_sectors(sd_raw_card, sd_raw_ra_buf, lba, (size_t)want);
    if (rc != ESP_OK) {
        // Some cards/libraries refuse multi-block reads; fall back to one sector
        // so the drive keeps working (only slower) instead of disappearing.
        want = 1;
        rc = sdmmc_read_sectors(sd_raw_card, sd_raw_ra_buf, lba, 1);
    }
    sd_io_us += (uint32_t)(esp_timer_get_time() - t0);
    sd_io_ops++;
    if (rc != ESP_OK) {
        sd_raw_ra_count = 0;
        return false;
    }
    sd_raw_ra_lba = lba;
    sd_raw_ra_count = (uint32_t)want;
    sd_io_reads++;
    memcpy(buffer, sd_raw_ra_buf, 512);
    return true;
}

static bool sd_raw_write(unsigned long lba, const unsigned char *buffer) {
    if (!sd_raw_card || lba >= sd_raw_sectors) return false;
    sd_raw_ra_count = 0;                  /* the window may now be stale */
    memcpy(sd_raw_buffer, buffer, 512);
    const int64_t t0 = esp_timer_get_time();
    const bool ok = sdmmc_write_sectors(sd_raw_card, sd_raw_buffer, lba, 1) == ESP_OK;
    sd_io_us += (uint32_t)(esp_timer_get_time() - t0);
    sd_io_ops++;
    return ok;
}

#ifdef use_lib_log_serial
/* Debug helpers for the external hard disk (dbglean only, driven by the serial
   command console: 'z' wipes, 'd' dumps).  They talk to the raw card directly,
   so they work no matter what state the guest's FDISK left behind. */
bool cardputer_storage_raw_wipe(uint32_t sectors) {
    if (!sd_raw_card) {
        Serial.printf("[sd] wipe: no raw card\n");
        return false;
    }
    memset(sd_raw_buffer, 0, sizeof(sd_raw_buffer));
    for (uint32_t i = 0; i < sectors; i++) {
        if (sdmmc_write_sectors(sd_raw_card, sd_raw_buffer, i, 1) != ESP_OK) {
            Serial.printf("[sd] wipe: write failed at lba %lu\n", (unsigned long)i);
            return false;
        }
    }
    Serial.printf("[sd] wipe: zeroed %lu sectors\n", (unsigned long)sectors);
    return true;
}

void cardputer_storage_raw_dump_mbr(void) {
    if (!sd_raw_card) {
        Serial.printf("[sd] dump: no raw card\n");
        return;
    }
    unsigned char mbr[512];
    if (!sd_raw_read(0, mbr)) {
        Serial.printf("[sd] dump: read failed\n");
        return;
    }
    Serial.printf("[sd] mbr sig=%02X%02X cap=%lu\n", mbr[510], mbr[511],
                  (unsigned long)sd_raw_sectors);
    for (int i = 0; i < 4; i++) {
        const unsigned char *e = mbr + 446 + i * 16;
        const uint32_t lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                             ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        const uint32_t cnt = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                             ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        Serial.printf("[sd] p%d boot=%02X type=%02X lba=%lu cnt=%lu\n", i + 1,
                      e[0], e[4], (unsigned long)lba, (unsigned long)cnt);
    }
}

/* Read-path counters ('s' command / per-5s debug line) and a micro benchmark
   ('p' command) so "dir c: is slow" can be split into card time vs guest time
   instead of guessed.  card_us is time actually spent inside the card driver;
   sector reads served from the read-ahead window cost nothing. */
void cardputer_storage_sd_io_stats(uint32_t *ops, uint32_t *op_us, uint32_t *reads,
                                   uint32_t *hits) {
    if (ops) *ops = sd_io_ops;
    if (op_us) *op_us = sd_io_us;
    if (reads) *reads = sd_io_reads;
    if (hits) *hits = sd_io_read_hits;
}

/* p: benchmark the raw card. 100 single-sector reads vs 100 reads served by the
   read-ahead window, at the same LBA, plus a cold sequential scan of 256
   sectors (the shape of a FAT scan). Prints microseconds. */
void cardputer_storage_debug_bench_sd(void) {
    if (!sd_raw_card) {
        Serial.printf("[sd] bench: no raw card\n");
        return;
    }
    const unsigned long lba = 64 + 2048;         /* inside the partition */
    unsigned char buf[512];

    sd_raw_ra_count = 0;                          /* force a cold read first */
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < 100; i++) {
        sd_raw_ra_count = 0;                      /* always miss */
        sd_raw_read(lba, buf);
    }
    const uint32_t one_us = (uint32_t)(esp_timer_get_time() - t0);

    t0 = esp_timer_get_time();
    for (int i = 0; i < 100; i++) {
        sd_raw_read(lba + (unsigned long)(i % SD_RAW_READAHEAD), buf);
    }
    const uint32_t hit_us = (uint32_t)(esp_timer_get_time() - t0);

    sd_raw_ra_count = 0;
    t0 = esp_timer_get_time();
    for (unsigned long i = 0; i < 256; i++) {
        sd_raw_read(64 + i, buf);
    }
    const uint32_t seq_us = (uint32_t)(esp_timer_get_time() - t0);

    Serial.printf("[sd] bench: cold1sector=%luus (%luus/read) window-hit=%luus (%luus/read) seq256=%luus (%luus/sector)\n",
                  (unsigned long)one_us, (unsigned long)(one_us / 100),
                  (unsigned long)hit_us, (unsigned long)(hit_us / 100),
                  (unsigned long)seq_us, (unsigned long)(seq_us / 256));
    Serial.printf("[sd] io: ops=%lu card_us=%lu reads=%lu hits=%lu\n",
                  (unsigned long)sd_io_ops, (unsigned long)sd_io_us,
                  (unsigned long)sd_io_reads, (unsigned long)sd_io_read_hits);
}
#endif

static void register_sd_raw_drive(void) {
    if (!sd_raw_card) return;
    const int8_t slot = drive_slot(0x80);
    if (slot < 0 || gb_disk_drives[slot].mounted) return;
    uint32_t sectors = sd_raw_sectors;
    if (sectors > SD_RAW_MAX_CHS) sectors = SD_RAW_MAX_CHS;
    CardputerDiskImage &image = gb_disk_drives[slot];
    image = {};
    set_disk_geometry(image, 0x80, sectors * 512UL);
    image.storage = CARDPUTER_STORAGE_SD_RAW;
    image.mounted = true;
    snprintf(image.path, sizeof(image.path), "<sd raw %luMB>",
             (unsigned long)(image.size / (1024UL * 1024UL)));
#ifdef use_lib_log_serial
    Serial.printf("[sd] C: = raw card, %luMB CHS %u/%u/%u\n",
                  (unsigned long)(image.size / (1024UL * 1024UL)),
                  image.cylinders, image.heads, image.sectors);
#endif
}

#ifdef use_lib_log_serial
// Escape hatch for a card that missed its boot-time handshake: mount again at
// runtime and register C:. The guest only sees the new drive after it reboots
// (send 0x01 'r'), because the hard-disk count in the BDA was written at POST.
bool cardputer_storage_debug_retry_sd(void) {
    if (sd_raw_card) return true;
    if (!mount_sd_raw()) return false;
    register_sd_raw_drive();
    return true;
}
#endif

bool cardputer_storage_init_and_select(void) {
    gb_disk_image = {};
    boot_image_count = 0;
    flash_detected = false;
    sd_detected = false;

    // Scan one filesystem at a time. Mounting FFat and SD together after the
    // emulator RAM reservation can exhaust or fragment the remaining heap.
    show_probe_status("Checking internal IMG partition...");
    if (mount_flash()) {
        flash_detected = true;
        scan_flash_images();
        unmount_flash();
    }
    // Bring the SD card up NOW, while the heap is still contiguous: the guest
    // memory reservation that follows leaves only ~14.8 KB as the largest free
    // block. The raw sector path costs almost nothing, so it fits; a FatFS
    // volume does not (that is why B: could never be mounted after boot).
    if (mount_sd_raw()) {
        sd_detected = true;
        register_sd_raw_drive();
    }
    // Hold Alt to additionally scan the card for bootable floppy images. That
    // needs a FatFS volume, which can only be mounted while the heap is still
    // healthy - i.e. right here - and it conflicts with the raw slot above, so
    // it is only offered when the raw hard disk did not come up.
    const bool force_menu = alt_requests_sd() && !sd_raw_card;
    if (force_menu) {
        show_probe_status("Checking SD card...", "Timeout: 15 seconds");
        if (mount_sd_timed()) {
            sd_detected = true;
            scan_sd_images();
            unmount_sd();
        }
    }

    show_probe_status("Selecting boot image...");

#ifdef use_lib_log_serial
    Serial.printf("Storage: images=%u heap=%lu\n", boot_image_count,
                  (unsigned long)ESP.getFreeHeap());
#endif
    const bool booted = select_boot_image(force_menu);
    return booted;
}

void cardputer_storage_show_boot_status(void) {
    if (gb_disk_image.mounted) {
        char status[64];
        char hint[64];
        snprintf(status, sizeof(status), "Booting %s from %s",
                 base_name(gb_disk_image.path),
                 gb_disk_image.storage == CARDPUTER_STORAGE_FLASH ?
                 "flash" : "SD");
        // 页数（常驻页/总页数）直接显示出来：这是这台机器上唯一能改变实际
        // 吞吐的因素 —— 每少一次换页就少一次「擦 4KB + 写 4KB」≈55ms 的停顿。
        snprintf(hint, sizeof(hint), "%s %luKB RAM %u/%lu pg %s%s",
                 gb_disk_image.drive == 0x80 ? "C:" : "A:",
                 (unsigned long)guest_memory_ram_kb(),
                 (unsigned)guest_memory_cache_pages(),
                 (unsigned long)(guest_memory_ram_kb() * 1024UL / 4096UL),
                 cardputer_cpu_profile_label(cardputer_cpu_profile()),
                 ext_floppy_drive != 0xFF ? " +B:SD" : "");
        show_probe_status(status, hint);
        delay(1200);
    } else {
        show_probe_status("ERROR: No bootable IMG found",
                          "Use USB disk import, then reboot");
        delay(2500);
    }
}

bool cardputer_storage_drive_geometry(uint8_t drive, uint16_t *cylinders,
                                      uint8_t *heads, uint8_t *sectors) {
    const int8_t slot = drive_slot(drive);
    if (slot < 0) return false;
    if (drive == 0x01 && !gb_disk_drives[slot].mounted) {
        // First time the guest asks about B:: bring the SD card up now instead
        // of during boot, where it would starve the emulator.
        cardputer_storage_ensure_second_drive();
    }
    if (!gb_disk_drives[slot].mounted) return false;
    if (cylinders) *cylinders = gb_disk_drives[slot].cylinders;
    if (heads) *heads = gb_disk_drives[slot].heads;
    if (sectors) *sectors = gb_disk_drives[slot].sectors;
    return true;
}

uint8_t cardputer_storage_hard_drive_count(void) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < CARDPUTER_DRIVE_COUNT; i++) {
        if (gb_disk_drives[i].mounted && gb_disk_drives[i].drive >= 0x80) count++;
    }
    return count;
}

bool cardputer_storage_read_sector(uint8_t drive, unsigned long lba,
                                   unsigned char *buffer) {
    const int8_t slot = drive_slot(drive);
    if (slot < 0) return false;
    if (drive == 0x01 && !gb_disk_drives[slot].mounted) {
        cardputer_storage_ensure_second_drive();
    }
    CardputerDiskImage &image = gb_disk_drives[slot];
    if (!image.mounted || (lba + 1) * 512UL > image.size) {
        disk_read_fail_count++;
        disk_read_fail_lba = lba;
        disk_read_fail_drive = drive;
        return false;
    }
    if (image.storage == CARDPUTER_STORAGE_FLASH) {
        const bool ok = flash_image_files[slot] &&
               fseek(flash_image_files[slot], lba * 512UL, SEEK_SET) == 0 &&
               fread(buffer, 1, 512, flash_image_files[slot]) == 512;
        if (ok) disk_read_ok_count++;
        else { disk_read_fail_count++; disk_read_fail_lba = lba; disk_read_fail_drive = drive; }
        return ok;
    }
    if (image.storage == CARDPUTER_STORAGE_SD) {
        const bool ok = sd_image_files[slot] &&
               fseek(sd_image_files[slot], (long)(lba * 512UL), SEEK_SET) == 0 &&
               fread(buffer, 1, 512, sd_image_files[slot]) == 512;
        if (ok) disk_read_ok_count++;
        else { disk_read_fail_count++; disk_read_fail_lba = lba; disk_read_fail_drive = drive; }
        return ok;
    }
    if (image.storage == CARDPUTER_STORAGE_SD_RAW) {
        const bool ok = sd_raw_read(lba, buffer);
        if (ok) disk_read_ok_count++;
        else { disk_read_fail_count++; disk_read_fail_lba = lba; disk_read_fail_drive = drive; }
        return ok;
    }
    return false;
}

bool cardputer_storage_write_sector(uint8_t drive, unsigned long lba,
                                    const unsigned char *buffer) {
    const int8_t slot = drive_slot(drive);
    if (slot < 0) return false;
    if (drive == 0x01 && !gb_disk_drives[slot].mounted) {
        cardputer_storage_ensure_second_drive();
    }
    CardputerDiskImage &image = gb_disk_drives[slot];
    if (!image.mounted || (lba + 1) * 512UL > image.size) return false;
    if (image.storage == CARDPUTER_STORAGE_FLASH) {
        if (!flash_image_files[slot]) return false;
        bool ok = fseek(flash_image_files[slot], lba * 512UL, SEEK_SET) == 0 &&
                  fwrite(buffer, 1, 512, flash_image_files[slot]) == 512;
        fflush(flash_image_files[slot]);
        return ok;
    }
    if (image.storage == CARDPUTER_STORAGE_SD) {
        if (!sd_image_files[slot]) return false;
        const bool ok =
            fseek(sd_image_files[slot], (long)(lba * 512UL), SEEK_SET) == 0 &&
            fwrite(buffer, 1, 512, sd_image_files[slot]) == 512;
        fflush(sd_image_files[slot]);
        return ok;
    }
    if (image.storage == CARDPUTER_STORAGE_SD_RAW) {
        return sd_raw_write(lba, buffer);
    }
    return false;
}

bool cardputer_storage_read_sector(unsigned long lba, unsigned char *buffer) {
    return gb_disk_image.mounted &&
           cardputer_storage_read_sector(gb_disk_image.drive, lba, buffer);
}

bool cardputer_storage_write_sector(unsigned long lba, const unsigned char *buffer) {
    return gb_disk_image.mounted &&
           cardputer_storage_write_sector(gb_disk_image.drive, lba, buffer);
}

static bool flush_flash_cache(void) {
    if (!usb_flash_cache_dirty || usb_flash_cache_address == SIZE_MAX) return true;
    if (wl_erase_range(usb_flash_wl, usb_flash_cache_address,
                       usb_flash_cache_size) != ESP_OK ||
        wl_write(usb_flash_wl, usb_flash_cache_address, usb_flash_cache,
                 usb_flash_cache_size) != ESP_OK) {
        return false;
    }
    usb_flash_cache_dirty = false;
    return true;
}

static bool load_flash_cache(size_t address) {
    const size_t block = address - address % usb_flash_cache_size;
    if (block == usb_flash_cache_address) return true;
    if (!flush_flash_cache()) return false;
    if (wl_read(usb_flash_wl, block, usb_flash_cache,
                usb_flash_cache_size) != ESP_OK) return false;
    usb_flash_cache_address = block;
    return true;
}

static int32_t usb_read(uint32_t lba, uint32_t offset, void *buffer,
                        uint32_t size) {
    uint8_t *destination = static_cast<uint8_t *>(buffer);
    uint32_t copied = 0;
    while (copied < size) {
        const size_t address = (size_t)lba * usb_block_size + offset + copied;
        uint32_t chunk;
        if (usb_storage == CARDPUTER_STORAGE_SD) {
            // Raw sector access to the card is gone with the arduino SD library:
            // the card now lives behind a VFS mount, and poking raw sectors
            // underneath FatFS would corrupt it. The flash export still works.
            return copied ? copied : -1;
        } else {
            if (!load_flash_cache(address)) return copied ? copied : -1;
            const size_t cache_offset = address - usb_flash_cache_address;
            chunk = min((size_t)(size - copied), usb_flash_cache_size - cache_offset);
            memcpy(destination + copied, usb_flash_cache + cache_offset, chunk);
        }
        copied += chunk;
    }
    return copied;
}

static int32_t usb_write(uint32_t lba, uint32_t offset, uint8_t *buffer,
                         uint32_t size) {
    uint32_t copied = 0;
    while (copied < size) {
        const size_t address = (size_t)lba * usb_block_size + offset + copied;
        uint32_t chunk;
        if (usb_storage == CARDPUTER_STORAGE_SD) {
            return copied ? copied : -1;   // see usb_read(): raw SD access removed
        } else {
            if (!load_flash_cache(address)) return copied ? copied : -1;
            const size_t cache_offset = address - usb_flash_cache_address;
            chunk = min((size_t)(size - copied), usb_flash_cache_size - cache_offset);
            memcpy(usb_flash_cache + cache_offset, buffer + copied, chunk);
            usb_flash_cache_dirty = true;
        }
        copied += chunk;
    }
    return copied;
}

static bool usb_start_stop(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition;
    flush_flash_cache();
    usb_msc.mediaPresent(!load_eject || start);
    return true;
}

static bool start_flash_usb(void) {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT,
        FLASH_PARTITION_LABEL);
    if (!partition || wl_mount(partition, &usb_flash_wl) != ESP_OK) return false;
    usb_flash_cache_size = wl_sector_size(usb_flash_wl);
    usb_flash_cache = static_cast<uint8_t *>(malloc(usb_flash_cache_size));
    if (!usb_flash_cache) {
        wl_unmount(usb_flash_wl);
        usb_flash_wl = WL_INVALID_HANDLE;
        return false;
    }
    usb_storage = CARDPUTER_STORAGE_FLASH;
    return true;
}

static bool start_usb_msc(CardputerStorageType storage) {
    uint32_t sector_count = 0;
    if (storage == CARDPUTER_STORAGE_SD) {
#ifdef use_lib_log_serial
        Serial.printf("USB MSC: SD export disabled (card is behind a VFS mount)\n");
#endif
        return false;
    } else {
        if (!start_flash_usb()) return false;
        usb_block_size = usb_flash_cache_size;
        sector_count = wl_size(usb_flash_wl) / usb_block_size;
    }
#ifdef use_lib_log_serial
    Serial.printf("USB MSC: source=%s blocks=%lu block_size=%u\n",
                  storage == CARDPUTER_STORAGE_SD ? "sd" : "flash",
                  (unsigned long)sector_count, usb_block_size);
#endif

    usb_msc.vendorID("M5Stack");
    usb_msc.productID(storage == CARDPUTER_STORAGE_SD ?
                      "CardPuter86 SD" : "CardPuter86 Flash");
    usb_msc.productRevision("1.0");
    usb_msc.onStartStop(usb_start_stop);
    usb_msc.onRead(usb_read);
    usb_msc.onWrite(usb_write);
    usb_msc.mediaPresent(true);
    if (!usb_msc.begin(sector_count, usb_block_size)) {
        usb_msc.end();
        if (usb_flash_wl != WL_INVALID_HANDLE) {
            free(usb_flash_cache);
            usb_flash_cache = nullptr;
            wl_unmount(usb_flash_wl);
            usb_flash_wl = WL_INVALID_HANDLE;
        }
        return false;
    }
    USB.productName(storage == CARDPUTER_STORAGE_SD ?
                    "CardPuter86 SD Card" : "CardPuter86 Internal Disk");
    USB.begin();
    return true;
}

static bool start_selected_usb_mode(void) {
    auto &display = M5Cardputer.Display;
    CardputerStorageType selected = CARDPUTER_STORAGE_FLASH;
    if (sd_detected && mount_sd_timed()) {
        const char *items[] = {"Internal Flash", "SD Card"};
        uint8_t choice = choose_menu("USB storage source", items, 2, 0, false);
        if (choice == MENU_CANCEL) {
            unmount_sd();
            return false;
        }
        selected = choice == 0 ? CARDPUTER_STORAGE_FLASH : CARDPUTER_STORAGE_SD;
        if (selected == CARDPUTER_STORAGE_FLASH) unmount_sd();
    }

    display.fillScreen(TFT_BLACK);
    display.setCursor(8, 48);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.print("Starting USB storage...");
    if (!start_usb_msc(selected)) {
        display.setCursor(8, 68);
        display.setTextColor(TFT_RED, TFT_BLACK);
        display.print("USB storage init failed");
        delay(2000);
        return false;
    }

    display.fillScreen(TFT_BLACK);
    display.setCursor(8, 44);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.print(selected == CARDPUTER_STORAGE_SD ?
                  "SD USB disk active" : "Flash USB disk active");
    display.setCursor(8, 64);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.print("Copy IMG files, then eject");
    display.setCursor(8, 78);
    display.print("Reboot after safe eject");
    while (true) delay(1000);
}

static bool start_usb_cdc_mode(void) {
    auto &display = M5Cardputer.Display;
    display.fillScreen(TFT_BLACK);
    display.setCursor(8, 40);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.print("USB CDC serial mode");
    display.setCursor(8, 58);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.print("Mutually exclusive with disk");

    USB.productName("CardPuter86 Serial");
    usb_cdc.begin();
    USB.begin();
    usb_cdc.println("CardPuter86 CDC serial ready.");
    usb_cdc.println("Reboot to leave this mode.");
    display.setCursor(8, 82);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.print("Serial active; reboot to exit");
    while (true) {
        if (usb_cdc.available()) {
            int c = usb_cdc.read();
            if (c >= 0) usb_cdc.write((uint8_t)c);
        }
        delay(10);
    }
}

bool cardputer_storage_show_mount_menu(void) {
    if (boot_image_count == 0) {
        show_probe_status("No IMG files found", "Use USB disk mode to import");
        delay(1500);
        return false;
    }

    const char *drive_items[] = {"A: floppy", "B: floppy", "C: hard disk", "D: hard disk"};
    uint8_t drive_choice = choose_menu("Mount target", drive_items, 4, 1, false);
    if (drive_choice == MENU_CANCEL) return false;
    const uint8_t drive = DRIVE_IDS[drive_choice];

    const char *items[MAX_BOOT_IMAGES];
    static char labels[MAX_BOOT_IMAGES][36];
    uint8_t selected = 0;
    for (uint8_t i = 0; i < boot_image_count; i++) {
        snprintf(labels[i], sizeof(labels[i]), "[%c] %.30s",
                 boot_images[i].storage == CARDPUTER_STORAGE_FLASH ? 'F' : 'S',
                 base_name(boot_images[i].path));
        items[i] = labels[i];
    }
    selected = choose_menu("Mount IMG", items, boot_image_count, selected, false);
    if (selected == MENU_CANCEL) return false;

    if ((drive < 0x80 && boot_images[selected].size > 2949120) ||
        (drive >= 0x80 && boot_images[selected].size <= 2949120)) {
        show_probe_status("Image/drive type mismatch",
                          drive < 0x80 ? "Choose floppy-sized IMG" : "Choose hard-disk IMG");
        delay(1800);
        return false;
    }

    if (!open_image_on_drive(boot_images[selected], drive)) {
        show_probe_status("Mount failed", base_name(boot_images[selected].path));
        delay(1500);
        return false;
    }
    show_probe_status("Mounted", base_name(boot_images[selected].path));
    delay(1000);
    return true;
}

bool cardputer_storage_show_settings_menu(bool allow_usb_disk) {
    while (true) {
        char ram_item[40];
        char cpu_item[32];
        char mcu_item[40];
        char audio_item[32];
        char com1_item[40];
        char sleep_item[32];
        char sleep_led_item[32];
        char rtc_item[40];
        char boot_item[40];
        char mount_item[32];
        char usb_item[40];
        char wifi_item[40];
        char btkbd_item[40];
        snprintf(ram_item, sizeof(ram_item), "Guest RAM: %lu KB",
                 (unsigned long)guest_memory_ram_kb());
        snprintf(cpu_item, sizeof(cpu_item), "CPU speed: %s",
                 cardputer_cpu_profile_label(cardputer_cpu_profile()));
        snprintf(mcu_item, sizeof(mcu_item), "Processor: %lu MHz%s",
                 (unsigned long)(cardputer_settings_mcu_80mhz_enabled() ? 80 : 240),
                 cardputer_settings_mcu_80mhz_enabled() ? " (eco)" : "");
        snprintf(audio_item, sizeof(audio_item), "Audio: %s",
                 cardputer_settings_audio_enabled() ? "Enabled" : "Disabled");
        snprintf(com1_item, sizeof(com1_item), "Network COM1: %s",
                 cardputer_settings_com1_enabled() ? "Enabled" : "Disabled");
        const uint32_t sleep_seconds = cardputer_settings_sleep_timeout_seconds();
        if (sleep_seconds == 0) {
            snprintf(sleep_item, sizeof(sleep_item), "Sleep timeout: Never");
        } else if (sleep_seconds < 60) {
            snprintf(sleep_item, sizeof(sleep_item), "Sleep timeout: %lu sec",
                     (unsigned long)sleep_seconds);
        } else {
            snprintf(sleep_item, sizeof(sleep_item), "Sleep timeout: %lu min",
                     (unsigned long)(sleep_seconds / 60));
        }
        snprintf(sleep_led_item, sizeof(sleep_led_item), "Sleep LED: %s",
                 cardputer_settings_sleep_led_enabled() ? "Enabled" : "Disabled");
        char rtc_text[24];
        cardputer_rtc_format(cardputer_rtc_now(), rtc_text, sizeof(rtc_text));
        snprintf(rtc_item, sizeof(rtc_item), "RTC: %.20s", rtc_text);
        // 「下次启动哪个系统」：显示当前偏好那张镜像的文件名
        {
            const uint8_t pref = cardputer_storage_boot_image_pref();
            snprintf(boot_item, sizeof(boot_item), "Boot system: %.20s",
                     pref == 0xFF ? "none" :
                     cardputer_storage_boot_image_name(pref));
        }
        snprintf(mount_item, sizeof(mount_item), "Mount disk/IMG");
        const char *usb_labels[] = {"Charge only", "Serial CDC", "USB disk"};
        snprintf(usb_item, sizeof(usb_item), "USB interface: %s",
                 usb_labels[cardputer_settings_usb_mode()]);
        snprintf(wifi_item, sizeof(wifi_item), "WiFi modem: %s",
                 cardputer_modem_wifi_connected() ? "Connected" :
                 (cardputer_modem_wifi_configured() ? "Configured" : "Not set"));
        snprintf(btkbd_item, sizeof(btkbd_item), "BT keyboard: %s",
                 cardputer_settings_bt_kbd_enabled() ? "Enabled" : "Disabled");
        const char *items[] = {
            mount_item, wifi_item, usb_item, audio_item, com1_item,
            "More settings", btkbd_item, "Continue"
        };
        const uint8_t choice = choose_menu(
            "CardPuter86 Settings", items, 8, 0, false);
        if (choice == MENU_CANCEL) return false;
        if (choice == 0) {
            cardputer_storage_show_mount_menu();
            continue;
        }
        if (choice == 1) {
            show_wifi_modem_menu();
            continue;
        }
        if (choice == 2) {
            const char *usb_modes[] = {"Charge only", "Serial CDC", "USB disk"};
            uint8_t mode = choose_menu("USB interface", usb_modes, 3,
                                       cardputer_settings_usb_mode(), false);
            if (mode == MENU_CANCEL) continue;
            cardputer_settings_set_usb_mode(mode);
            if (mode == 0) {
                show_probe_status("USB: charge only", "No USB function started");
                delay(1000);
                continue;
            }
            if (mode == 1) return start_usb_cdc_mode();
            if (allow_usb_disk) return start_selected_usb_mode();
            show_probe_status("USB disk unavailable", "Use boot storage mode");
            delay(1500);
            continue;
        }
        if (choice == 3) {
            const bool enable = !cardputer_settings_audio_enabled();
            if (!cardputer_settings_set_audio_enabled(enable)) {
                show_probe_status("Audio setting failed", "NVS write error");
                delay(1500);
            }
            gb_silence = enable ? 0 : 1;
            continue;
        }
        if (choice == 4) {
            const bool enable = !cardputer_settings_com1_enabled();
            if (!cardputer_settings_set_com1_enabled(enable)) {
                show_probe_status("COM1 setting failed", "NVS write error");
                delay(1500);
            }
            continue;
        }
        if (choice == 5) {
            while (true) {
                const char *more_items[] = {
                    ram_item, cpu_item, mcu_item, sleep_item, sleep_led_item,
                    rtc_item, boot_item, "Back"
                };
                const uint8_t more_choice = choose_menu(
                    "More settings", more_items, 8, 0, false);
                if (more_choice == MENU_CANCEL || more_choice == 7) break;
                if (more_choice == 0) {
                    // 客机内存三档：128 KB 不分页（最快，装不下 DOS 5.0）、
                    // 256 KB 分页（默认）、512 KB 分页（DOS 看到 512K，但
                    // flash 换页量翻倍，POST 和读写都明显变慢）。
                    const char *ram_options[] = {
                        "128 KB (DOS 2.11; pages if SRAM short)",
                        "256 KB (DOS 5.0 default)",
                        "512 KB (very slow unless SRAM free)"
                    };
                    const uint32_t current_kb = guest_memory_ram_kb();
                    const uint8_t selected =
                        current_kb == 128 ? 0 : (current_kb == 512 ? 2 : 1);
                    const uint8_t pick = choose_menu("Guest RAM", ram_options, 3,
                                                     selected, false);
                    if (pick != MENU_CANCEL) {
                        const uint32_t target_kb =
                            pick == 0 ? 128 : (pick == 2 ? 512 : 256);
                        // 切档会把整块 guest RAM 换掉（guest_memory_set_ram_kb()
                        // 内部重建页缓存并清零），所以必须立刻重放引导，否则
                        // guest 会在被清空的 RAM 上继续执行 → 黑屏/崩。
                        extern void cardputer_guest_restart(void);
                        if (guest_memory_set_ram_kb(target_kb)) {
                            char ram_msg[32];
                            snprintf(ram_msg, sizeof(ram_msg), "Guest RAM: %lu KB",
                                     (unsigned long)guest_memory_ram_kb());
                            show_probe_status(ram_msg, "Rebooting guest...");
                            delay(600);
                            cardputer_guest_restart();
                        } else {
                            show_probe_status("Guest RAM change failed",
                                              "Restart the device to apply");
                            delay(1500);
                        }
                    }
                    break;
                }
                if (more_choice == 1) {
                    const char *cpu_profiles[] = {
                        "4.77 MHz (IBM PC)", "8 MHz", "10 MHz", "12 MHz",
                        "Unlimited (fastest)", "16 MHz", "24 MHz", "33 MHz",
                        "80 MHz (turbo)"
                    };
                    const uint8_t profile = choose_menu(
                        "CPU speed", cpu_profiles, cardputer_cpu_profile_count(),
                        cardputer_cpu_profile(), false);
                    if (profile != MENU_CANCEL &&
                        !cardputer_cpu_set_profile(profile)) {
                        show_probe_status("CPU setting failed", "NVS write error");
                        delay(1500);
                    }
                    break;
                }
                if (more_choice == 2) {
                    // 处理器主频：80 MHz 省电（模拟器约慢 3 倍、WiFi 吞吐下降），
                    // 240 MHz 默认。立即生效，下次开机由 apply_mcu_clock() 复现。
                    const char *mcu_options[] = {
                        "240 MHz (fastest)",
                        "80 MHz (eco, ~3x slower)"
                    };
                    const uint8_t selected =
                        cardputer_settings_mcu_80mhz_enabled() ? 1 : 0;
                    const uint8_t pick = choose_menu("Processor clock", mcu_options,
                                                     2, selected, false);
                    if (pick != MENU_CANCEL &&
                        !cardputer_settings_set_mcu_80mhz_enabled(pick == 1)) {
                        show_probe_status("Processor setting failed",
                                          "NVS write error");
                        delay(1500);
                    }
                    break;
                }
                if (more_choice == 3) {
                    const char *sleep_profiles[] = {
                        "30 seconds", "2 minutes", "5 minutes", "10 minutes", "Never"
                    };
                    const uint32_t sleep_values[] = {30, 120, 300, 600, 0};
                    uint8_t selected = 1;
                    for (uint8_t i = 0; i < 5; i++) {
                        if (sleep_values[i] == cardputer_settings_sleep_timeout_seconds()) {
                            selected = i;
                            break;
                        }
                    }
                    const uint8_t profile = choose_menu(
                        "Sleep timeout", sleep_profiles, 5, selected, false);
                    if (profile != MENU_CANCEL &&
                        !cardputer_settings_set_sleep_timeout_seconds(sleep_values[profile])) {
                        show_probe_status("Sleep setting failed", "NVS write error");
                        delay(1500);
                    }
                    break;
                }
                if (more_choice == 4) {
                    const bool enable = !cardputer_settings_sleep_led_enabled();
                    if (!cardputer_settings_set_sleep_led_enabled(enable)) {
                        show_probe_status("Sleep LED failed", "NVS write error");
                        delay(1500);
                    }
                    break;
                }
                if (more_choice == 5) {
                    char input[15] = {};
                    uint8_t pos = 0;
                    while (true) {
                        auto &display = M5Cardputer.Display;
                        display.fillScreen(TFT_BLACK);
                        display.fillRect(0, 0, display.width(), 16, TFT_BLUE);
                        display.setTextColor(TFT_WHITE, TFT_BLUE);
                        display.setCursor(4, 4);
                        display.print("Set RTC");
                        display.setTextColor(TFT_WHITE, TFT_BLACK);
                        display.setCursor(6, 30);
                        display.print("YYYYMMDDhhmmss");
                        display.setCursor(6, 48);
                        display.print(input);
                        display.setTextColor(TFT_YELLOW, TFT_BLACK);
                        display.setCursor(6, 70);
                        display.print("Enter=save Backspace=del");
                        display.setCursor(6, 84);
                        display.print("Esc=cancel");

                        M5Cardputer.update();
                        const char digit = cardputer_input_consume_digit();
                        if (digit != 0 && pos < 14) {
                            input[pos++] = digit;
                            input[pos] = '\0';
                        }
                        if (cardputer_input_consume(CARDPUTER_VK_BACKSPACE) && pos > 0) {
                            input[--pos] = '\0';
                        }
                        if (cardputer_input_consume(CARDPUTER_VK_ESC) ||
                            cardputer_input_consume_char('`')) break;
                        if (cardputer_input_consume(CARDPUTER_VK_ENTER)) {
                            CardputerRtcTime rtc_time;
                            if (cardputer_rtc_parse_yyyymmddhhmmss(input, &rtc_time) &&
                                cardputer_rtc_set(rtc_time)) {
                                show_probe_status("RTC updated", input);
                            } else {
                                show_probe_status("RTC update failed", "Use YYYYMMDDhhmmss");
                            }
                            delay(1500);
                            break;
                        }
                        delay(20);
                    }
                    break;
                }
                if (more_choice == 6) {
                    // 多系统：flash 上每张可引导 IMG 一项（cardputer86.img =
                    // 1.44MB MS-DOS 5.0，dos211.img = 360KB COMPAQ MS-DOS 2.11）。
                    // 选中后写进 NVS 偏好并**立刻换盘重启 guest**，不用断电；
                    // 下次开机也是这张（偏好优先级高于「上次启动的镜像」）。
                    const uint8_t total = cardputer_storage_boot_image_count();
                    if (total <= 1) {
                        show_probe_status("Only one boot image",
                                          "Add another .img under /flash");
                        delay(1600);
                        break;
                    }
                    const uint8_t listed = total < 9 ? total : 9;
                    static char boot_labels[9][32];
                    const char *boot_items[9];
                    for (uint8_t i = 0; i < listed; i++) {
                        snprintf(boot_labels[i], sizeof(boot_labels[i]), "%s",
                                 cardputer_storage_boot_image_label(i));
                        boot_items[i] = boot_labels[i];
                    }
                    uint8_t boot_selected = cardputer_storage_boot_image_pref();
                    if (boot_selected == 0xFF || boot_selected >= listed) {
                        boot_selected = 0;
                    }
                    const uint8_t pick = choose_menu("Boot system", boot_items,
                                                     listed, boot_selected, false);
                    if (pick != MENU_CANCEL) {
                        if (cardputer_storage_set_boot_image(pick, true)) {
                            show_probe_status("Boot system changed",
                                              cardputer_storage_boot_image_name(pick));
                            delay(1400);
                        } else {
                            show_probe_status("Boot image open failed",
                                              cardputer_storage_boot_image_name(pick));
                            delay(1800);
                        }
                    }
                    break;
                }
            }
            continue;
        }
        if (choice == 6) {
            // 蓝牙键盘模式：写 NVS("btkbd")，下次启动仍然有效。开启时立刻试一次
            // 运行期初始化；堆这时已被 guest 页缓存啃过，可能失败，失败就提示重启
            // （启动期 BLE 排在 CreateRAM 之前，那时堆连续块够大）。
            const bool enable = !cardputer_settings_bt_kbd_enabled();
            if (!cardputer_settings_set_bt_kbd_enabled(enable)) {
                show_probe_status("BT setting failed", "NVS write error");
                delay(1500);
                continue;
            }
            if (!enable) {
                cardputer_btkbd_stop();
                show_probe_status("BT keyboard off", "Keys back to DOS");
                delay(1200);
                continue;
            }
            if (cardputer_btkbd_start()) {
                // 直接离开设置菜单，让 loop() 接管 BT 模式并画出状态屏
                show_probe_status("BT keyboard on", "Exit: press b t n g o");
                delay(1200);
                return true;
            }
            show_probe_status("BT needs a restart", "Reboot to apply");
            delay(1800);
            continue;
        }
        return false;
    }
}
